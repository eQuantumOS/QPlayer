// full_state_gpu.cpp — FullStateGpuBackend: noiseless execute() + gate apply.
// Drives the GPU-resident SoA state vector through extern "C" launchers defined
// in full_state_gpu.cu. This .cpp never includes cuda_runtime.h (one-way include
// policy). adds: CUDA stream lifecycle, device-resident gate launchers,
// execute() fast path (gate apply on GPU, sampling on host), apply_gate().
// Dual-budget pre-check (VRAM ∧ host) and max_qubits() per
// CUDA Streams: single stream per backend instance for in-order async dispatch;
// gate kernels are serialised by the stream, cudaDeviceSynchronize before D2H
// transfer. No cross-stream parallelism — shared state vector precludes it.

#include "backend/full_state/full_state_backend.hpp"

#include <algorithm>    // std::upper_bound, std::fill
#include <chrono>       // std::chrono — QPLAYER_PROFILE_GPU section timing
#include <cmath>        // std::sin, std::cos, std::sqrt, M_SQRT1_2, std::isfinite, std::fma
#include <complex>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>       // std::shared_ptr — GPU-resident state owner
#include <numeric>      // std::partial_sum
#include <random>       // std::mt19937_64, std::uniform_real_distribution, std::random_device
#include <string>
#include <utility>      // std::pair, std::move
#include <variant>
#include <vector>

#include "backend/full_state/gate_matrix.hpp"  // ecr/iswap/sqrt_iswap/fsim matrix data (0610)
#include "backend/full_state/kraus_apply_2q.hpp"  // detail::apply_kraus_2q
#include "backend/memory_check.hpp"
#include "circuit/circuit.hpp"
#include "detail/rng.hpp"
#include "error.hpp"
#include "log.hpp"      // qplayer::log — QPLAYER_PROFILE_GPU diagnostics
#include "noise/noise_model.hpp"  // KrausChannel, for GPU noisy execute()

#if defined(QPLAYER_BUILD_GPU)
// Launchers defined in full_state_gpu.cu. cudaError_t returned as int (0=success).
extern "C" {
int  qplayer_cuda_mem_get_info(unsigned long long* free_out,
                               unsigned long long* total_out);
int  qplayer_cuda_alloc_state(double** d_re, double** d_im,
                               unsigned long long dim);
void qplayer_cuda_free_state(double* d_re, double* d_im);
int  qplayer_cuda_init_zero_state(double* d_re, double* d_im,
                                   unsigned long long dim);
int  qplayer_cuda_alloc_pinned(double** h_re, double** h_im,
                                unsigned long long dim);
void qplayer_cuda_free_pinned(double* h_re, double* h_im);
int  qplayer_cuda_download_state(double* h_re, double* h_im,
                                  const double* d_re, const double* d_im,
                                  unsigned long long dim);
// : stream lifecycle and device-resident gate launchers.
int  qplayer_cuda_stream_create(unsigned long long* handle_out);
void qplayer_cuda_stream_destroy(unsigned long long handle);
int  qplayer_cuda_stream_sync(unsigned long long handle);
int  qplayer_cuda_apply_1q_dev(double* d_re, double* d_im,
                                unsigned long long dim,
                                unsigned int target,
                                const double* u8,
                                unsigned long long stream_handle);
int  qplayer_cuda_apply_2q_dev(double* d_re, double* d_im,
                                unsigned long long dim,
                                unsigned int control,
                                unsigned int target,
                                const double* u8,
                                unsigned long long stream_handle);
int  qplayer_cuda_apply_swap_dev(double* d_re, double* d_im,
                                  unsigned long long dim,
                                  unsigned int q_a, unsigned int q_b,
                                  unsigned int control_bit,
                                  unsigned long long stream_handle);
int  qplayer_cuda_apply_ccx_dev(double* d_re, double* d_im,
                                 unsigned long long dim,
                                 unsigned int c0, unsigned int c1,
                                 unsigned int target,
                                 unsigned long long stream_handle);
// : dense 4x4 two-qubit gate (ECR). m32 = 32 doubles, row-major
// operand-order (qa=operand0/bit0, qb=operand1/bit1); see ecr_matrix_data().
int  qplayer_cuda_apply_dense2q_dev(double* d_re, double* d_im,
                                     unsigned long long dim,
                                     unsigned int qa, unsigned int qb,
                                     const double* m32,
                                     unsigned long long stream_handle);
int  qplayer_cuda_download_state_stream(double* h_re, double* h_im,
                                         const double* d_re, const double* d_im,
                                         unsigned long long dim,
                                         unsigned long long stream_handle);
// : GPU Monte Carlo trajectory cuRAND state init. states_out is
// an opaque device pointer (curandStatePhilox4_32_10 array) kept off this TU.
int  qplayer_cuda_init_curand_states(void** states_out,
                                     const unsigned long long* h_seeds,
                                     int n_traj);
void qplayer_cuda_free_curand_states(void* states);
// : H2D upload for Kraus CPU-apply-then-upload trajectory pattern.
// Copies host pinned buffers h_re/h_im to already-allocated device buffers d_re/d_im.
int  qplayer_cuda_upload_state(const double* h_re, const double* h_im,
                               double* d_re, double* d_im,
                               unsigned long long dim,
                               unsigned long long stream_handle);
}  // extern "C"
#endif  // QPLAYER_BUILD_GPU

