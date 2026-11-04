// test_full_state_gpu.cpp — FullState GPU backend tests.
// Two layers: (1) factory runtime-probe behaviour, which must hold in both
// CPU-only and GPU builds; (2) the 1Q/2Q CUDA gate
// kernels, compiled and run only when QPLAYER_BUILD_GPU is defined.

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"

#include <cmath>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include "../../../include/backend/simulator_backend.hpp"
#include "../../../include/backend/full_state/qubit_reorder.hpp"
#include "../../../include/circuit/circuit.hpp"
#include "../../../include/error.hpp"
#include "../../../include/noise/noise_model.hpp"
#include "../../../include/qregister.hpp"
#include "../../../include/types.hpp"

using qplayer::make_backend;
using qplayer::ProcType;
using qplayer::SimType;
using qplayer::SimulatorBackend;

// Forward-declare the CUDA device count probe at file (global) scope inside a
// GPU-build guard. This satisfies GCC's C++17 [dcl.link] requirement that
// extern "C" linkage specifications appear at namespace scope, not inside a
// function body. The symbol is defined in full_state_gpu.cu.
#if defined(QPLAYER_BUILD_GPU)
extern "C" int qplayer_cuda_device_count();
#endif

// Helper: returns true when the GPU build is active and at least one CUDA
// device is visible at runtime. Always false in CPU-only builds.
static bool gpu_device_present() noexcept {
#if defined(QPLAYER_BUILD_GPU)
    return qplayer_cuda_device_count() > 0;
#else
    return false;
#endif
}

// A GPU request must never throw: with no usable device the factory falls back
// to the CPU backend, and either way the result stays in the FullState family
// This is the core factory-probe invariant.
TEST(FullStateGpuFactory, GpuRequestResolvesToFullStateFamily) {
    std::unique_ptr<SimulatorBackend> backend;
    EXPECT_NO_THROW(backend = make_backend(SimType::FullState, ProcType::GPU));
    EXPECT_TRUE(backend != nullptr);
    EXPECT_TRUE(backend->sim_type() == SimType::FullState);
}

// ProcType::Auto resolves to the GPU backend when a CUDA device is
// present (GPU build), otherwise falls back to the CPU backend. The resolved
// backend must always belong to the FullState family (sim_type == FullState).
// The name is "FullStateGPU" with a GPU device present, "FullState" otherwise.
TEST(FullStateGpuFactory, AutoResolvesToGpuWhenDevicePresent) {
    std::unique_ptr<SimulatorBackend> backend;
    EXPECT_NO_THROW(backend = make_backend(SimType::FullState, ProcType::Auto));
    EXPECT_TRUE(backend != nullptr);
    // sim_type must always be FullState regardless of CPU/GPU resolution.
    EXPECT_TRUE(backend->sim_type() == SimType::FullState);
    // Name: "FullStateGPU" when a CUDA device is present in a GPU build,
    // "FullState" in a CPU-only build or when no device is available.
    const std::string_view expected_name =
        gpu_device_present() ? std::string_view("FullStateGPU")
                             : std::string_view("FullState");
    EXPECT_TRUE(backend->name() == expected_name);
}

// QRegister::max_qubits() delegates to the bound backend. The Auto
// backend (CPU or GPU) returns a positive count under the uint64
// overflow guard (59). On GPU it uses the VRAM dual-budget model.
TEST(FullStateGpuFactory, MaxQubitsDelegatesToBackendAndIsBounded) {
    qplayer::QRegister qr(SimType::FullState);  // Auto -> GPU or CPU
    const std::uint32_t mq = qr.max_qubits();
    EXPECT_TRUE(mq > 0);
    EXPECT_TRUE(mq < 59);
}

#if defined(QPLAYER_BUILD_GPU)

// Self-contained CUDA launchers exported by full_state_gpu.cu. extern "C"
// linkage means the symbols are namespace-agnostic, so the test names them
// directly without the qplayer::cuda qualifier.
extern "C" int qplayer_cuda_device_count();
extern "C" int qplayer_cuda_apply_1q(double* h_re, double* h_im,
                                     unsigned long long dim,
                                     unsigned int target, const double* u8);
extern "C" int qplayer_cuda_apply_2q(double* h_re, double* h_im,
                                     unsigned long long dim,
                                     unsigned int control, unsigned int target,
                                     const double* u8);
