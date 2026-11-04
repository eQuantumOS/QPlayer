/// @file factory.cpp
/// @brief -- Backend factory.
///
/// make_backend() instantiates one of the four concrete SimulatorBackend
/// subclasses driven by (SimType, ProcType): SimType picks the family
/// (FullState / SparseState / TensorNetwork), ProcType picks the CPU vs GPU
/// implementation within the family. Constructors are trivial in Phase 0-S,
/// so creation always succeeds; prepare()/execute()/init_state() etc. then
/// throw the appropriate per-phase NotImplementedError when the user actually
/// drives the backend.
///
/// GPU fallback policy: when proc resolves to GPU but no
/// usable CUDA device is present, fall back to the CPU backend with a
/// qplayer::log::warn() banner. combines a build-time gate (QPLAYER_BUILD_GPU)
/// with a runtime probe (cudaGetDeviceCount via qplayer_cuda_device_count): the
/// GPU backend is selected only when the build enabled CUDA *and* at least one
/// device is visible, so a GPU-enabled binary still degrades gracefully on a
/// machine with no GPU attached.

#include "backend/simulator_backend.hpp"

#include "backend/full_state/full_state_backend.hpp"
#include "backend/sparse_state/sparse_state_backend.hpp"
#include "backend/tensor_network/tn_backend.hpp"
#include "error.hpp"
#include "log.hpp"

namespace qplayer {

#if defined(QPLAYER_BUILD_GPU)
// Defined in full_state_gpu.cu. Returns the count of visible CUDA devices (0 on
// any runtime error). Declared here so factory.cpp stays free of cuda_runtime.h
// — the one-way include policy keeps CUDA headers confined to .cu TUs.
extern "C" int qplayer_cuda_device_count();
#endif

// Returns true when the build provides a CUDA GPU backend *and* a CUDA device
// is actually visible at runtime. The build-time gate
// avoids referencing the CUDA symbol when QPLAYER_BUILD_GPU=OFF; the runtime
// probe guards against GPU-enabled binaries running on a GPU-less host.
// Declared in simulator_backend.hpp so QRegister::gpu_available() reports the
// same verdict that resolve_proc() acts on (audit #6).
namespace detail {
bool gpu_runtime_available() {
#if defined(QPLAYER_BUILD_GPU)
    return qplayer_cuda_device_count() > 0;
#else
    return false;
#endif
}
}  // namespace detail

namespace {

// Resolves ProcType::Auto to CPU or GPU, and downgrades GPU to CPU when the
// build lacks GPU support. Emits a warning on fallback so users see why the
// requested backend was substituted.
ProcType resolve_proc(ProcType proc) {
    if (proc == ProcType::Auto) {
        // Auto picks GPU whenever a CUDA device is present ( — the
        // temporary CPU gating is lifted). Circuits the GPU execute()
        // cannot handle (IfOp/noise/Reset/Opaque) and VRAM OOM are caught and
        // retried on CPU by QRegister::run()'s runtime fallback.
        return detail::gpu_runtime_available() ? ProcType::GPU : ProcType::CPU;
    }
    if (proc == ProcType::GPU && !detail::gpu_runtime_available()) {
        log::warn("GPU backend requested but no usable CUDA device detected "
                  "(CUDA build disabled or no GPU present); falling back to CPU.");
        return ProcType::CPU;
    }
    return proc;
}

}  // namespace

std::unique_ptr<SimulatorBackend> make_backend(
    SimType  sim_type,
    ProcType proc) {
    const ProcType resolved = resolve_proc(proc);

    switch (sim_type) {
        case SimType::FullState:
            return resolved == ProcType::GPU
                ? std::unique_ptr<SimulatorBackend>(std::make_unique<FullStateGpuBackend>())
                : std::unique_ptr<SimulatorBackend>(std::make_unique<FullStateCpuBackend>());
        case SimType::SparseState:
        case SimType::TensorNetwork: {
            // These families are CPU-only, so `resolved` does not select an
            // implementation here. An explicit GPU request is still a request the
            // factory cannot honour: FullState warns on its fallback while these
            // two dropped it in silence (audit #37). Auto is not warned about —
            // there the user delegated the choice.
            // SparseState is CPU-only, so an explicit GPU request there is a
            // request the factory cannot honour. TensorNetwork is no longer in
            // that class: since it offloads the two-site SVD, so the
            // request IS honoured -- partially, and only above the break-even.
            if (proc == ProcType::GPU && sim_type == SimType::SparseState) {
                log::warn("GPU requested for SparseState but that backend is "
                          "CPU-only; running on CPU.");
            }
            if (sim_type == SimType::SparseState) {
                return std::make_unique<SparseStateBackend>();
            }
            // TN honours an explicit GPU request by offloading the two-site
            // SVD; everything else stays on the host.
            return std::make_unique<TensorNetworkBackend>(proc == ProcType::GPU);
        }
    }
    // Unreachable under the three enum values but required so the function
    // has a defined return path on every control flow. The "FullState"
    // backend literal is a defensive placeholder -- the throw signals a
    // logic error in the dispatcher itself, not a backend failure.
    throw SimulatorError{"FullState", "make_backend: unknown SimType"};
}

}  // namespace qplayer