namespace qplayer {

namespace {

// kPhase tag for stub methods that remain NotImplemented in.
constexpr const char* kPhase = "Phase 6 pending";

// Section profiler for the GPU execute()/sampling breakdown.
// Gated on QPLAYER_PROFILE_GPU so it costs nothing in production; emits one
// log::info line per timed region. Kept as a permanent opt-in so can
// re-measure the GPU-resident sampling improvement against this baseline.
class GpuSectionTimer {
public:
    explicit GpuSectionTimer(const char* tag)
        : tag_(tag), on_(std::getenv("QPLAYER_PROFILE_GPU") != nullptr) {
        if (on_) mark_ = std::chrono::steady_clock::now();
    }
    void lap(const char* section) {
        if (!on_) return;
        const auto now = std::chrono::steady_clock::now();
        const double ms =
            std::chrono::duration<double, std::milli>(now - mark_).count();
        mark_ = now;
        log::info(std::string{"[profile-gpu] "} + tag_ + " " + section + " = " +
                  std::to_string(ms) + " ms");
    }

private:
    const char*                           tag_;
    bool                                  on_;
    std::chrono::steady_clock::time_point mark_{};
};

#if defined(QPLAYER_BUILD_GPU)

// Sentinel: no control qubit for unconditional SWAP. Must be outside [0,127].
constexpr unsigned int kNoControlBit = 0xFFFFFFFFU;

// ── 2x2 complex gate matrix helper ────────────────────────────────────────────
// Mirrors full_state_cpu.cpp's anonymous-namespace gate_matrix(); reproduced here
// because the CPU version is not accessible from this TU. Provides the u8 array
// {u00r,u00i, u01r,u01i, u10r,u10i, u11r,u11i} consumed by the .cu launchers.
struct GpuMat2x2 {
    double u00r, u00i;
    double u01r, u01i;
    double u10r, u10i;
    double u11r, u11i;
};

GpuMat2x2 gpu_gate_matrix(GateType type, const std::vector<double>& params) {
    const double INV_SQRT2 = M_SQRT1_2;

    switch (type) {
        case GateType::I:   return {1,0, 0,0, 0,0, 1,0};
        case GateType::X:   return {0,0, 1,0, 1,0, 0,0};
        case GateType::Y:   return {0,0, 0,-1, 0,1, 0,0};
        case GateType::Z:   return {1,0, 0,0, 0,0, -1,0};
        case GateType::H:   return {INV_SQRT2,0, INV_SQRT2,0, INV_SQRT2,0, -INV_SQRT2,0};
        case GateType::S:   return {1,0, 0,0, 0,0, 0,1};
        case GateType::Sdg: return {1,0, 0,0, 0,0, 0,-1};
        case GateType::T:   return {1,0, 0,0, 0,0, INV_SQRT2, INV_SQRT2};
        case GateType::Tdg: return {1,0, 0,0, 0,0, INV_SQRT2, -INV_SQRT2};
        // SX = sqrt(X), SXdg = SX dagger.
        case GateType::SX:   return {0.5,0.5, 0.5,-0.5, 0.5,-0.5, 0.5,0.5};
        case GateType::SXdg: return {0.5,-0.5, 0.5,0.5, 0.5,0.5, 0.5,-0.5};

        case GateType::RX: {
            const double t = params[0];
            const double c = std::cos(t * 0.5), s = std::sin(t * 0.5);
            return {c,0, 0,-s, 0,-s, c,0};
        }
        case GateType::RY: {
            const double t = params[0];
            const double c = std::cos(t * 0.5), s = std::sin(t * 0.5);
            return {c,0, -s,0, s,0, c,0};
        }
        case GateType::RZ: {
            const double t = params[0];
            const double c = std::cos(t * 0.5), s = std::sin(t * 0.5);
            return {c,-s, 0,0, 0,0, c,s};
        }
        case GateType::U1: {
            const double lam = params[0];
            return {1,0, 0,0, 0,0, std::cos(lam), std::sin(lam)};
        }
        case GateType::U2: {
            const double phi = params[0], lam = params[1];
            const double cp  = std::cos(phi), sp  = std::sin(phi);
            const double cl  = std::cos(lam), sl  = std::sin(lam);
            const double cpl = std::cos(phi+lam), spl = std::sin(phi+lam);
            return {INV_SQRT2,0,
                    -INV_SQRT2*cl, -INV_SQRT2*sl,
                     INV_SQRT2*cp,  INV_SQRT2*sp,
                     INV_SQRT2*cpl, INV_SQRT2*spl};
        }
        case GateType::U3: {
            const double th = params[0], phi = params[1], lam = params[2];
            const double ct = std::cos(th*0.5), st = std::sin(th*0.5);
            const double cp = std::cos(phi), sp = std::sin(phi);
            const double cl = std::cos(lam), sl = std::sin(lam);
            const double cpl = std::cos(phi+lam), spl = std::sin(phi+lam);
            return {ct, 0, -cl*st, -sl*st, cp*st, sp*st, cpl*ct, spl*ct};
        }
        // Controlled gates: return the target sub-unitary.
        case GateType::CX:  return {0,0, 1,0, 1,0, 0,0};
        case GateType::CY:  return {0,0, 0,-1, 0,1, 0,0};
        case GateType::CZ:  return {1,0, 0,0, 0,0, -1,0};
        case GateType::CH:  return {INV_SQRT2,0, INV_SQRT2,0, INV_SQRT2,0, -INV_SQRT2,0};
        case GateType::CRZ: {
            const double lam = params[0];
            const double c = std::cos(lam*0.5), s = std::sin(lam*0.5);
            return {c,-s, 0,0, 0,0, c,s};
        }
        case GateType::CU1: {
            const double lam = params[0];
            return {1,0, 0,0, 0,0, std::cos(lam), std::sin(lam)};
        }
        case GateType::CU3: {
            const double th = params[0], phi = params[1], lam = params[2];
            const double ct = std::cos(th*0.5), st = std::sin(th*0.5);
            const double cp = std::cos(phi), sp = std::sin(phi);
            const double cl = std::cos(lam), sl = std::sin(lam);
            const double cpl = std::cos(phi+lam), spl = std::sin(phi+lam);
            return {ct, 0, -cl*st, -sl*st, cp*st, sp*st, cpl*ct, spl*ct};
        }
        case GateType::CCX: return {0,0, 1,0, 1,0, 0,0};  // target sub-unitary = X
        default:
            // SWAP / CSwap / Reset / Opaque dispatched separately.
            return {1,0, 0,0, 0,0, 1,0};
    }
}

void pack_u8(const GpuMat2x2& m, double* u8) noexcept {
    u8[0] = m.u00r; u8[1] = m.u00i;
    u8[2] = m.u01r; u8[3] = m.u01i;
    u8[4] = m.u10r; u8[5] = m.u10i;
    u8[6] = m.u11r; u8[7] = m.u11i;
}

// GPU VRAM budget. Returns UINT64_MAX when query fails so the
// precheck is disabled and defers to the post-hoc GPUError path.
std::uint64_t gpu_available_bytes() noexcept {
    constexpr std::uint64_t kBytesPerGiB = std::uint64_t{1} << 30;
    constexpr std::uint64_t kNoLimit     = UINT64_MAX;
    if (const char* raw = std::getenv("QPLAYER_GPU_MEMORY_BUDGET_GB")) {
        try {
            const double gb = std::stod(raw);
            if (gb >= 0.0 && gb <= 1e12) {
                return static_cast<std::uint64_t>(
                    gb * static_cast<double>(kBytesPerGiB));
            }
        } catch (...) {}  // fall through to cudaMemGetInfo
    }
    unsigned long long free_b = 0, total_b = 0;
    if (qplayer_cuda_mem_get_info(&free_b, &total_b) != 0) {
        return kNoLimit;
    }
    return static_cast<std::uint64_t>(static_cast<double>(free_b) * 0.8);
}

// Shared owner of the GPU-resident SoA buffers. A std::shared_ptr to
// this holder is shared with the Result from execute(); when the last reference
// drops (backend re-init AND all Results released) the device buffers are freed.
// This lets a Result keep |psi> on the device for deferred sampling / lazy D2H
// after the backend's next run() allocates a fresh buffer.
struct GpuStateBuffers {
    double* d_re;
    double* d_im;
    GpuStateBuffers(double* re, double* im) : d_re(re), d_im(im) {}
    ~GpuStateBuffers() { qplayer_cuda_free_state(d_re, d_im); }
    // Non-copyable: a copy would free the same device buffers twice. Construct
    // in place via make_shared(re, im) — never from a temporary (that temporary's
    // destructor would free the live buffers mid-circuit).
    GpuStateBuffers(const GpuStateBuffers&)            = delete;
    GpuStateBuffers& operator=(const GpuStateBuffers&) = delete;
};

// ── apply_kraus_channel_host — Kraus channel application on host pinned buffer ──
// Applies one KrausChannel to an SoA state vector on the host (pinned memory).
// Same Monte Carlo algorithm as the CPU trajectory path:
//   1. Compute p_i = ||K_i|ψ⟩||² for each operator.
//   2. Draw r ~ Uniform[0,1) and pick the operator by cumulative probability.
//   3. Apply chosen K to target_qubit, renormalise.
// Single-qubit channels only (2x2 row-major complex). Used after D2H download
// before the modified state is re-uploaded (H2D) to the device (order).
static void apply_kraus_channel_host(
        double* re, double* im, unsigned long long dim,
        const KrausChannel& ch,
        std::mt19937_64& rng)
{
    // Two-qubit (4x4) channels: shared routine, identical to the CPU path.
    if (!ch.operators.empty() && ch.operators.front().size() == 4) {
        qplayer::detail::apply_kraus_2q(re, im, static_cast<StateIndex>(dim),
                                        ch.target_qubit, ch.target_qubit2,
                                        ch.operators, rng);
        return;
    }

    const unsigned int tgt = static_cast<unsigned int>(ch.target_qubit);
    const std::size_t nops = ch.operators.size();
    const unsigned long long tbit = 1ULL << tgt;
    const unsigned long long half_dim = dim >> 1;

    // Step 1: probabilities.
    std::vector<double> probs(nops, 0.0);
    for (std::size_t ki = 0; ki < nops; ++ki) {
        const auto& K = ch.operators[ki];
        const double k00r = K[0][0].real(), k00i = K[0][0].imag();
        const double k01r = K[0][1].real(), k01i = K[0][1].imag();
        const double k10r = K[1][0].real(), k10i = K[1][0].imag();
        const double k11r = K[1][1].real(), k11i = K[1][1].imag();

        double pi = 0.0;
        for (unsigned long long jj = 0; jj < half_dim; ++jj) {
            const unsigned long long i0 = ((jj >> tgt) << (tgt + 1)) | (jj & (tbit - 1));
            const unsigned long long i1 = i0 | tbit;
            const double a0r = re[i0], a0i = im[i0];
            const double a1r = re[i1], a1i = im[i1];
            const double n0r = k00r*a0r - k00i*a0i + k01r*a1r - k01i*a1i;
            const double n0i = k00r*a0i + k00i*a0r + k01r*a1i + k01i*a1r;
            const double n1r = k10r*a0r - k10i*a0i + k11r*a1r - k11i*a1i;
            const double n1i = k10r*a0i + k10i*a0r + k11r*a1i + k11i*a1r;
            pi += std::fma(n0r, n0r, n0i*n0i) + std::fma(n1r, n1r, n1i*n1i);
        }
        probs[ki] = pi;
    }

    // Step 2-3: pick operator.
    std::uniform_real_distribution<double> dist(0.0, 1.0);
    const double u = dist(rng);
    double cumsum = 0.0;
    std::size_t chosen = 0;
    for (std::size_t ki = 0; ki < nops; ++ki) {
        cumsum += probs[ki];
        if (u < cumsum) { chosen = ki; break; }
        chosen = ki;
    }

    const auto& Kc = ch.operators[chosen];
    const double k00r = Kc[0][0].real(), k00i = Kc[0][0].imag();
    const double k01r = Kc[0][1].real(), k01i = Kc[0][1].imag();
    const double k10r = Kc[1][0].real(), k10i = Kc[1][0].imag();
    const double k11r = Kc[1][1].real(), k11i = Kc[1][1].imag();

    for (unsigned long long jj = 0; jj < half_dim; ++jj) {
        const unsigned long long i0 = ((jj >> tgt) << (tgt + 1)) | (jj & (tbit - 1));
        const unsigned long long i1 = i0 | tbit;
        const double a0r = re[i0], a0i = im[i0];
        const double a1r = re[i1], a1i = im[i1];
        re[i0] = k00r*a0r - k00i*a0i + k01r*a1r - k01i*a1i;
        im[i0] = k00r*a0i + k00i*a0r + k01r*a1i + k01i*a1r;
        re[i1] = k10r*a0r - k10i*a0i + k11r*a1r - k11i*a1i;
        im[i1] = k10r*a0i + k10i*a0r + k11r*a1i + k11i*a1r;
    }

    const double p_chosen = probs[chosen];
    if (p_chosen < 1e-15) return;
    const double inv_norm = 1.0 / std::sqrt(p_chosen);
    for (unsigned long long ii = 0; ii < dim; ++ii) {
        re[ii] *= inv_norm;
        im[ii] *= inv_norm;
    }
}

#endif  // QPLAYER_BUILD_GPU

}  // anonymous namespace

// ── GPU build ─────────────────────────────────────────────────────────────────

#if defined(QPLAYER_BUILD_GPU)

FullStateGpuBackend::FullStateGpuBackend() = default;

FullStateGpuBackend::~FullStateGpuBackend() {
    // Destroy stream first so any in-flight work completes before the device
    // buffers are freed (stream lifecycle).
    qplayer_cuda_stream_destroy(stream_handle_);
    // Device buffers are freed by state_owner_'s deleter when its last reference
    // drops — a Result from execute() may still hold a reference, so
    // we do NOT free d_re_/d_im_ directly here.
    state_owner_.reset();
}

// prepare() — dual-budget memory pre-check (VRAM ∧ host) then init_state
// VRAM term = peak Execute working set (2× base SoA);
// host term = Materialize (pinned buffer + AoS copy, 2^n × 16 bytes).
// A UINT64_MAX budget disables that term (query failed).
void FullStateGpuBackend::prepare(const Circuit& circuit) {
    const QubitIndex n = circuit.num_qubits();
    const std::uint64_t vram_need =
        detail::peak_state_bytes(n, detail::WorkingSet::Execute);
    const std::uint64_t host_need =
        detail::peak_state_bytes(n, detail::WorkingSet::Materialize);
    const std::uint64_t vram_budget = gpu_available_bytes();
    const std::uint64_t host_budget = detail::available_memory_bytes();

    if (vram_need > vram_budget || host_need > host_budget) {
        throw GPUError{
            0,
            "FullStateGPU OOM precheck failed (n_qubits = " +
                std::to_string(n) + "): VRAM needs " +
                std::to_string(vram_need) + " B / budget " +
                std::to_string(vram_budget) + " B, host needs " +
                std::to_string(host_need) + " B / budget " +
                std::to_string(host_budget) +
                " B; set QPLAYER_GPU_MEMORY_BUDGET_GB to relax"};
    }
    init_state(n);
}

// init_state() — (re-)allocate the GPU-resident SoA state vector initialised to
// |0...0>.  Destroys and recreates the CUDA stream so in-flight kernels from a
// previous circuit finish before the new allocation begins.
void FullStateGpuBackend::init_state(QubitIndex num_qubits) {
    // Retire old stream (waits for in-flight work implicitly via cudaStreamDestroy).
    qplayer_cuda_stream_destroy(stream_handle_);
    stream_handle_ = 0;

    // Drop this backend's reference to the previous buffer. The
    // device memory is freed only if no Result still holds it; otherwise it
    // survives for that Result's deferred sampling / lazy D2H.
    state_owner_.reset();
    d_re_ = nullptr;
    d_im_ = nullptr;

    num_qubits_ = num_qubits;
    dim_        = StateIndex{1} << num_qubits;
    const unsigned long long dim = static_cast<unsigned long long>(dim_);

    // Create the per-backend stream (: single stream, in-order dispatch).
    int rc = qplayer_cuda_stream_create(&stream_handle_);
    if (rc != 0) {
        throw GPUError{rc, "cudaStreamCreate failed"};
    }

    rc = qplayer_cuda_alloc_state(&d_re_, &d_im_, dim);
    if (rc != 0) {
        qplayer_cuda_stream_destroy(stream_handle_);
        stream_handle_ = 0;
        throw GPUError{rc, "cudaMalloc GPU-resident state vector failed"};
    }

    rc = qplayer_cuda_init_zero_state(d_re_, d_im_, dim);
    if (rc != 0) {
        qplayer_cuda_free_state(d_re_, d_im_);
        d_re_ = nullptr;
        d_im_ = nullptr;
        qplayer_cuda_stream_destroy(stream_handle_);
        stream_handle_ = 0;
        throw GPUError{rc, "device |0...0> initialisation failed"};
    }

    // Take shared ownership of the freshly allocated buffers. From
    // here, d_re_/d_im_ are cached pointers into state_owner_'s managed memory;
    // the GpuStateBuffers destructor calls qplayer_cuda_free_state on last drop.
    state_owner_ = std::make_shared<GpuStateBuffers>(d_re_, d_im_);
}

// get_statevector() — single device->host copy policy.
// Pinned host buffer allocated, stream synchronised, state downloaded, AoS
// materialised, then the pinned buffer is freed.
StateVector FullStateGpuBackend::get_statevector() {
    if (d_re_ == nullptr) {
        throw SimulatorError{"FullStateGPU", "get_statevector before init_state"};
    }
    detail::check_memory_budget(
        detail::peak_state_bytes(num_qubits_, detail::WorkingSet::Materialize),
        "FullStateGPU", num_qubits_);

    const unsigned long long dim = static_cast<unsigned long long>(dim_);
    double* h_re = nullptr;
    double* h_im = nullptr;
    int rc = qplayer_cuda_alloc_pinned(&h_re, &h_im, dim);
    if (rc != 0) {
        throw GPUError{rc, "cudaMallocHost pinned transfer buffer failed"};
    }
    // Use the stream-aware download to flush all queued kernels before copy.
    rc = qplayer_cuda_download_state_stream(h_re, h_im, d_re_, d_im_, dim,
                                            stream_handle_);
    if (rc != 0) {
        qplayer_cuda_free_pinned(h_re, h_im);
        throw GPUError{rc, "device->host state copy failed"};
    }

    StateVector out(static_cast<std::size_t>(dim_));
    for (std::size_t i = 0; i < static_cast<std::size_t>(dim_); ++i) {
        out[i] = std::complex<double>(h_re[i], h_im[i]);
    }
    qplayer_cuda_free_pinned(h_re, h_im);
    return out;
}

// max_qubits() — VRAM ∧ host dual-budget model.
std::uint32_t FullStateGpuBackend::max_qubits(detail::WorkingSet ws) const noexcept {
    const std::uint64_t vram_budget = gpu_available_bytes();
    const std::uint64_t host_budget = detail::available_memory_bytes();
    std::uint32_t best = 0;
    for (std::uint32_t n = 1; n < 59; ++n) {
        const std::uint64_t vram_need =
            detail::peak_state_bytes(static_cast<QubitIndex>(n), ws);
        const std::uint64_t host_need = detail::peak_state_bytes(
            static_cast<QubitIndex>(n), detail::WorkingSet::Materialize);
        if (vram_need <= vram_budget && host_need <= host_budget) {
            best = n;
        } else {
            break;
        }
    }
    return best;
}

// set_seed() — store the master PRNG seed for reproducible sampling.
// Mirrors the CPU backend override; the seed is consumed in execute().
void FullStateGpuBackend::set_seed(std::uint64_t seed) {
    master_seed_ = seed;
}

// apply_gate() — dispatch a GateOp to the GPU-resident state vector via the
// device-resident launchers. All kernels are submitted to stream_handle_
// for in-order async execution; correctness is guaranteed by stream serialisation
// without extra synchronisation between consecutive gates.
void FullStateGpuBackend::apply_gate(const GateOp& op) {
    if (d_re_ == nullptr) {
        throw SimulatorError{"FullStateGPU", "apply_gate called before init_state"};
    }

    const GateType type = op.type;

    if (type == GateType::Reset) {
        throw NotImplementedError{"FullStateGPU",
            "Reset gate in apply_gate not supported (use execute() per-shot path)"};
    }
    if (type == GateType::Opaque) {
        throw NotImplementedError{"FullStateGPU", "Opaque gate not yet supported"};
    }

    const unsigned long long dim = static_cast<unsigned long long>(dim_);
    const unsigned long long sh  = stream_handle_;

    // SWAP and CSwap dispatch directly to the SWAP kernel.
    if (type == GateType::SWAP) {
        const int rc = qplayer_cuda_apply_swap_dev(
            d_re_, d_im_, dim,
            static_cast<unsigned int>(op.qubits[0]),
            static_cast<unsigned int>(op.qubits[1]),
            kNoControlBit, sh);
        if (rc != 0) throw GPUError{rc, "SWAP kernel launch failed"};
        return;
    }
    if (type == GateType::CSwap) {
        // Fredkin: control=qubits[0], swap qubits[1] and qubits[2].
        const int rc = qplayer_cuda_apply_swap_dev(
            d_re_, d_im_, dim,
            static_cast<unsigned int>(op.qubits[1]),
            static_cast<unsigned int>(op.qubits[2]),
            static_cast<unsigned int>(op.qubits[0]),   // control bit
            sh);
        if (rc != 0) throw GPUError{rc, "CSwap kernel launch failed"};
        return;
    }
    // ECR: genuine 4x4, applied via the dense-2Q kernel.
    if (type == GateType::ECR) {
        const int rc = qplayer_cuda_apply_dense2q_dev(
            d_re_, d_im_, dim,
            static_cast<unsigned int>(op.qubits[0]),   // operand 0 (bit 0)
            static_cast<unsigned int>(op.qubits[1]),   // operand 1 (bit 1)
            detail::ecr_matrix_data().data(), sh);
        if (rc != 0) throw GPUError{rc, "ECR dense-2Q kernel launch failed"};
        return;
    }
    // Google native 2Q gates: genuine 4x4 via the dense-2Q kernel.
    if (type == GateType::ISWAP) {
        const int rc = qplayer_cuda_apply_dense2q_dev(
            d_re_, d_im_, dim,
            static_cast<unsigned int>(op.qubits[0]),
            static_cast<unsigned int>(op.qubits[1]),
            detail::iswap_matrix_data().data(), sh);
        if (rc != 0) throw GPUError{rc, "iSWAP dense-2Q kernel launch failed"};
        return;
    }
    if (type == GateType::SQRT_ISWAP) {
        const int rc = qplayer_cuda_apply_dense2q_dev(
            d_re_, d_im_, dim,
            static_cast<unsigned int>(op.qubits[0]),
            static_cast<unsigned int>(op.qubits[1]),
            detail::sqrt_iswap_matrix_data().data(), sh);
        if (rc != 0) throw GPUError{rc, "sqrt_iSWAP dense-2Q kernel launch failed"};
        return;
    }
    if (type == GateType::FSIM) {
        // fSim is parametric: compute matrix from params[0]=theta, params[1]=phi.
        const auto m = detail::fsim_matrix_data(op.params[0], op.params[1]);
        const int rc = qplayer_cuda_apply_dense2q_dev(
            d_re_, d_im_, dim,
            static_cast<unsigned int>(op.qubits[0]),
            static_cast<unsigned int>(op.qubits[1]),
            m.data(), sh);
        if (rc != 0) throw GPUError{rc, "fSim dense-2Q kernel launch failed"};
        return;
    }

    // All other gate types reduce to controlled-1Q via the 2x2 sub-unitary.
    double u8[8];
    pack_u8(gpu_gate_matrix(type, op.params), u8);

    // Determine control qubit(s) and target qubit, mirroring CPU apply_gate.
    switch (type) {
        // Pure 1Q gates: control_mask = 0, target = qubits[0].
        case GateType::I:
        case GateType::X:
        case GateType::Y:
        case GateType::Z:
        case GateType::H:
        case GateType::S:
        case GateType::Sdg:
        case GateType::T:
        case GateType::Tdg:
        case GateType::SX:    //
        case GateType::SXdg:  //
        case GateType::RX:
        case GateType::RY:
        case GateType::RZ:
        case GateType::U1:
        case GateType::U2:
        case GateType::U3: {
            const int rc = qplayer_cuda_apply_1q_dev(
                d_re_, d_im_, dim,
                static_cast<unsigned int>(op.qubits[0]),
                u8, sh);
            if (rc != 0) throw GPUError{rc, "1Q gate kernel launch failed"};
            return;
        }

        // 2Q controlled gates: qubits[0]=control, qubits[1]=target.
        case GateType::CX:
        case GateType::CY:
        case GateType::CZ:
        case GateType::CH:
        case GateType::CRZ:
        case GateType::CU1:
        case GateType::CU3: {
            const int rc = qplayer_cuda_apply_2q_dev(
                d_re_, d_im_, dim,
                static_cast<unsigned int>(op.qubits[0]),   // control
                static_cast<unsigned int>(op.qubits[1]),   // target
                u8, sh);
            if (rc != 0) throw GPUError{rc, "2Q controlled gate kernel launch failed"};
            return;
        }

        // CCX (Toffoli): qubits[0]+qubits[1] control, qubits[2]=target. A single
        // exact controlled-controlled-X kernel (flips target where both controls
        // are 1), matching the CPU control_mask path. Replaces a prior H/T/CX
        // decomposition that dropped one T on the target and produced a wrong
        // (non-unitary on the firing subspace) result on GPU.
        case GateType::CCX: {
            const int rc = qplayer_cuda_apply_ccx_dev(
                d_re_, d_im_, dim,
                static_cast<unsigned int>(op.qubits[0]),   // control 0
                static_cast<unsigned int>(op.qubits[1]),   // control 1
                static_cast<unsigned int>(op.qubits[2]),   // target
                sh);
            if (rc != 0) throw GPUError{rc, "CCX kernel launch failed"};
            return;
        }

        default:
            throw SimulatorError{"FullStateGPU",
                "apply_gate: unsupported gate type"};
    }
}

// execute() — fast path for noiseless circuits with no mid-circuit
// measurement (no IfOp). Matches the CPU execute() algorithm exactly:
// 1. prepare() (precheck + init_state to GPU |0...0>)
// 2. Apply gate ops on the GPU-resident state (apply_gate via device launchers)
// 3. cudaDeviceSynchronize
// 4. D2H download to pinned host buffer
// 5. Build prob vector, CDF, nshots inverse-CDF sampling on host (same as CPU)
// per-shot path (IfOp / mid-circuit collapse) is not implemented in.
Result FullStateGpuBackend::execute(const Circuit& circuit,
                                    std::uint32_t  nshots,
                                    const NoiseModel* noise) {
    // ── Noisy path: per-trajectory Monte Carlo ─────────
    // GPU-accelerated gate application + CPU-side Kraus channel application.
    // Strategy: for each trajectory, keep the state on the GPU for gate kernels;
    // when a gate has noise channels, D2H → apply Kraus on CPU → H2D.
    // This gives GPU acceleration for gate application while keeping Kraus
    // application simple (no new GPU kernel required for arbitrary 2x2 Kraus).
    // Each trajectory runs sequentially (nshots loop on host); trajectories are
    // independent so OpenMP parallelism can be added in if needed.
    if (noise != nullptr) {
        // Memory precheck + GPU |0...0> initialisation (same as noiseless path).
        // prepare() allocates/resets the GPU state vector and creates the stream.
        prepare(circuit);

        const ClassicalBitIndex total_c = circuit.total_cubits();
        const auto& cregs = circuit.classical_registers();

        // Readout error gate: skip the per-measure lookup
        // when none are registered. Same helper as the CPU path -> parity.
        const bool readout_active = noise->has_readout_errors();

        const std::uint64_t mseed =
            master_seed_ != 0
                ? master_seed_
                : static_cast<std::uint64_t>(std::random_device{}());

        std::map<std::string, std::uint64_t> counts;
        const unsigned long long dim_ull = static_cast<unsigned long long>(dim_);
        const std::size_t dimz = static_cast<std::size_t>(dim_);

        // Allocate one pair of pinned host buffers for D2H/H2D transfers
        // (reused across all trajectories — no repeated alloc/free overhead,
        // pinned-memory policy).
        double* h_re = nullptr;
        double* h_im = nullptr;
        int rc = qplayer_cuda_alloc_pinned(&h_re, &h_im, dim_ull);
        if (rc != 0) {
            throw GPUError{rc, "noisy execute(): cudaMallocHost pinned buffer failed"};
        }

        // Scope-guard to free pinned memory on any exit path.
        struct PinnedGuard {
            double* re; double* im;
            ~PinnedGuard() { qplayer_cuda_free_pinned(re, im); }
        } pinned_guard{h_re, h_im};

        for (std::uint32_t traj = 0; traj < nshots; ++traj) {
            // (Re)initialise GPU state to |0...0> for each trajectory.
            rc = qplayer_cuda_init_zero_state(d_re_, d_im_, dim_ull);
            if (rc != 0) {
                throw GPUError{rc, "noisy execute(): GPU state reset failed at trajectory " +
                                       std::to_string(traj)};
            }

            // Per-trajectory RNG seeded reproducibly.
            const std::uint64_t traj_seed =
                (mseed != 0)
                    ? qplayer::detail::thread_seed(
                          mseed, static_cast<std::uint32_t>(traj))
                    : static_cast<std::uint64_t>(std::random_device{}());
            std::mt19937_64 traj_rng(traj_seed);

            std::vector<int> cubit_vals(static_cast<std::size_t>(total_c), 0);
            bool need_kraus_download = false;  // defer D2H until Kraus needed

            for (const Op& op : circuit.ops()) {
                std::visit([&](auto&& node) {
                    using T = std::decay_t<decltype(node)>;

                    if constexpr (std::is_same_v<T, GateOp>) {
                        if (node.type == GateType::Reset) {
                            throw NotImplementedError{"FullStateGPU",
                                "Reset gate in GPU noisy execute() not supported"};
                        } else if (node.type == GateType::Opaque) {
                            throw NotImplementedError{"FullStateGPU",
                                "Opaque gate in GPU noisy execute() not supported"};
                        }

                        // Apply gate on GPU (existing device-resident path).
                        apply_gate(node);

                        // Apply noise channels (D2H → CPU Kraus → H2D) if needed.
                        if (noise->has_channels_for_gate(node.name)) {
                            // Sync stream so gate is complete before D2H.
                            const int sync_rc = qplayer_cuda_stream_sync(stream_handle_);
                            if (sync_rc != 0) {
                                throw GPUError{sync_rc,
                                    "noisy execute(): stream sync before Kraus D2H failed"};
                            }
                            // D2H: download GPU state to pinned host buffers.
                            rc = qplayer_cuda_download_state_stream(
                                h_re, h_im, d_re_, d_im_, dim_ull, stream_handle_);
                            if (rc != 0) {
                                throw GPUError{rc,
                                    "noisy execute(): D2H for Kraus application failed"};
                            }
                            // Apply Kraus channels on host.
                            auto chans = noise->channels_for_gate(node.name, node.qubits);
                            for (const KrausChannel& ch : chans) {
                                apply_kraus_channel_host(
                                    h_re, h_im, dim_ull, ch, traj_rng);
                            }
                            // H2D: upload modified state back to GPU.
                            rc = qplayer_cuda_upload_state(
                                h_re, h_im, d_re_, d_im_, dim_ull, stream_handle_);
                            if (rc != 0) {
                                throw GPUError{rc,
                                    "noisy execute(): H2D after Kraus application failed"};
                            }
                            need_kraus_download = false;
                        }
                        need_kraus_download = true;  // state changed on GPU

                    } else if constexpr (std::is_same_v<T, IdleOp>) {
                        // Apply idle relaxation Kraus channels.
                        auto chans = noise->idle_channels(node.qubit, node.duration_ns);
                        if (!chans.empty()) {
                            // Sync and D2H before applying idle Kraus.
                            const int sync_rc = qplayer_cuda_stream_sync(stream_handle_);
                            if (sync_rc != 0) {
                                throw GPUError{sync_rc,
                                    "noisy execute(): stream sync before idle Kraus D2H failed"};
                            }
                            rc = qplayer_cuda_download_state_stream(
                                h_re, h_im, d_re_, d_im_, dim_ull, stream_handle_);
                            if (rc != 0) {
                                throw GPUError{rc,
                                    "noisy execute(): D2H for idle Kraus application failed"};
                            }
                            for (const KrausChannel& ch : chans) {
                                apply_kraus_channel_host(
                                    h_re, h_im, dim_ull, ch, traj_rng);
                            }
                            rc = qplayer_cuda_upload_state(
                                h_re, h_im, d_re_, d_im_, dim_ull, stream_handle_);
                            if (rc != 0) {
                                throw GPUError{rc,
                                    "noisy execute(): H2D after idle Kraus application failed"};
                            }
                        }

                    } else if constexpr (std::is_same_v<T, MeasureOp>) {
                        // D2H if needed to compute Born-rule probabilities on host.
                        if (need_kraus_download) {
                            const int sync_rc = qplayer_cuda_stream_sync(stream_handle_);
                            if (sync_rc != 0) {
                                throw GPUError{sync_rc,
                                    "noisy execute(): stream sync before measure D2H failed"};
                            }
                            rc = qplayer_cuda_download_state_stream(
                                h_re, h_im, d_re_, d_im_, dim_ull, stream_handle_);
                            if (rc != 0) {
                                throw GPUError{rc,
                                    "noisy execute(): D2H for measure failed"};
                            }
                            need_kraus_download = false;
                        }
                        // Born-rule collapse on host.
                        double p1 = 0.0;
                        for (std::size_t ii = 0; ii < dimz; ++ii) {
                            const unsigned long long i = static_cast<unsigned long long>(ii);
                            if ((i >> node.qubit) & 1ULL) {
                                p1 += std::fma(h_re[ii], h_re[ii], h_im[ii]*h_im[ii]);
                            }
                        }
                        std::uniform_real_distribution<double> u01(0.0, 1.0);
                        const int outcome = (u01(traj_rng) < p1) ? 1 : 0;
                        const double p_out = (outcome == 1) ? p1 : (1.0 - p1);
                        if (p_out < 1e-15) {
                            throw SimulatorError{"FullStateGPU",
                                "trajectory " + std::to_string(traj) +
                                ": measure p_outcome near zero"};
                        }
                        const double inv = 1.0 / std::sqrt(p_out);
                        const int keep = outcome;
                        for (std::size_t ii = 0; ii < dimz; ++ii) {
                            const unsigned long long i = static_cast<unsigned long long>(ii);
                            const int bit = static_cast<int>((i >> node.qubit) & 1ULL);
                            if (bit != keep) { h_re[ii] = 0.0; h_im[ii] = 0.0; }
                            else { h_re[ii] *= inv; h_im[ii] *= inv; }
                        }
                        // Re-upload collapsed state to GPU.
                        rc = qplayer_cuda_upload_state(
                            h_re, h_im, d_re_, d_im_, dim_ull, stream_handle_);
                        if (rc != 0) {
                            throw GPUError{rc,
                                "noisy execute(): H2D after measure collapse failed"};
                        }
                        // Readout error: collapse used the
                        // true outcome; only the reported classical bit is
                        // flipped. IfOp and final counts see the reported value.
                        int reported = outcome;
                        if (readout_active) {
                            const auto [p01, p10] =
                                noise->readout_error(node.qubit);
                            if (p01 > 0.0 || p10 > 0.0) {
                                reported = apply_readout_error(
                                    outcome, p01, p10, u01(traj_rng));
                            }
                        }
                        cubit_vals[static_cast<std::size_t>(node.cubit)] = reported;

                    } else if constexpr (std::is_same_v<T, IfOp>) {
                        // Classical control: evaluate register and apply body if match.
                        const ClassicalRegister& reg = cregs[node.creg];
                        std::uint64_t regval = 0;
                        for (ClassicalBitIndex b = 0; b < reg.width; ++b) {
                            if (cubit_vals[static_cast<std::size_t>(reg.offset + b)]) {
                                regval |= (std::uint64_t{1} << b);
                            }
                        }
                        if (regval == node.value) {
                            apply_gate(node.body);
                            need_kraus_download = true;
                        }

                    } else {
                        // BarrierOp: no state change.
                        (void)node;
                    }
                }, op);
            }  // end op loop

            // Norm check: verify |ψ|² ≈ 1 at trajectory end.
            // Ensure the latest state is in the pinned host buffers.
            if (need_kraus_download) {
                const int sync_rc = qplayer_cuda_stream_sync(stream_handle_);
                if (sync_rc != 0) {
                    throw GPUError{sync_rc,
                        "noisy execute(): stream sync for norm check failed"};
                }
                rc = qplayer_cuda_download_state_stream(
                    h_re, h_im, d_re_, d_im_, dim_ull, stream_handle_);
                if (rc != 0) {
                    throw GPUError{rc, "noisy execute(): D2H for norm check failed"};
                }
                need_kraus_download = false;
            }

            double norm = 0.0;
            for (std::size_t ii = 0; ii < dimz; ++ii) {
                norm += std::fma(h_re[ii], h_re[ii], h_im[ii]*h_im[ii]);
            }
            if (!std::isfinite(norm) || std::abs(norm - 1.0) > 1e-6) {
                throw SimulatorError{"FullStateGPU",
                    "numerical divergence at trajectory " + std::to_string(traj) +
                    ": |psi|^2 = " + std::to_string(norm)};
            }

            // Build bitstring from cubit_vals (MSB-first, same convention as CPU).
            std::string bs(static_cast<std::size_t>(total_c), '0');
            for (ClassicalBitIndex k = 0; k < total_c; ++k) {
                bs[static_cast<std::size_t>(k)] =
                    static_cast<char>('0' + cubit_vals[
                        static_cast<std::size_t>(total_c - 1u - k)]);
            }
            ++counts[bs];
        }  // end trajectory loop

        if (total_c == 0) {
            counts[""] = nshots;
        }

        Result r;
        result_set_counts(r, std::move(counts));
        result_set_shots(r, nshots);
        return r;
    }

    if (circuit.has_classical_control()) {
        throw NotImplementedError{"FullStateGPU",
            "Classical-control (IfOp) circuits not supported in "
            "GPU execute(); use FullState CPU backend for conditional circuits"};
    }

    // PRNG seed resolved here (mirrors CPU execute() master_seed_ logic)
    // and stored in the lazy snapshot; the nshots draw is deferred to
    // Result::counts() (lazy dense mode — reuses the attached AoS state).
    const std::uint64_t seed =
        master_seed_ != 0
            ? master_seed_
            : static_cast<std::uint64_t>(std::random_device{}());

    // ── fast path ────────────────────────────────────────────────────────
    GpuSectionTimer prof{"execute"};
    prepare(circuit);  // memory precheck + init_state (GPU |0...0>)
    prof.lap("prepare");

    // Apply unitary gate ops; skip Measure/Barrier/Idle (deferred to sampling).
    for (const Op& op : circuit.ops()) {
        std::visit([this](auto&& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, GateOp>) {
                if (node.type == GateType::Reset) {
                    throw NotImplementedError{"FullStateGPU",
                        "Reset in fast-path GPU execute() not supported"};
                } else if (node.type == GateType::Opaque) {
                    throw NotImplementedError{"FullStateGPU",
                        "Opaque gate not yet supported"};
                } else {
                    apply_gate(node);
                }
            }
            // MeasureOp / BarrierOp / IdleOp: defer — no state change.
        }, op);
    }