extern "C" int qplayer_cuda_apply_ccx(double* h_re, double* h_im,
                                      unsigned long long dim,
                                      unsigned int c0, unsigned int c1,
                                      unsigned int target);
extern "C" int qplayer_cuda_reorder_host(double* h_re, double* h_im,
                                         unsigned long long dim,
                                         unsigned int n_qubits,
                                         const unsigned int* perm);
extern "C" int  qplayer_cuda_init_curand_states(void** states_out,
                                                const unsigned long long* h_seeds,
                                                int n_traj);
extern "C" void qplayer_cuda_free_curand_states(void* states);

// The runtime probe must report at least one device on a CUDA-enabled host.
TEST(FullStateGpuKernel, DeviceCountIsPositive) {
    EXPECT_TRUE(qplayer_cuda_device_count() > 0);
}

// an explicit ProcType::GPU request opts into the GPU backend
// ("FullStateGPU") when a device is present, even though execute() is still
// Phase-6-pending. This is the opt-in path Auto deliberately avoids.
TEST(FullStateGpuFactory, ExplicitGpuRequestSelectsGpuBackendWhenDevicePresent) {
    if (qplayer_cuda_device_count() <= 0) return;  // no device -> CPU fallback path
    auto backend = make_backend(SimType::FullState, ProcType::GPU);
    EXPECT_TRUE(backend != nullptr);
    EXPECT_TRUE(backend->name() == std::string_view("FullStateGPU"));
}

// H|0> = (|0> + |1>)/sqrt(2): the 1Q kernel must produce equal real amplitudes.
TEST(FullStateGpuKernel, Hadamard1qOnZeroState_ProducesEqualSuperposition) {
    // Arrange
    const double h = 1.0 / std::sqrt(2.0);
    std::vector<double> re = {1.0, 0.0};
    std::vector<double> im = {0.0, 0.0};
    const double u8[8] = {h, 0.0, h, 0.0, h, 0.0, -h, 0.0};  // H, row-major (re,im)

    // Act
    const int rc = qplayer_cuda_apply_1q(re.data(), im.data(), 2, /*target=*/0, u8);

    // Assert
    EXPECT_EQ(rc, 0);                 // cudaSuccess
    EXPECT_NEAR(re[0], h, 1e-12);     // exact analytic value
    EXPECT_NEAR(re[1], h, 1e-12);
    EXPECT_NEAR(im[0], 0.0, 1e-12);
    EXPECT_NEAR(im[1], 0.0, 1e-12);
}

// Controlled-X with control=1, target=0 must flip |10> (index 2) to |11>
// (index 3), leaving the control-0 subspace untouched.
TEST(FullStateGpuKernel, ControlledX2q_FlipsTargetWhenControlSet) {
    // Arrange: basis state |index=2> (control bit1 = 1, target bit0 = 0).
    std::vector<double> re = {0.0, 0.0, 1.0, 0.0};
    std::vector<double> im = {0.0, 0.0, 0.0, 0.0};
    const double x8[8] = {0.0, 0.0, 1.0, 0.0, 1.0, 0.0, 0.0, 0.0};  // Pauli X

    // Act
    const int rc =
        qplayer_cuda_apply_2q(re.data(), im.data(), 4, /*control=*/1, /*target=*/0, x8);

    // Assert
    EXPECT_EQ(rc, 0);
    EXPECT_NEAR(re[2], 0.0, 1e-12);   // |10> emptied
    EXPECT_NEAR(re[3], 1.0, 1e-12);   // |11> now carries the amplitude
    EXPECT_NEAR(re[0], 0.0, 1e-12);   // control-0 subspace untouched
    EXPECT_NEAR(re[1], 0.0, 1e-12);
}

// Controlled-X must leave a control-0 state unchanged (no spurious flip).
TEST(FullStateGpuKernel, ControlledX2q_NoopWhenControlClear) {
    // Arrange: basis state |index=1> (control bit1 = 0, target bit0 = 1).
    std::vector<double> re = {0.0, 1.0, 0.0, 0.0};
    std::vector<double> im = {0.0, 0.0, 0.0, 0.0};
    const double x8[8] = {0.0, 0.0, 1.0, 0.0, 1.0, 0.0, 0.0, 0.0};

    // Act
    const int rc =
        qplayer_cuda_apply_2q(re.data(), im.data(), 4, /*control=*/1, /*target=*/0, x8);

    // Assert
    EXPECT_EQ(rc, 0);
    EXPECT_NEAR(re[1], 1.0, 1e-12);   // unchanged — control was 0
    EXPECT_NEAR(re[3], 0.0, 1e-12);
}

