// result_access.cpp — SimulatorBackend protected helpers that write into the
// private fields of Result.
// SimulatorBackend is a friend of Result (result.hpp), so these static methods
// can reach counts_ / shots_ / statevector_ directly.  Derived backends (e.g.
// FullStateCpuBackend) call them via the protected interface; the actual write
// to the private member does not require the derived class itself to be a friend.

#include "backend/simulator_backend.hpp"
#include "result.hpp"

#include <utility>   // std::move

namespace qplayer {

// static
void SimulatorBackend::result_set_counts(
        Result& r, std::map<std::string, std::uint64_t> counts) {
    r.counts_ = std::move(counts);
}

// static
void SimulatorBackend::result_set_shots(Result& r, std::uint64_t shots) {
    r.shots_ = shots;
}

// static
void SimulatorBackend::result_set_statevector(Result& r, StateVector sv) {
    r.statevector_ = std::move(sv);
}

// static
void SimulatorBackend::result_set_lazy_sampling(
        Result& r,
        std::uint32_t nshots,
        std::uint64_t seed,
        ClassicalBitIndex total_cubits,
        std::vector<std::pair<QubitIndex, ClassicalBitIndex>> measure_map) {
    r.lazy_     = Result::LazySampling{nshots, seed, total_cubits,
                                       std::move(measure_map), {}};
    r.sampled_  = false;
}

// static
void SimulatorBackend::result_set_lazy_sampling_sparse(
        Result& r,
        std::uint32_t nshots,
        std::uint64_t seed,
        ClassicalBitIndex total_cubits,
        std::vector<std::pair<QubitIndex, ClassicalBitIndex>> measure_map,
        std::vector<std::pair<StateIndex, double>> sparse_probs) {
    r.lazy_     = Result::LazySampling{nshots, seed, total_cubits,
                                       std::move(measure_map),
                                       std::move(sparse_probs)};
    r.sampled_  = false;
}

// static
void SimulatorBackend::result_set_gpu_resident(
        Result& r,
        std::shared_ptr<void> owner,
        const double* d_re,
        const double* d_im,
        StateIndex dim) {
    r.gpu_owner_ = std::move(owner);
    r.gpu_re_    = d_re;
    r.gpu_im_    = d_im;
    r.gpu_dim_   = dim;
}

// static
// CPU-resident SoA handle. Mirrors result_set_gpu_resident; stores
// the CpuStateBuffers shared ownership and cached SoA pointers so Result::
// perform_lazy_sampling() can read prob[i] directly from SoA without first
// building an AoS copy, and statevector() can materialise lazily.
void SimulatorBackend::result_set_cpu_resident(
        Result& r,
        std::shared_ptr<void> owner,
        const double* re,
        const double* im,
        StateIndex dim) {
    r.cpu_owner_ = std::move(owner);
    r.cpu_re_    = re;
    r.cpu_im_    = im;
    r.cpu_dim_   = dim;
}

}  // namespace qplayer