    prof.lap("gate-dispatch");

    // Synchronise the stream so all queued kernels have completed before D2H.
    // Wall time of this sync ~= GPU kernel execution (dispatch above is async).
    const int sync_rc = qplayer_cuda_stream_sync(stream_handle_);
    if (sync_rc != 0) {
        throw GPUError{sync_rc, "stream sync before state download failed"};
    }
    prof.lap("kernel-sync");

    // Collect (qubit, cubit) mapping for deferred bitstring construction.
    const ClassicalBitIndex total_c = circuit.total_cubits();
    std::vector<std::pair<QubitIndex, ClassicalBitIndex>> measure_map;
    for (const Op& op : circuit.ops()) {
        if (std::holds_alternative<MeasureOp>(op)) {
            const MeasureOp& m = std::get<MeasureOp>(op);
            measure_map.emplace_back(m.qubit, m.cubit);
        }
    }

    // GPU-resident sampling (Phase 7-C). Keep |psi> in VRAM and share
    // ownership of the device buffer with the Result instead of downloading the
    // full 2GiB state and materialising an AoS copy (: that host handoff
    // was 97.4% of q27 counts() time). Result::counts() samples on-device via
    // qplayer_cuda_sample_counts(); Result::statevector() D2Hs lazily on demand.
    Result r;
    result_set_lazy_sampling(r, nshots, seed, total_c, std::move(measure_map));
    result_set_gpu_resident(r, state_owner_, d_re_, d_im_, dim_);
    result_set_shots(r, nshots);
    return r;
}