// CCX (Toffoli) regression: with both controls = 1 the target must flip
// deterministically (|110> index 3 -> |111> index 7 in a 3-qubit register,
// qubit 0 = least significant). Guards against the prior decomposition bug
// that left the target in a superposition (sampling-comparison FAIL, 2026-06-12).
TEST(FullStateGpuKernel, CCX_FlipsTargetWhenBothControlsSet) {
    // Arrange: |index=3> = q0=1,q1=1,q2=0 (controls c0=0,c1=1 set; target=2 clear).
    std::vector<double> re(8, 0.0);
    std::vector<double> im(8, 0.0);
    re[3] = 1.0;

    // Act
    const int rc = qplayer_cuda_apply_ccx(re.data(), im.data(), 8,
                                          /*c0=*/0, /*c1=*/1, /*target=*/2);

    // Assert: amplitude moved 3 -> 7 (target bit set), rest empty.
    EXPECT_EQ(rc, 0);
    EXPECT_NEAR(re[7], 1.0, 1e-12);
    EXPECT_NEAR(re[3], 0.0, 1e-12);
    for (int k = 0; k < 8; ++k)
        if (k != 7) EXPECT_NEAR(re[k], 0.0, 1e-12);
}

// CCX must be a no-op when either control is 0 (here c1=1 is clear at index 1).
TEST(FullStateGpuKernel, CCX_NoopWhenAControlClear) {
    // Arrange: |index=1> = q0=1 (c0 set), q1=0 (c1 clear), q2=0 (target clear).
    std::vector<double> re(8, 0.0);
    std::vector<double> im(8, 0.0);
    re[1] = 1.0;

    // Act
    const int rc = qplayer_cuda_apply_ccx(re.data(), im.data(), 8,
                                          /*c0=*/0, /*c1=*/1, /*target=*/2);

    // Assert: unchanged — second control was 0.
    EXPECT_EQ(rc, 0);
    EXPECT_NEAR(re[1], 1.0, 1e-12);
    EXPECT_NEAR(re[5], 0.0, 1e-12);  // would-be flipped index if it fired
}

// GPU-resident init_state() + the single device->host copy of
// get_statevector() must round-trip the |0...0> ground state.
TEST(FullStateGpuKernel, ResidentInitThenGetStatevector_ReturnsGroundState) {
    if (qplayer_cuda_device_count() <= 0) return;
    auto backend = make_backend(SimType::FullState, ProcType::GPU);
    if (backend->name() != std::string_view("FullStateGPU")) return;

    backend->init_state(2);
    auto sv = backend->get_statevector();

    EXPECT_EQ(sv.size(), static_cast<std::size_t>(4));
    EXPECT_NEAR(sv[0].real(), 1.0, 1e-12);  // |00> amplitude
    EXPECT_NEAR(sv[0].imag(), 0.0, 1e-12);
    EXPECT_NEAR(sv[1].real(), 0.0, 1e-12);
    EXPECT_NEAR(sv[2].real(), 0.0, 1e-12);
    EXPECT_NEAR(sv[3].real(), 0.0, 1e-12);
}

// / : GPU max_qubits() reports min(VRAM, host), a positive
// count under the overflow guard.
TEST(FullStateGpuKernel, MaxQubitsRespectsDualBudget) {
    if (qplayer_cuda_device_count() <= 0) return;
    auto backend = make_backend(SimType::FullState, ProcType::GPU);
    if (backend->name() != std::string_view("FullStateGPU")) return;

    const std::uint32_t mq = backend->max_qubits(qplayer::detail::WorkingSet::Execute);
    EXPECT_TRUE(mq > 0);
    EXPECT_TRUE(mq < 59);
}

// / : an oversized circuit (2^40 x 16 x 2 = 32 TiB) must fail
// the VRAM precheck with a GPUError (which is-a SimulatorError).
TEST(FullStateGpuKernel, PrepareThrowsOnOversizedCircuit) {
    if (qplayer_cuda_device_count() <= 0) return;
    auto backend = make_backend(SimType::FullState, ProcType::GPU);
    if (backend->name() != std::string_view("FullStateGPU")) return;

    qplayer::Circuit circ(40, 0);
    EXPECT_THROW(backend->prepare(circ), qplayer::GPUError);
}

// / : the GPU reorder kernel must produce the same permutation as
// the CPU reference detail::reorder_qubits (new_index bit k = old bit perm[k]).
TEST(FullStateGpuKernel, QubitReorderMatchesCpuReference) {
    if (qplayer_cuda_device_count() <= 0) return;

    // Arrange: 3-qubit state with distinct amplitudes; perm swaps bit0<->bit2.
    const unsigned int n = 3;
    const unsigned long long dim = 8;
    std::vector<double> re_gpu(dim), im_gpu(dim), re_cpu(dim), im_cpu(dim);
    for (std::size_t i = 0; i < dim; ++i) {
        re_gpu[i] = re_cpu[i] = static_cast<double>(i + 1);
        im_gpu[i] = im_cpu[i] = static_cast<double>(i) * -3.0;
    }
    const unsigned int        gperm[3] = {2, 1, 0};
    std::vector<qplayer::QubitIndex> cperm = {2, 1, 0};

    // Act
    const int rc = qplayer_cuda_reorder_host(re_gpu.data(), im_gpu.data(), dim, n, gperm);
    qplayer::detail::reorder_qubits(re_cpu.data(), im_cpu.data(),
                                    qplayer::StateIndex{8}, 3, cperm);

    // Assert
    EXPECT_EQ(rc, 0);
    for (std::size_t i = 0; i < dim; ++i) {
        EXPECT_NEAR(re_gpu[i], re_cpu[i], 1e-12);
        EXPECT_NEAR(im_gpu[i], im_cpu[i], 1e-12);
    }
}

// / transparency: reorder followed by its inverse permutation must
// restore the original state exactly (round-trip == identity).
TEST(FullStateGpuKernel, QubitReorderRoundTripIsIdentity) {
    if (qplayer_cuda_device_count() <= 0) return;

    const unsigned int n = 3;
    const unsigned long long dim = 8;
    std::vector<double> re(dim), im(dim), re0(dim), im0(dim);
    for (std::size_t i = 0; i < dim; ++i) {
        re[i] = re0[i] = static_cast<double>(i + 1);
        im[i] = im0[i] = static_cast<double>(i) * 2.0;
    }
    const unsigned int perm[3] = {1, 2, 0};  // inverse: inv[perm[i]]=i -> {2,0,1}
    const unsigned int inv[3]  = {2, 0, 1};

    EXPECT_EQ(qplayer_cuda_reorder_host(re.data(), im.data(), dim, n, perm), 0);
    EXPECT_EQ(qplayer_cuda_reorder_host(re.data(), im.data(), dim, n, inv), 0);

    for (std::size_t i = 0; i < dim; ++i) {
        EXPECT_NEAR(re[i], re0[i], 1e-12);
        EXPECT_NEAR(im[i], im0[i], 1e-12);
    }
}

// ── : GPU noiseless execute() tests ───────────────────────────────────────

// Bell state via GPU execute(): H(0) + CX(0,1) + measure both qubits.
// Expected distribution: ~50% "00", ~50% "11", 0% "01"/"10".
TEST(FullStateGpuExecute, BellCircuit_CountsAre00And11Only) {
    if (qplayer_cuda_device_count() <= 0) return;
    auto backend = make_backend(SimType::FullState, ProcType::GPU);
    if (backend->name() != std::string_view("FullStateGPU")) return;

    // Arrange
    qplayer::Circuit c(2, 2);
    c.h(0).cx(0, 1).measure(0, 0).measure(1, 1);
    backend->set_seed(42);

    // Act
    const auto result = backend->execute(c, 10000);

    // Assert: only "00" and "11" should appear.
    const auto& counts = result.counts();
    EXPECT_TRUE(counts.find("01") == counts.end());
    EXPECT_TRUE(counts.find("10") == counts.end());
    const double total = static_cast<double>(result.shots());
    const double p00 =
        (counts.count("00") ? static_cast<double>(counts.at("00")) : 0.0) / total;
    const double p11 =
        (counts.count("11") ? static_cast<double>(counts.at("11")) : 0.0) / total;
    // Statistical tolerance for nshots=10000.
    EXPECT_NEAR(p00, 0.5, 0.03);
    EXPECT_NEAR(p11, 0.5, 0.03);
}