int FullStateGpuBackend::apply_measure(QubitIndex /*qubit*/) {
    throw NotImplementedError{"FullStateGPU", kPhase};
}

void FullStateGpuBackend::apply_reset(QubitIndex /*qubit*/) {
    throw NotImplementedError{"FullStateGPU", kPhase};
}

// get_statevector_raw() — host-cache D2H copy + SoA RawStateView.
// The GPU state vector lives on the device; we download it into raw_cache_re_/im_
// (std::vector host buffers) on every call. The returned view is valid until
// the next state-mutating call (apply_gate / init_state). This matches the CPU
// contract: "the next gate invalidates the view" (table). Using
// pinned memory for the transfer would be faster but requires an extra
// cudaMallocHost/Free cycle; plain host vectors are sufficient here because
// get_statevector_raw() is not on the hot path.
RawStateView FullStateGpuBackend::get_statevector_raw() {
    if (d_re_ == nullptr) {
        throw SimulatorError{"FullStateGPU",
                             "get_statevector_raw called before init_state"};
    }

    const unsigned long long dim_ull = static_cast<unsigned long long>(dim_);
    const std::size_t dimi = static_cast<std::size_t>(dim_);

    // Allocate (or reuse) host cache vectors.
    raw_cache_re_.resize(dimi);
    raw_cache_im_.resize(dimi);

    // Use pinned transfer buffers for the D2H copy, then memcpy into the
    // std::vector buffers so the returned pointers are stable host memory
    // that will not be freed until the next resize (contract).
    double* h_re = nullptr;
    double* h_im = nullptr;
    int rc = qplayer_cuda_alloc_pinned(&h_re, &h_im, dim_ull);
    if (rc != 0) {
        throw GPUError{rc,
            "get_statevector_raw: cudaMallocHost pinned buffer failed"};
    }
    // Flush stream and copy D2H (stream-aware variant handles in-flight kernels).
    rc = qplayer_cuda_download_state_stream(h_re, h_im, d_re_, d_im_, dim_ull,
                                            stream_handle_);
    if (rc != 0) {
        qplayer_cuda_free_pinned(h_re, h_im);
        throw GPUError{rc, "get_statevector_raw: device->host copy failed"};
    }

    // Transfer into stable std::vector storage, then free the pinned buffer.
    for (std::size_t i = 0; i < dimi; ++i) {
        raw_cache_re_[i] = h_re[i];
        raw_cache_im_[i] = h_im[i];
    }
    qplayer_cuda_free_pinned(h_re, h_im);

    return RawStateView{raw_cache_re_.data(), raw_cache_im_.data(), dim_};
}