// Bell state statevector: GPU result must agree with CPU within 1e-10 (golden precondition).
TEST(FullStateGpuExecute, BellCircuit_StatevectorMatchesCpu) {
    if (qplayer_cuda_device_count() <= 0) return;
    auto gpu = make_backend(SimType::FullState, ProcType::GPU);
    if (gpu->name() != std::string_view("FullStateGPU")) return;

    qplayer::Circuit c(2, 2);
    c.h(0).cx(0, 1).measure(0, 0).measure(1, 1);

    // GPU execute
    const auto gpu_result = gpu->execute(c, 1);
    const auto gpu_sv = gpu_result.statevector();

    // CPU reference: init + apply H + CX directly
    auto cpu = make_backend(SimType::FullState, ProcType::CPU);
    cpu->init_state(2);
    {
        qplayer::GateOp h_op;
        h_op.type   = qplayer::GateType::H;
        h_op.qubits = {0};
        cpu->apply_gate(h_op);
    }
    {
        qplayer::GateOp cx_op;
        cx_op.type   = qplayer::GateType::CX;
        cx_op.qubits = {0, 1};
        cpu->apply_gate(cx_op);
    }
    const auto cpu_sv = cpu->get_statevector();

    // Bell state: [1/sqrt(2), 0, 0, 1/sqrt(2)]
    EXPECT_EQ(gpu_sv.size(), static_cast<std::size_t>(4));
    for (std::size_t i = 0; i < 4; ++i) {
        EXPECT_NEAR(gpu_sv[i].real(), cpu_sv[i].real(), 1e-10);
        EXPECT_NEAR(gpu_sv[i].imag(), cpu_sv[i].imag(), 1e-10);
    }
}

// ECR genuine 4x4 gate: GPU dense-2Q kernel must match the CPU
// apply_dense_2q_kernel within 1e-10. Uses reversed operands + a superposed
// context so operand order and off-diagonal terms are both exercised.
TEST(FullStateGpuKernel, EcrGate_GpuMatchesCpu) {
    if (qplayer_cuda_device_count() <= 0) return;
    auto gpu = make_backend(SimType::FullState, ProcType::GPU);
    if (gpu->name() != std::string_view("FullStateGPU")) return;

    auto apply_seq = [](std::unique_ptr<SimulatorBackend>& b) {
        b->init_state(3);
        auto g = [&](qplayer::GateType t, std::vector<qplayer::QubitIndex> q) {
            qplayer::GateOp op; op.type = t; op.qubits = std::move(q);
            b->apply_gate(op);
        };
        g(qplayer::GateType::H,   {0});
        g(qplayer::GateType::X,   {2});
        g(qplayer::GateType::ECR, {2, 0});   // reversed operands
        g(qplayer::GateType::ECR, {0, 1});
    };

    apply_seq(gpu);
    const auto gpu_sv = gpu->get_statevector();

    auto cpu = make_backend(SimType::FullState, ProcType::CPU);
    apply_seq(cpu);
    const auto cpu_sv = cpu->get_statevector();

    EXPECT_EQ(gpu_sv.size(), cpu_sv.size());
    for (std::size_t i = 0; i < cpu_sv.size(); ++i) {
        EXPECT_NEAR(gpu_sv[i].real(), cpu_sv[i].real(), 1e-10);
        EXPECT_NEAR(gpu_sv[i].imag(), cpu_sv[i].imag(), 1e-10);
    }
}