// expectation_value() — <psi|P|psi> for a Pauli string P.
// Reuses get_statevector() (AoS D2H) and computes the expectation value on the
// host with the same algorithm as FullStateCpuBackend::expectation_value()
// (Qiskit qubit order; empty string -> all-Z). This avoids
// duplicating a GPU kernel path for a non-hot-path API while guaranteeing
// identical numerical results to the CPU reference.
double FullStateGpuBackend::expectation_value(std::string_view pauli) {
    if (d_re_ == nullptr) {
        throw SimulatorError{"FullStateGPU",
                             "expectation_value called before init_state"};
    }

    // Build the all-Z default when the caller passes an empty string.
    // The local string outlives the string_view for the duration of this call.
    std::string all_z;
    if (pauli.empty()) {
        all_z.assign(static_cast<std::size_t>(num_qubits_), 'Z');
        pauli = all_z;
    }
    if (pauli.size() != static_cast<std::size_t>(num_qubits_)) {
        throw CircuitError{
            "expectation_value: Pauli string length (" +
            std::to_string(pauli.size()) + ") != num_qubits (" +
            std::to_string(num_qubits_) + ")"};
    }

    // Download the current device state once (reuse the raw-cache path so the
    // vectors are warmed up; the returned view is only used locally here).
    const RawStateView view = get_statevector_raw();
    const double* re = view.re;
    const double* im = view.im;

    // Build flip_mask (X|Y qubits), sign_mask (Y|Z qubits), and num_y (global
    // phase i^num_y) using Qiskit qubit order: pauli[0] acts on qubit (n-1).
    StateIndex flip_mask = 0;
    StateIndex sign_mask = 0;
    int        num_y     = 0;
    for (std::size_t k = 0; k < pauli.size(); ++k) {
        const QubitIndex q =
            num_qubits_ - 1u - static_cast<QubitIndex>(k);  // Qiskit order
        switch (pauli[k]) {
            case 'I': break;
            case 'X': flip_mask |= (StateIndex{1} << q); break;
            case 'Y': flip_mask |= (StateIndex{1} << q);
                      sign_mask |= (StateIndex{1} << q); ++num_y; break;
            case 'Z': sign_mask |= (StateIndex{1} << q); break;
            default:
                throw CircuitError{
                    std::string("expectation_value: invalid Pauli char '") +
                    pauli[k] + "' (only I/X/Y/Z allowed)"};
        }
    }

    // Global phase i^num_y from Y factors.
    double ph_re = 1.0, ph_im = 0.0;
    switch (num_y & 3) {
        case 0: ph_re =  1.0; ph_im =  0.0; break;
        case 1: ph_re =  0.0; ph_im =  1.0; break;
        case 2: ph_re = -1.0; ph_im =  0.0; break;
        case 3: ph_re =  0.0; ph_im = -1.0; break;
    }

    // Accumulate <psi|P|psi> = sum_j conj(psi[j^flip]) * coeff(j) * psi[j].
    // Hermitian P => imaginary part is numerical noise; return the real part.
    const std::int64_t idim = static_cast<std::int64_t>(dim_);
    double acc_re = 0.0;
    for (std::int64_t jj = 0; jj < idim; ++jj) {
        const StateIndex j  = static_cast<StateIndex>(jj);
        const StateIndex jp = j ^ flip_mask;
        const int parity = __builtin_popcountll(
            static_cast<std::uint64_t>(j & sign_mask)) & 1;
        const double sgn  = parity ? -1.0 : 1.0;
        const double c_re = sgn * ph_re;
        const double c_im = sgn * ph_im;
        const double bj_re = re[j],   bj_im = im[j];
        const double cp_re = re[jp],  cp_im = -im[jp];  // conj(psi[jp])
        const double t1_re = c_re * bj_re - c_im * bj_im;
        const double t1_im = c_re * bj_im + c_im * bj_re;
        acc_re += cp_re * t1_re - cp_im * t1_im;
    }
    return acc_re;
}

// nonzero_amplitudes() — enumerate (index, amplitude) pairs with |amp| >=
// kNonzeroAmplitudeThreshold via a full D2H download + host-side filter
// The threshold (1e-12) matches SparseState and the CPU
// backend so the nonzero set is identical across backends (FP dust excluded).
// GPU is dense (<=30 qubits) so densifying is always within the densify cap;
// no extra guard needed. Compares squared magnitudes to avoid a sqrt.
// Returns an empty vector before init_state() (d_re_ is null; no throw).
std::vector<std::pair<StateIndex, std::complex<double>>>
FullStateGpuBackend::nonzero_amplitudes() const {
    std::vector<std::pair<StateIndex, std::complex<double>>> result;
    if (d_re_ == nullptr) {
        return result;  // Not yet initialised — defined as empty.
    }
    // Cast away const for get_statevector() call (download is logically const;
    // the host copy is transient and does not modify device state).
    const StateVector sv =
        const_cast<FullStateGpuBackend*>(this)->get_statevector();
    constexpr double thr2 =
        kNonzeroAmplitudeThreshold * kNonzeroAmplitudeThreshold;
    for (StateIndex i = 0; i < static_cast<StateIndex>(sv.size()); ++i) {
        const double re = sv[static_cast<std::size_t>(i)].real();
        const double im = sv[static_cast<std::size_t>(i)].imag();
        if (re * re + im * im >= thr2) {
            result.emplace_back(i, sv[static_cast<std::size_t>(i)]);
        }
    }
    return result;
}