// GHZ 3-qubit circuit via GPU execute(): H(0) + CX(0,1) + CX(1,2) + measure all.
// Expected: ~50% "000", ~50% "111".
TEST(FullStateGpuExecute, GHZ3Qubit_CountsAre000And111Only) {
    if (qplayer_cuda_device_count() <= 0) return;
    auto backend = make_backend(SimType::FullState, ProcType::GPU);
    if (backend->name() != std::string_view("FullStateGPU")) return;

    qplayer::Circuit c(3, 3);
    c.h(0).cx(0, 1).cx(1, 2).measure(0, 0).measure(1, 1).measure(2, 2);
    backend->set_seed(123);

    const auto result = backend->execute(c, 10000);
    const auto& counts = result.counts();
    const double total = static_cast<double>(result.shots());

    // Only "000" and "111" should appear (GHZ property).
    for (const auto& [bs, cnt] : counts) {
        EXPECT_TRUE(bs == "000" || bs == "111");
        (void)cnt;
    }
    const double p000 =
        (counts.count("000") ? static_cast<double>(counts.at("000")) : 0.0) / total;
    const double p111 =
        (counts.count("111") ? static_cast<double>(counts.at("111")) : 0.0) / total;
    EXPECT_NEAR(p000, 0.5, 0.03);
    EXPECT_NEAR(p111, 0.5, 0.03);
}

// GHZ 5-qubit circuit.
TEST(FullStateGpuExecute, GHZ5Qubit_CountsAre00000And11111Only) {
    if (qplayer_cuda_device_count() <= 0) return;
    auto backend = make_backend(SimType::FullState, ProcType::GPU);
    if (backend->name() != std::string_view("FullStateGPU")) return;

    qplayer::Circuit c(5, 5);
    c.h(0).cx(0, 1).cx(1, 2).cx(2, 3).cx(3, 4);
    c.measure(0, 0).measure(1, 1).measure(2, 2).measure(3, 3).measure(4, 4);
    backend->set_seed(456);

    const auto result = backend->execute(c, 10000);
    const auto& counts = result.counts();
    const double total = static_cast<double>(result.shots());

    for (const auto& [bs, cnt] : counts) {
        EXPECT_TRUE(bs == "00000" || bs == "11111");
        (void)cnt;
    }
    const double p0 =
        (counts.count("00000") ? static_cast<double>(counts.at("00000")) : 0.0) / total;
    const double p1 =
        (counts.count("11111") ? static_cast<double>(counts.at("11111")) : 0.0) / total;
    EXPECT_NEAR(p0, 0.5, 0.03);
    EXPECT_NEAR(p1, 0.5, 0.03);
}

// set_seed reproducibility: same seed must yield identical counts across two runs.
TEST(FullStateGpuExecute, SetSeed_SameSeedGivesSameCounts) {
    if (qplayer_cuda_device_count() <= 0) return;

    qplayer::Circuit c(2, 2);
    c.h(0).cx(0, 1).measure(0, 0).measure(1, 1);

    auto b1 = make_backend(SimType::FullState, ProcType::GPU);
    auto b2 = make_backend(SimType::FullState, ProcType::GPU);
    if (b1->name() != std::string_view("FullStateGPU")) return;
    if (b2->name() != std::string_view("FullStateGPU")) return;

    b1->set_seed(777);
    b2->set_seed(777);

    const auto r1 = b1->execute(c, 1000);
    const auto r2 = b2->execute(c, 1000);

    // Identical seeds must produce identical count maps.
    EXPECT_TRUE(r1.counts() == r2.counts());
}

// GPU apply_gate (interactive path) via init_state + apply_gate + get_statevector.
// H|0> = (|0>+|1>)/sqrt(2): both amplitudes must equal 1/sqrt(2).
TEST(FullStateGpuExecute, ApplyGate_Hadamard_ProducesEqualSuperposition) {
    if (qplayer_cuda_device_count() <= 0) return;
    auto backend = make_backend(SimType::FullState, ProcType::GPU);
    if (backend->name() != std::string_view("FullStateGPU")) return;

    backend->init_state(1);
    qplayer::GateOp h_op;
    h_op.type   = qplayer::GateType::H;
    h_op.qubits = {0};
    EXPECT_NO_THROW(backend->apply_gate(h_op));

    const auto sv = backend->get_statevector();
    const double inv_sqrt2 = 1.0 / std::sqrt(2.0);
    EXPECT_EQ(sv.size(), static_cast<std::size_t>(2));
    EXPECT_NEAR(sv[0].real(), inv_sqrt2, 1e-10);  // exact analytic value
    EXPECT_NEAR(sv[1].real(), inv_sqrt2, 1e-10);
    EXPECT_NEAR(sv[0].imag(), 0.0, 1e-10);
    EXPECT_NEAR(sv[1].imag(), 0.0, 1e-10);
}