// num_qubits() — active qubit count (0 before init_state). Used by
// dump_statevector() to format bitstring width without densifying.
QubitIndex FullStateGpuBackend::num_qubits() const {
    return num_qubits_;
}

#else  // !QPLAYER_BUILD_GPU ── CPU-only build stubs ──────────────────────────

FullStateGpuBackend::FullStateGpuBackend()  = default;
FullStateGpuBackend::~FullStateGpuBackend() = default;

void FullStateGpuBackend::prepare(const Circuit& /*circuit*/) {
    throw NotImplementedError{"FullStateGPU",
        "GPU backend not built (QPLAYER_BUILD_GPU=OFF)"};
}
void FullStateGpuBackend::init_state(QubitIndex /*num_qubits*/) {
    throw NotImplementedError{"FullStateGPU",
        "GPU backend not built (QPLAYER_BUILD_GPU=OFF)"};
}
StateVector FullStateGpuBackend::get_statevector() {
    throw NotImplementedError{"FullStateGPU",
        "GPU backend not built (QPLAYER_BUILD_GPU=OFF)"};
}
std::uint32_t FullStateGpuBackend::max_qubits(detail::WorkingSet ws) const noexcept {
    return detail::max_qubits_for_budget(ws);
}
void FullStateGpuBackend::set_seed(std::uint64_t seed) {
    master_seed_ = seed;  // store even in CPU-only build (header access)
}