// / : the cuRAND trajectory state init launcher must allocate and
// seed n_traj Philox states without error and hand back a non-null buffer.
TEST(FullStateGpuKernel, CurandStateInitSucceedsAndFrees) {
    if (qplayer_cuda_device_count() <= 0) return;

    const std::vector<unsigned long long> seeds = {1ULL, 2ULL, 3ULL, 4ULL,
                                                   5ULL, 6ULL, 7ULL, 8ULL};
    void* states = nullptr;
    const int rc = qplayer_cuda_init_curand_states(
        &states, seeds.data(), static_cast<int>(seeds.size()));

    EXPECT_EQ(rc, 0);
    EXPECT_TRUE(states != nullptr);
    qplayer_cuda_free_curand_states(states);  // no crash on free
}

// GPU noisy execute() now fully implemented. An empty NoiseModel
// selects the Monte Carlo trajectory path; with no channels registered the state
// evolves noiseless and produces the Bell distribution. Verifies:
// (a) execute() succeeds without throwing (trajectory body is live in),
//   (b) the returned counts are normalised (non-zero entries only for 00 and 11),
//   (c) the distribution is within statistical tolerance of 50% each.
TEST(FullStateGpuExecute, NoisyExecute_EmptyNoiseModel_ProducesBellDistribution) {
    if (qplayer_cuda_device_count() <= 0) return;
    auto backend = make_backend(SimType::FullState, ProcType::GPU);
    if (backend->name() != std::string_view("FullStateGPU")) return;

    qplayer::Circuit c(2, 2);
    c.h(0).cx(0, 1).measure(0, 0).measure(1, 1);
    qplayer::NoiseModel nm;  // empty model — selects noisy path, but no channels

    qplayer::Result result;
    // Statistical tolerance 0.05 for nshots=1000.
    EXPECT_NO_THROW(result = backend->execute(c, 1000, &nm));
    const auto counts = result.counts();
    const double total = static_cast<double>(result.shots());
    const double p00 = counts.count("00") ? static_cast<double>(counts.at("00")) / total : 0.0;
    const double p11 = counts.count("11") ? static_cast<double>(counts.at("11")) / total : 0.0;
    // Bell state: |00> and |11> each appear 50% +/- 5% (statistical, nshots=1000).
    EXPECT_NEAR(p00, 0.5, 0.05);
    EXPECT_NEAR(p11, 0.5, 0.05);
}

// Google native 2Q gates: GPU dense-2Q kernel must match CPU within 1e-10.
// Tests iSWAP, sqrt_iSWAP, and fSim(0.7, 0.3) in a 3-qubit entangled context.
TEST(FullStateGpuKernel, GoogleNativeGates_GpuMatchesCpu) {
    if (qplayer_cuda_device_count() <= 0) return;
    auto gpu = make_backend(SimType::FullState, ProcType::GPU);
    if (gpu->name() != std::string_view("FullStateGPU")) return;

    auto apply_seq = [](std::unique_ptr<SimulatorBackend>& b) {
        b->init_state(3);
        auto g = [&](qplayer::GateType t, std::vector<qplayer::QubitIndex> q,
                     std::vector<double> params = {}) {
            qplayer::GateOp op; op.type = t; op.qubits = std::move(q);
            op.params = std::move(params);
            b->apply_gate(op);
        };
        g(qplayer::GateType::H,          {0});
        g(qplayer::GateType::X,          {2});
        g(qplayer::GateType::ISWAP,      {0, 1});
        g(qplayer::GateType::SQRT_ISWAP, {1, 2});
        g(qplayer::GateType::FSIM,       {0, 2}, {0.7, 0.3});
    };

    apply_seq(gpu);
    const auto gpu_sv = gpu->get_statevector();

    auto cpu = make_backend(SimType::FullState, ProcType::CPU);
    apply_seq(cpu);
    const auto cpu_sv = cpu->get_statevector();

    EXPECT_EQ(gpu_sv.size(), cpu_sv.size());
    for (std::size_t i = 0; i < cpu_sv.size(); ++i) {
        // 1e-10: GPU double-precision matches CPU; small rounding allowed.
        EXPECT_NEAR(gpu_sv[i].real(), cpu_sv[i].real(), 1e-10);
        EXPECT_NEAR(gpu_sv[i].imag(), cpu_sv[i].imag(), 1e-10);
    }
}

#endif  // QPLAYER_BUILD_GPU