// Gate-application stubs (CPU-only build; GPU path not compiled).
Result FullStateGpuBackend::execute(const Circuit& /*c*/, std::uint32_t /*n*/,
                                    const NoiseModel* /*noise*/) {
    throw NotImplementedError{"FullStateGPU",
        "GPU backend not built (QPLAYER_BUILD_GPU=OFF)"};
}
void FullStateGpuBackend::apply_gate(const GateOp& /*op*/) {
    throw NotImplementedError{"FullStateGPU",
        "GPU backend not built (QPLAYER_BUILD_GPU=OFF)"};
}
int FullStateGpuBackend::apply_measure(QubitIndex /*q*/) {
    throw NotImplementedError{"FullStateGPU",
        "GPU backend not built (QPLAYER_BUILD_GPU=OFF)"};
}
void FullStateGpuBackend::apply_reset(QubitIndex /*q*/) {
    throw NotImplementedError{"FullStateGPU",
        "GPU backend not built (QPLAYER_BUILD_GPU=OFF)"};
}
RawStateView FullStateGpuBackend::get_statevector_raw() {
    throw NotImplementedError{"FullStateGPU",
        "GPU backend not built (QPLAYER_BUILD_GPU=OFF)"};
}
double FullStateGpuBackend::expectation_value(std::string_view /*p*/) {
    throw NotImplementedError{"FullStateGPU",
        "GPU backend not built (QPLAYER_BUILD_GPU=OFF)"};
}
std::vector<std::pair<StateIndex, std::complex<double>>>
FullStateGpuBackend::nonzero_amplitudes() const {
    throw NotImplementedError{"FullStateGPU",
        "GPU backend not built (QPLAYER_BUILD_GPU=OFF)"};
}
QubitIndex FullStateGpuBackend::num_qubits() const {
    return num_qubits_;
}

#endif  // QPLAYER_BUILD_GPU

// ── Metadata (both builds) ────────────────────────────────────────────────────

SimType          FullStateGpuBackend::sim_type()       const { return SimType::FullState; }
bool             FullStateGpuBackend::supports_noise() const { return true; }
bool             FullStateGpuBackend::supports_gpu()   const { return true; }
std::string_view FullStateGpuBackend::name()           const { return "FullStateGPU"; }

// apply_op() — routes each IR Op node to a backend primitive.
// Fusion boundaries (BarrierOp, MeasureOp, IfOp, IdleOp) are never merged
// across.
void FullStateGpuBackend::apply_op(const Op& op) {
    std::visit([this](auto&& node) {
        using T = std::decay_t<decltype(node)>;

        if constexpr (std::is_same_v<T, GateOp>) {
            apply_gate(node);

        } else if constexpr (std::is_same_v<T, MeasureOp>) {
            apply_measure(node.qubit);

        } else if constexpr (std::is_same_v<T, BarrierOp>) {
            // Fusion boundary; no state change.
            (void)node;

        } else if constexpr (std::is_same_v<T, IfOp>) {
            // execute() handles IfOp; apply_op() cannot evaluate classical regs.
            throw SimulatorError{"FullStateGPU",
                "apply_op: IfOp must be handled by execute() per-shot loop "
                "(use QRegister::run() for circuits with classical control)"};

        } else if constexpr (std::is_same_v<T, IdleOp>) {
            apply_idle(node);
        }
    }, op);
}

// apply_idle() — Phase 8 slot for Amplitude Damping + Dephasing Kraus.
// No state change in.
void FullStateGpuBackend::apply_idle(const IdleOp& /*op*/) {
}

}  // namespace qplayer
