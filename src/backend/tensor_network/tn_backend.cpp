/// @file tn_backend.cpp
/// @brief TensorNetworkBackend entry points.
///
/// The Interactive surface, the SoA view and nonzero_amplitudes() are *policy*
/// rejections that no future phase will implement (,
/// section 11.7), so they raise SimulatorError rather than a stub marker. The
/// execution surface — prepare / execute / get_statevector / expectation_value —
/// is implemented: the tensor-graph storage arrived in tn_cpu.cpp, the
/// backend was wired onto it with the MPS path, and the Pauli sweep
/// tn_expectation.cpp closed expectation_value (section 11.7).

#include "backend/tensor_network/tn_backend.hpp"

#include "backend/tensor_network/mps.hpp"
#include "backend/tensor_network/tn_fusion.hpp"
#include "backend/tensor_network/tn_measure.hpp"
#include "backend/tensor_network/tn_noise.hpp"
#include "backend/tensor_network/tn_sampling.hpp"
#include "backend/tensor_network/tn_mpo.hpp"
#include "backend/tensor_network/topology.hpp"
#include "backend/memory_check.hpp"
#include "detail/rng.hpp"
#include "noise/noise_model.hpp"
#include "log.hpp"
#include "backend/full_state/gate_fusion.hpp"
#include "backend/full_state/pauli_expectation.hpp"
#include "circuit/circuit.hpp"
#include "detail/noise_schedule.hpp"  // noise_trajectory_canonical_order
#include "detail/noise_site.hpp"       // NoiseSiteCursor
#include "error.hpp"
#include "backend/tensor_network/blas_loader.hpp"
#include "result.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#ifdef _OPENMP
#include <omp.h>
#endif

namespace qplayer {

namespace {
constexpr const char* kBackend      = "TensorNetwork";
constexpr const char* kNoInteractive = "Interactive mode not supported";

// Value of classical register `reg` in this shot, LSB first.
//
// Same reconstruction the statevector per-shot path performs inline
// (full_state_cpu.cpp section 12.4), so `if (c == n)` means the same thing on
// both backends -- an off-by-one in bit order here would branch on a different
// number and look like a physics difference.
std::uint64_t register_value(const ClassicalRegister& reg, const std::vector<int>& cubit_vals) {
    std::uint64_t v = 0;
    for (ClassicalBitIndex b = 0; b < reg.width; ++b) {
        const std::size_t at = static_cast<std::size_t>(reg.offset) + b;
        if (at < cubit_vals.size() && cubit_vals[at] != 0) v |= (std::uint64_t{1} << b);
    }
    return v;
}

}  // namespace

TensorNetworkBackend::TensorNetworkBackend(bool gpu_svd) : gpu_svd_(gpu_svd) {
    // Bring OpenBLAS in now rather than at the first zgesvd_. That
    // call sits inside the trajectory OpenMP region, and dlopen from within a
    // parallel region is a needless hazard; here it is single-threaded, and a
    // missing library is reported when the backend is built rather than mid-run.
    qplayer::detail::ensure_blas_loaded();

    // route, resolved once. Anything other than "mpo" —
    // including a typo — leaves the Monte Carlo default in place with a warning,
    // so a mistyped variable never silently changes which physics ran.
    if (const char* raw = std::getenv("QPLAYER_TN_NOISE_PATH"); raw != nullptr && *raw != '\0') {
        const std::string v{raw};
        if (v == "mpo") {
            use_mpo_ = true;
        } else if (v != "mc") {
            log::warn("QPLAYER_TN_NOISE_PATH: ignoring unknown value '" + v +
                      "' (expected 'mc' or 'mpo'); keeping Monte Carlo");
        }
    }

    // NonLinear site layout, same resolution rule: a typo keeps the
    // default rather than silently running a different layout.
    if (const char* raw = std::getenv("QPLAYER_TN_LAYOUT"); raw != nullptr && *raw != '\0') {
        const std::string v{raw};
        if (v == "minla") {
            nonlinear_layout_ = detail::NonLinearLayout::MinLA;
        } else if (v != "identity") {
            log::warn("QPLAYER_TN_LAYOUT: ignoring unknown value '" + v +
                      "' (expected 'identity' or 'minla'); keeping identity");
        }
    }
}
TensorNetworkBackend::~TensorNetworkBackend() = default;

// Builds the |0...0> MPS the circuit will run on. Resolution order for
// the truncation knobs is defaults -> env -> set_mps_truncation().
void TensorNetworkBackend::prepare(const Circuit& circuit) {
    detail::MpsTruncationConfig cfg = detail::MpsTruncationConfig::from_env();
    if (forced_max_bond_) cfg.max_bond = *forced_max_bond_;
    if (forced_cutoff_)   cfg.cutoff   = *forced_cutoff_;
    if (forced_gauge_)    cfg.gauge    = *forced_gauge_;   //
    cfg.gpu_svd = gpu_svd_;                              //

    num_qubits_ = circuit.num_qubits();

    // Topology drives the layout. A chain-shaped circuit gets
    // its sites ordered along that chain, which is what removes the SWAP walks
    // apply_2q() pays for distant operands.
    topology_ = detail::CircuitTopologyAnalyzer::analyze(circuit, nonlinear_layout_);
    site_of_qubit_.assign(num_qubits_, 0);
    for (std::size_t site = 0; site < topology_.chain_order.size(); ++site) {
        site_of_qubit_[topology_.chain_order[site]] = static_cast<QubitIndex>(site);
    }

    if (topology_.topology == detail::TnTopology::NonLinear && !warned_non_linear_) {
        warned_non_linear_ = true;
        // Honest about the gap: an MPS still gives the RIGHT answer here, it just
        // pays for it in bond dimension. The general TN contraction that would
        // suit this graph is separate work.
        log::warn("TensorNetwork: circuit interaction graph is not a chain (max degree " +
                  std::to_string(topology_.max_degree) +
                  "); running on MPS, which stays exact but may need a large bond dimension");
    }

    state_ = std::make_unique<detail::MpsState>(num_qubits_, cfg);
    trajectory_state_unavailable_ = false;
}

// Reorders amplitudes from site order to qubit order. Site i holds qubit
// chain_order[i], so bit i of a site index belongs at bit chain_order[i]. Without
// this every exported vector would be a silent bit permutation of the truth.
std::vector<std::complex<double>> TensorNetworkBackend::to_qubit_order(
        std::vector<std::complex<double>> site_ordered) const {
    if (topology_.is_identity_order()) return site_ordered;

    std::vector<std::complex<double>> out(site_ordered.size(), std::complex<double>{0, 0});
    for (std::size_t s_idx = 0; s_idx < site_ordered.size(); ++s_idx) {
        std::size_t q_idx = 0;
        for (std::size_t site = 0; site < topology_.chain_order.size(); ++site) {
            if ((s_idx >> site) & 1u) q_idx |= std::size_t{1} << topology_.chain_order[site];
        }
        out[q_idx] = site_ordered[s_idx];
    }
    return out;
}

// State access after a Monte Carlo run has no defined answer: nshots independent
// chains evolved and none of them is "the" state. Registered <P> targets are the
// supported way to get a number out of that run.
void TensorNetworkBackend::require_single_state(const char* what) const {
    if (state_ == nullptr) {
        throw SimulatorError{kBackend, std::string{what} + " before a circuit was prepared"};
    }
    if (trajectory_state_unavailable_) {
        throw SimulatorError{kBackend,
            std::string{what} + " is not defined after a noisy run: each of the nshots"
            " trajectories has its own final state. Register a target with"
            " Circuit::add_expectation() to read an nshot average, or run without a"
            " noise model for single-state access"};
    }
}

std::size_t TensorNetworkBackend::swap_steps() const noexcept {
    return state_ ? state_->swap_steps() : 0;
}

// StateIndex overload: the facade's index space is the statevector's,
// so unpack bit q into qubit q and hand off to the bit-vector form.
std::complex<double> TensorNetworkBackend::amplitude(StateIndex basis) const {
    require_single_state("amplitude");
    if (num_qubits_ < 128 && basis >= (StateIndex{1} << num_qubits_)) {
        throw SimulatorError{kBackend, "amplitude basis index out of range"};
    }
    std::vector<int> bits(num_qubits_, 0);
    for (QubitIndex q = 0; q < num_qubits_; ++q) {
        bits[q] = static_cast<int>((basis >> q) & StateIndex{1});
    }
    return amplitude(bits);
}

// Amplitude of one basis state. The chain stores sites, not qubits, so
// the caller's qubit-indexed bitstring is permuted into site order first — the
// same layout mapping execute() applies to gates.
std::complex<double> TensorNetworkBackend::amplitude(const std::vector<int>& bitstring) const {
    require_single_state("amplitude");
    if (bitstring.size() != num_qubits_) {
        throw SimulatorError{kBackend,
            "amplitude bitstring length " + std::to_string(bitstring.size()) +
            " does not match the circuit width " + std::to_string(num_qubits_)};
    }

    std::vector<int> site_bits(num_qubits_, 0);
    for (QubitIndex q = 0; q < num_qubits_; ++q) site_bits[site_of_qubit_[q]] = bitstring[q];
    return detail::mps_amplitude(state_->graph(), site_bits);
}

// Reduced density matrix over a qubit subset. The requested qubits are
// mapped to sites and the result's bit order is restored to ascending QUBIT
// order — the site order is an internal layout detail the caller never chose.
std::vector<std::complex<double>> TensorNetworkBackend::reduced_density_matrix(
        std::vector<QubitIndex> qubits) const {
    require_single_state("reduced_density_matrix");
    std::sort(qubits.begin(), qubits.end());
    if (std::adjacent_find(qubits.begin(), qubits.end()) != qubits.end()) {
        throw SimulatorError{kBackend, "reduced_density_matrix received a repeated qubit"};
    }
    if (!qubits.empty() && qubits.back() >= num_qubits_) {
        throw SimulatorError{kBackend, "reduced_density_matrix qubit out of range"};
    }

    std::vector<QubitIndex> sites;
    sites.reserve(qubits.size());
    for (QubitIndex q : qubits) sites.push_back(site_of_qubit_[q]);

    // mps_reduced_density_matrix() orders its open bits by ascending SITE, which
    // need not be ascending qubit. Build the permutation that maps its bit i to
    // the caller's bit i, then rewrite both matrix axes.
    std::vector<QubitIndex> sorted_sites = sites;
    std::sort(sorted_sites.begin(), sorted_sites.end());

    const std::vector<std::complex<double>> raw =
        detail::mps_reduced_density_matrix(state_->graph(), sites);

    const std::size_t k = qubits.size();
    if (k == 0) return raw;

    std::vector<std::size_t> caller_bit_of_raw_bit(k, 0);
    for (std::size_t raw_bit = 0; raw_bit < k; ++raw_bit) {
        const QubitIndex site = sorted_sites[raw_bit];
        for (std::size_t caller_bit = 0; caller_bit < k; ++caller_bit) {
            if (sites[caller_bit] == site) { caller_bit_of_raw_bit[raw_bit] = caller_bit; break; }
        }
    }

    const std::size_t dim = std::size_t{1} << k;
    const auto permute = [&](std::size_t raw_index) {
        std::size_t out = 0;
        for (std::size_t bit = 0; bit < k; ++bit) {
            if ((raw_index >> bit) & 1u) out |= std::size_t{1} << caller_bit_of_raw_bit[bit];
        }
        return out;
    };

    std::vector<std::complex<double>> rho(dim * dim, std::complex<double>{0.0, 0.0});
    for (std::size_t r = 0; r < dim; ++r) {
        for (std::size_t c = 0; c < dim; ++c) rho[permute(r) * dim + permute(c)] = raw[r * dim + c];
    }
    return rho;
}

// Runs a circuit on the MPS chain. The op families this rejects are
// rejected explicitly rather than skipped: silently ignoring a Reset or an IfOp
// would return a plausible-looking wrong distribution.
Result TensorNetworkBackend::execute(const Circuit&    circuit,
                                     std::uint32_t     nshots,
                                     const NoiseModel* noise) {
    // Unconditionally, on every entry: execute() starts from |0...0>, so the
    // chain a previous run left behind must not survive into this one. Guarding
    // this on a width change (the shape it had until 2026-09-04) skipped the
    // rebuild whenever the widths matched, and the deferred fast path below then
    // evolved the PREVIOUS run's chain -- a second Bell run reporting a uniform
    // distribution instead of 00/11, with no exception and no warning. The
    // statevector backends call prepare() here for the same reason
    // (full_state_cpu.cpp, sparse_state_backend.cpp), so this is what makes the
    // three agree. Cost is one fresh MpsState plus the topology scan, which the
    // per-shot and trajectory loops below dwarf.
    prepare(circuit);

    if (noise != nullptr) {
        // The two routes. They accept the same channels and
        // differ in kind: trajectories sample, the MPO carries rho exactly.
        if (use_mpo_) {
            // The MPO route evolves one density matrix, so there is no trajectory
            // and no per-shot draw to intervene on.
            if (!noise_sites_.empty()) {
                throw SimulatorError{kBackend,
                    "noise interventions need the Monte Carlo trajectory route; "
                    "the MPO route has no per-shot noise draw"};
            }
            return execute_mpo(circuit, nshots, *noise);
        }
        return execute_trajectories(circuit, nshots, *noise);
    }

    // Deferring measurement is exact only while nothing follows it. Once a gate
    // touches a measured qubit again the collapse changes the distribution, so
    // those circuits take the per-shot path — routed by the same predicate the
    // statevector backends use (section 12.4).
    //
    // Reset routes here too: it is a measurement plus a
    // conditional X, so evolving it ONCE and drawing nshots reports one collapse
    // branch nshots times. That trigger lived in a TN-local predicate until
    // moved it into the shared one, which is why this reads as a single
    // question again.
    if (circuit.requires_per_shot()) return execute_per_shot(circuit, nshots);

    std::vector<std::pair<QubitIndex, ClassicalBitIndex>> measure_map;

    // Fuse first: consecutive gates on the same operands become one
    // matrix, which on an MPS removes a whole merge/SVD/split cycle each time.
    // tn_fuse() also rejects unsupported op families up front, so a circuit that
    // cannot run fails before any amplitude has been touched.
    // With the canonical gauge the order of commuting steps decides how far the
    // centre travels, so they are scheduled to follow it.
    const std::vector<detail::TnStep> steps =
        state_->config().gauge != MpsGauge::None
            ? detail::tn_sweep_order(detail::tn_fuse(circuit.ops()), site_of_qubit_)
            : detail::tn_fuse(circuit.ops());

    for (const detail::TnStep& step : steps) {
        if (step.kind == detail::TnStep::Kind::Measure) {
            // Deferred: sampled from the final state, like the statevector fast
            // path. Sound here because has_classical_control() was false, which
            // means nothing in this circuit reads a measured qubit again.
            measure_map.emplace_back(step.measure.qubit, step.measure.cubit);
            continue;
        }
        if (step.kind == detail::TnStep::Kind::Reset) {
            // Reachable only because requires_per_shot() said no, which means no
            // gate has touched this qubit — so it is still |0> and
            // the reset is the identity. Doing nothing is therefore exact.
            //
            // Verified rather than assumed. This path has one shared state and no
            // per-shot RNG, so a reset that actually had something to collapse
            // would return a plausible wrong distribution rather than fail; if the
            // routing rule is ever loosened, the check catches it right here.
            const std::size_t site = site_of_qubit_[step.qubits[0]];
            const std::vector<std::complex<double>> rho =
                state_->site_marginal(static_cast<QubitIndex>(site));
            const double trace = rho[0].real() + rho[3].real();
            if (!(trace > 0.0) || rho[3].real() / trace > 1e-9) {
                throw SimulatorError{kBackend,
                    "reset reached the deferred path on a qubit that is not |0> "
                    "(P(1) = " + std::to_string(rho[3].real() / trace) +
                    "); Circuit::requires_per_shot() should have routed this "
                    "circuit per shot"};
            }
            continue;
        }
        if (step.kind == detail::TnStep::Kind::If) {
            // Unreachable: an IfOp makes has_classical_control() true, so the
            // circuit left through execute_per_shot() above. Stated rather than
            // assumed, because falling through would apply the body to every shot.
            throw SimulatorError{kBackend,
                "classical conditional reached the deferred path; it should have "
                "routed per shot"};
        }
        if (step.qubits.size() == 3) {
            // No matrix on a 3Q step: the fixed decomposition is chosen from the
            // gate type.
            state_->apply_3q(step.gate,
                             site_of_qubit_[step.qubits[0]],
                             site_of_qubit_[step.qubits[1]],
                             site_of_qubit_[step.qubits[2]]);
        } else if (step.qubits.size() == 1) {
            state_->apply_1q(step.matrix, site_of_qubit_[step.qubits[0]]);
        } else {
            // Sites, not qubits — operand order is preserved, so the matrix's
            // local bit 0 still means the step's first operand.
            state_->apply_2q(step.matrix,
                             site_of_qubit_[step.qubits[0]],
                             site_of_qubit_[step.qubits[1]]);
        }
    }

    Result r;
    result_set_shots(r, nshots);
    result_set_truncation_errors(r, state_->truncation_errors());

    // Sampling reuses the dense deferred draw: contract once, then
    // Result::counts() runs the same inverse-CDF path as FullState, so bit and
    // cubit ordering cannot drift between backends.
    if (num_qubits_ > MAX_DENSE_EXPORT_QUBITS) {
        // Past the dense cap the statevector cannot exist, so counts come from
        // the chain itself. Eager rather than lazy: the deferral
        // attaches a state for counts() to draw from later, and here there is no
        // state to attach — only the sampler, which the backend owns and the
        // Result does not.
        if (!measure_map.empty()) {
            result_set_counts(r, sample_counts_native(*state_, nshots,
                                                      circuit.total_cubits(), measure_map));
        }
        return r;
    }

    result_set_statevector(r, to_qubit_order(state_->to_dense()));
    if (!measure_map.empty()) {
        result_set_lazy_sampling(r, nshots, seed_, circuit.total_cubits(), std::move(measure_map));
    }
    return r;
}

namespace {

// Classical register -> counts key, MSB-first. Both sampling paths below
// build the same key, and Result::counts() builds it identically for the dense
// path, so the three cannot drift.
std::string counts_key_from_cubits(const std::vector<int>& cubit_vals) {
    const std::size_t total_c = cubit_vals.size();
    std::string bs(total_c, '0');
    for (std::size_t k = 0; k < total_c; ++k) {
        bs[k] = static_cast<char>('0' + cubit_vals[total_c - 1u - k]);
    }
    return bs;
}

}  // namespace

// Native MPS sampling for the noiseless path.
//
// The environments are built once and every shot then costs O(n * chi^2), so
// nshots draws are cheap next to the one O(n * chi^3) build — which is the whole
// reason the sampler is an object. Shot seeds are derived per shot index
// rather than taken from one shared stream, so the counts do not
// depend on how many threads drew them.
std::map<std::string, std::uint64_t> TensorNetworkBackend::sample_counts_native(
        const detail::MpsState& state,
        std::uint32_t nshots,
        ClassicalBitIndex total_cubits,
        const std::vector<std::pair<QubitIndex, ClassicalBitIndex>>& measure_map) const {
    std::map<std::string, std::uint64_t> counts;
    if (total_cubits == 0) {
        counts[""] = nshots;
        return counts;
    }

    const detail::MpsSampler sampler(state.graph());

    int threads = 1;
#ifdef _OPENMP
    threads = omp_get_max_threads() > 0 ? omp_get_max_threads() : 1;
#endif
    threads = std::max(1, std::min<int>(threads, static_cast<int>(std::max<std::uint32_t>(nshots, 1))));
    std::vector<std::map<std::string, std::uint64_t>> local(static_cast<std::size_t>(threads));

    std::atomic<bool> error_flag{false};
    std::mutex        error_mutex;
    std::string       error_msg;
    const std::int64_t shots_i = static_cast<std::int64_t>(nshots);

#ifdef _OPENMP
#pragma omp parallel for schedule(static) num_threads(threads)
#endif
    for (std::int64_t shot = 0; shot < shots_i; ++shot) {
        if (error_flag.load(std::memory_order_relaxed)) continue;
        try {
            std::mt19937_64 rng(detail::thread_seed(seed_, static_cast<std::uint32_t>(shot)));
            std::vector<int> site_bits;
            sampler.sample_into(rng, site_bits);

            std::vector<int> cubit_vals(static_cast<std::size_t>(total_cubits), 0);
            for (const auto& [q, c] : measure_map) {
                cubit_vals[static_cast<std::size_t>(c)] = site_bits[site_of_qubit_[q]];
            }
#ifdef _OPENMP
            const int tid = omp_get_thread_num();
#else
            const int tid = 0;
#endif
            ++local[static_cast<std::size_t>(tid) % local.size()]
                   [counts_key_from_cubits(cubit_vals)];
        } catch (const std::exception& ex) {
            if (!error_flag.exchange(true, std::memory_order_acq_rel)) {
                std::lock_guard<std::mutex> lk(error_mutex);
                error_msg = ex.what();
            }
        }
    }

    if (error_flag.load()) throw SimulatorError{kBackend, error_msg};

    for (const auto& lm : local) {
        for (const auto& kv : lm) counts[kv.first] += kv.second;
    }
    return counts;
}

// ── Monte Carlo trajectory path ───────────────────

namespace {

// Ops the noisy path cannot carry yet, checked BEFORE any trajectory starts so a
// rejected circuit costs nothing and no partial state escapes. IfOp is the
// remaining Phase 10-A gap; opaque and 3Q+ are the same gaps the
// noiseless path already reports. Mid-circuit collapse arrived in and
// Reset, so neither is listed here any more.
void reject_unsupported_noisy_ops(const Circuit& circuit) {
    for (const Op& op : circuit.ops()) {
        if (const auto* g = std::get_if<GateOp>(&op)) {
            if (g->is_opaque || g->type == GateType::Opaque) {
                throw SimulatorError{kBackend, "opaque gates are not supported on TensorNetwork yet"};
            }
            if (g->qubits.size() > 3) {
                throw SimulatorError{kBackend,
                    "gates on more than 3 qubits are not supported on TensorNetwork yet"};
            }
        } else if (const auto* cond = std::get_if<IfOp>(&op)) {
            // The body is checked with the same rules as an unconditional gate:
            // whether it runs is a per-shot question, but whether it COULD run is
            // not, and a rejection is cheaper before any trajectory starts.
            const GateOp& body = cond->body;
            if (body.is_opaque || body.type == GateType::Opaque) {
                throw SimulatorError{kBackend, "opaque gates are not supported on TensorNetwork yet"};
            }
            // A 3Q body stays rejected — see tn_fuse() for why.
            if (body.qubits.size() > 2) {
                throw SimulatorError{kBackend,
                    "conditional gates on more than 2 qubits are not supported on "
                    "TensorNetwork yet"};
            }
        }
    }
}

// BLAS thread control for the trajectory region.
//
// This is the first place in the codebase that calls BLAS from inside an OpenMP
// parallel region: each trajectory runs its own zgesvd_. The outer loop already
// saturates the cores and the SVDs are 2*chi x 2*chi, below the BLAS threading
// break-even, so an inner team has no cores to win.
//
// This is NOT a performance claim. At a sane thread count it is neutral (n=20,
// OMP=32: 3165.9 ms vs 3128.0 ms with OPENBLAS_NUM_THREADS=1, inside noise). It
// pays off only where the calling-thread count exceeds OpenBLAS MAX_THREADS —
// 64 on the reference host, against 144 cores — which is the actual cause of the
// "precompiled NUM_THREADS exceeded" warning, a message that misdirects the user
// to rebuild OpenBLAS. The dominant cost is thread count versus per-trajectory
// work, and its optimum reverses with circuit size, so that is, not this.
//
// Resolved through the dlopen handle, not weak-linked: OpenBLAS is no longer a
// link-time dependency, so weak declarations would bind to null
// forever and silently disarm the bound this guard exists to enforce. A null
// return still means "this build does not export it, nothing to do".

namespace {

// OpenBLAS's structural limit on the number of CALLING threads, parsed from its
// config string ("... MAX_THREADS=64"); 0 when unknown.
//
// This is not a tuning knob — it is a hard bound. OpenBLAS keeps a per-calling-
// thread buffer table sized at build time, and driving zgesvd_ from more threads
// than that **segfaults inside the library** (observed: the full noise suite
// crashed in zgemm_itcopy while 144 OMP threads shared a MAX_THREADS=64 build).
// The failure is accumulation-dependent — it needs enough distinct threads to
// have touched BLAS — so a narrow filtered test run can pass while the full suite
// dies, which is exactly why the bound has to be enforced rather than hoped for.
std::uint64_t openblas_calling_thread_limit() {
    const auto get_config = qplayer::detail::blas_get_config_fn();
    if (get_config == nullptr) return 0;
    const char* cfg = get_config();
    if (cfg == nullptr) return 0;
    const std::string text{cfg};
    const std::string key = "MAX_THREADS=";
    const std::size_t at = text.find(key);
    if (at == std::string::npos) return 0;
    std::uint64_t value = 0;
    for (std::size_t i = at + key.size(); i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i) {
        value = value * 10 + static_cast<std::uint64_t>(text[i] - '0');
    }
    return value;
}

// RAII so the restore also happens when a trajectory throws. Always
// constructed OUTSIDE the parallel region, which is what keeps this global poke
// from racing with itself.
class ScopedSingleThreadedBlas {
public:
    ScopedSingleThreadedBlas() {
        set_ = qplayer::detail::blas_set_num_threads_fn();
        const auto get = qplayer::detail::blas_get_num_threads_fn();
        if (get != nullptr && set_ != nullptr) {
            previous_ = get();
            if (previous_ > 1) {
                set_(1);
                restore_ = true;
            }
        }
    }
    ~ScopedSingleThreadedBlas() {
        if (restore_) set_(previous_);
    }
    ScopedSingleThreadedBlas(const ScopedSingleThreadedBlas&)            = delete;
    ScopedSingleThreadedBlas& operator=(const ScopedSingleThreadedBlas&) = delete;

private:
    qplayer::detail::BlasSetNumThreadsFn set_ = nullptr;
    int  previous_ = 1;
    bool restore_  = false;
};

}  // namespace

// Per-trajectory MPS footprint: n sites x (chi x 2 x chi) complex amplitudes.
// 's formula is deliberately NOT reused — it costs a private buffer
// 2^n x 16 B, which for a 100-qubit chain would refuse a run that actually needs
// a few hundred MB. chi_max is the right bound because that is what the truncation
// policy caps the chain, and the SVD scratch is a small multiple
// one site tensor.
std::uint64_t mps_trajectory_bytes(QubitIndex num_qubits, std::uint32_t max_bond,
                                   bool samples_shots) {
    const std::uint64_t chi  = static_cast<std::uint64_t>(max_bond);
    const std::uint64_t site = chi * 2ull * chi * 16ull;      // complex<double>
    const std::uint64_t chain = site * static_cast<std::uint64_t>(num_qubits);
    // Merge buffer + SVD workspace touch a couple of sites at a time; 4x one site
    // is a generous allowance next to the chain itself.
    std::uint64_t bytes = chain + 4ull * site;
    // site_marginal()'s environment cache: one chi x chi matrix per site on each
    // side, half a chain in total. It fills as soon as a 1Q channel
    // or a mid-circuit measurement queries a marginal.
    bytes += 2ull * static_cast<std::uint64_t>(num_qubits) * chi * chi * 16ull;
    // A sampling trajectory also holds one right environment per site —
    // half a chain, since an environment carries no physical leg. Left out, the
    // budget would size the thread team for two thirds of the real footprint.
    if (samples_shots) bytes += detail::mps_sampling_environment_bytes(num_qubits, max_bond);
    return bytes;
}

}  // namespace

Result TensorNetworkBackend::execute_trajectories(const Circuit& circuit,
                                                  std::uint32_t nshots,
                                                  const NoiseModel& noise) {
    reject_unsupported_noisy_ops(circuit);
    // From here on the prepared chain is not the run's state (see
    // require_single_state). Set before any work so an exception mid-run leaves
    // the flag consistent with what state_ actually holds.
    trajectory_state_unavailable_ = true;
    last_traj_bond_ = 0;
    last_traj_norm_drift_ = 0.0;

    // Two measurement regimes on this path.
    //
    // When nothing reads a measured qubit again, the draw stays deferred to the
    // end of the trajectory and is taken natively: no trajectory
    // densifies, so the noisy path has no qubit cap of its own. When something
    // DOES read one again, the deferral is simply wrong -- it hands the later
    // gate an uncollapsed state -- so every MeasureOp collapses in place instead.
    // The predicate is the one the statevector backends use, so a circuit cannot
    // get one convention here and another there.
    const bool midcircuit = circuit.has_classical_control();
    bool has_measure = false;
    std::vector<std::pair<QubitIndex, ClassicalBitIndex>> measure_map;
    std::vector<std::size_t> measure_op_index;   // parallel to measure_map, for the ledger
    for (std::size_t oi = 0; oi < circuit.ops().size(); ++oi) {
        if (const auto* m = std::get_if<MeasureOp>(&circuit.ops()[oi])) {
            has_measure = true;
            if (!midcircuit) {
                measure_map.emplace_back(m->qubit, m->cubit);
                measure_op_index.push_back(oi);
            }
        }
    }

    const detail::MpsTruncationConfig cfg = state_->config();
    const ClassicalBitIndex total_c = circuit.total_cubits();
    const std::int64_t nshots_i = static_cast<std::int64_t>(nshots);
    // IfOp::creg indexes this vector; read-only inside the region.
    const std::vector<ClassicalRegister>& cregs = circuit.classical_registers();

    // Registered <P> targets, compiled to SITE-ordered Pauli factors BEFORE the
    // parallel region: compile_pauli_ops() throws on a malformed string and an
    // exception cannot cross an OMP boundary. Site order because the
    // chain stores sites, the same permutation expectation_value() applies.
    const std::vector<std::string>& exp_targets = circuit.expectation_targets();
    std::vector<std::vector<detail::PauliOp>> exp_site_ops;
    exp_site_ops.reserve(exp_targets.size());
    for (const std::string& pauli : exp_targets) {
        const std::vector<detail::PauliOp> by_qubit =
            detail::compile_pauli_ops(pauli, num_qubits_);
        std::vector<detail::PauliOp> by_site(num_qubits_, detail::PauliOp::I);
        for (QubitIndex q = 0; q < num_qubits_; ++q) by_site[site_of_qubit_[q]] = by_qubit[q];
        exp_site_ops.push_back(std::move(by_site));
    }
    // Flat [target][trajectory]; slot (t, traj) is written by exactly one thread,
    // and the sum below runs in trajectory order so the average does not depend on
    // the team size.
    std::vector<double> exp_samples(exp_site_ops.size() *
                                    static_cast<std::size_t>(std::max<std::int64_t>(nshots_i, 0)),
                                    0.0);

    const bool readout_active = noise.has_readout_errors();

    // Thread team sized by the MPS footprint, not the dense one.
    std::uint64_t traj_threads = 1;
    {
#ifdef _OPENMP
        const std::uint64_t omp_max =
            static_cast<std::uint64_t>(omp_get_max_threads() > 0 ? omp_get_max_threads() : 1);
#else
        const std::uint64_t omp_max = 1;
#endif
        const std::uint64_t per_thread =
            mps_trajectory_bytes(num_qubits_, cfg.max_bond, !measure_map.empty());
        const std::uint64_t want =
            std::min<std::uint64_t>(omp_max, std::max<std::uint64_t>(nshots, 1));
        const std::uint64_t budget = detail::available_memory_bytes();
        const std::uint64_t capacity =
            (budget == UINT64_MAX || per_thread == 0) ? want : budget / per_thread;
        if (capacity == 0) {
            detail::check_memory_budget(per_thread, kBackend, num_qubits_);
        }
        traj_threads = std::min(want, capacity);
        // Hard cap: never drive BLAS from more calling threads than the library
        // was built for. Measured cost of the cap on this host
        // (limit 64 of 144 cores): -4% on a 20-qubit depth-12 run, +2.4x on a
        // 12-qubit one — the bound is close to the granularity sweet spot anyway.
        const std::uint64_t blas_limit = openblas_calling_thread_limit();
        if (blas_limit > 0) traj_threads = std::min(traj_threads, blas_limit);
        if (traj_threads < want && (blas_limit == 0 || traj_threads < blas_limit)) {
            log::warn("TensorNetwork trajectory threads reduced from " + std::to_string(want) +
                      " to " + std::to_string(traj_threads) +
                      " to fit the memory budget (chi_max " + std::to_string(cfg.max_bond) + ")");
        }
    }

    std::map<std::string, std::uint64_t> counts;
    std::mutex counts_mutex;
    std::vector<std::map<std::string, std::uint64_t>> local_counts(
        static_cast<std::size_t>(traj_threads));

    // Truncation ledger: one list per trajectory, of which the WORST is reported.
    // Concatenating all of them would break 's "one entry per 2Q step"
    // shape, and reporting trajectory 0 would understate the run. Worst-case with
    // a lowest-index tie-break is deterministic and is the number a user needs to
    // decide whether to trust the result.
    std::vector<std::vector<double>> traj_errors(static_cast<std::size_t>(nshots_i));

    // Per-trajectory cumulative truncation weight (L1). The worst
    // trajectory's per-step history above answers "is this run exact"; this
    // answers "how far can this trajectory push an ensemble observable", and a
    // mean needs every trajectory, not the worst one.
    std::vector<double> traj_totals(static_cast<std::size_t>(nshots_i), 0.0);

    // Peak chi per trajectory, reported per shot via Result and also
    // max-reduced into last_traj_bond_ for the backend's own use.
    //
    // CORRECTION (this comment was wrong until 2026-09-08): it used
    // read MpsState::max_bond_dimension() at the end of the trajectory, on the
    // stated premise that "chi only ever grows within a run, so the final value
    // IS the peak". It does not. A bond shrinks whenever a later split needs
    // fewer singular values, and two things in these circuits do exactly that --
    // the SWAP round trip inside apply_2q(), and syndrome extraction, where an
    // ancilla is entangled with up to four data qubits and then measured away.
    // Measured on rotated surface code: the run requires chi 8/32/64 at d=3/5/7
    // while the end-of-run value is 4/16/32, so the old read understated the
    // resource by 2x. MpsState::peak_bond_dimension() carries the running max.
    std::vector<std::uint32_t> traj_bonds(static_cast<std::size_t>(nshots_i), 0u);

    // Pre-normalisation |psi|^2 per trajectory. Reported as the WORST deviation
    // from 1, because that is the one that says how far the local rescale drifted
    // — an average would hide it behind the lossless trajectories.
    std::vector<double> traj_norms(static_cast<std::size_t>(nshots_i), 1.0);

    std::atomic<bool> error_flag{false};
    std::mutex error_mutex;
    std::string error_msg;

    // : BLAS threads pinned to 1 for the duration of the region.
    const ScopedSingleThreadedBlas blas_guard;

    // Canonical op order: same permutation as FullState CPU/GPU so the
    // per-trajectory RNG streams are consumed identically.
    // Per-shot noise-site counts and ledgers; one slot per trajectory.
    std::vector<std::uint32_t> traj_site_counts(static_cast<std::size_t>(nshots_i), 0u);
    std::vector<std::vector<NoiseDrawEvent>> traj_ledgers(
        noise_ledger_ ? static_cast<std::size_t>(nshots_i) : std::size_t{0});

    const std::vector<std::size_t> traj_op_order =
        qplayer::detail::noise_trajectory_canonical_order(circuit);

#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic) num_threads(static_cast<int>(traj_threads))
#endif
    for (std::int64_t traj = 0; traj < nshots_i; ++traj) {
        if (error_flag.load(std::memory_order_relaxed)) continue;
        try {
            detail::MpsState state(num_qubits_, cfg);
            std::mt19937_64 rng(
                detail::thread_seed(seed_, static_cast<std::uint32_t>(traj)));
            std::vector<KrausChannel> chan_buf;
            // Noise-site hook: every NoiseModel draw goes
            // noise_site(), which draws first and skips only the channel when the
            // site is intervened, so later draws never shift.
            detail::NoiseSiteCursor site_cur(
                noise_sites_,
                noise_ledger_ ? &traj_ledgers[static_cast<std::size_t>(traj)] : nullptr);
            std::uniform_real_distribution<double> site_u01(0.0, 1.0);
            const auto noise_site = [&](const KrausChannel& ch, std::size_t op_idx) {
                const double u = site_u01(rng);
                if (!site_cur.next(op_idx, u))
                    site_cur.set_branch(detail::apply_kraus_channel_u(state, ch, u));
            };
            // Written by the in-place collapse below; left all zero on the
            // deferred route, where the draw fills it after the op loop.
            std::vector<int> cubit_vals(static_cast<std::size_t>(total_c), 0);

            for (std::size_t traj_op_idx : traj_op_order) {
                const Op& op = circuit.ops()[traj_op_idx];
                if (const auto* g = std::get_if<GateOp>(&op)) {
                    if (g->qubits.empty()) continue;
                    if (g->type == GateType::Reset) {
                        // Non-unitary, so it has no matrix to fetch. This
                        // is the op every QEC round needs: the ancilla is reused
                        // each round, and without it a surface-code circuit cannot
                        // run on ANY noise-capable backend (FullState GPU still
                        // cannot, which is what keeps verify-sc17-willow on CPU).
                        detail::mps_reset_site(state, site_of_qubit_[g->qubits[0]], rng);
                        // Apply reset channel.
                        if (noise.has_channels_for_gate("reset")) {
                            noise.channels_for_gate("reset", g->qubits, chan_buf);
                            for (const KrausChannel& ch : chan_buf) {
                                KrausChannel sited = ch;
                                sited.target_qubit  = site_of_qubit_[ch.target_qubit];
                                sited.target_qubit2 = site_of_qubit_[ch.target_qubit2];
                                noise_site(sited, traj_op_idx);
                            }
                        }
                        continue;
                    }
                    // 3Q gates never build a matrix: apply_3q() picks a fixed
                    // decomposition from the gate type.
                    if (g->qubits.size() == 3) {
                        state.apply_3q(g->type, site_of_qubit_[g->qubits[0]],
                                                site_of_qubit_[g->qubits[1]],
                                                site_of_qubit_[g->qubits[2]]);
                    } else {
                        const std::vector<std::complex<double>> u = detail::gate_full_matrix(*g);
                        if (g->qubits.size() == 1) {
                            state.apply_1q(u, site_of_qubit_[g->qubits[0]]);
                        } else {
                            state.apply_2q(u, site_of_qubit_[g->qubits[0]],
                                              site_of_qubit_[g->qubits[1]]);
                        }
                    }
                    // Gate-attached channels (..06). The out-parameter
                    // overload keeps its capacity across gates.
                    if (noise.has_channels_for_gate(g->name)) {
                        noise.channels_for_gate(g->name, g->qubits, chan_buf);
                        for (const KrausChannel& ch : chan_buf) {
                            KrausChannel sited = ch;
                            sited.target_qubit  = site_of_qubit_[ch.target_qubit];
                            sited.target_qubit2 = site_of_qubit_[ch.target_qubit2];
                            noise_site(sited, traj_op_idx);
                        }
                    }
                } else if (const auto* idle = std::get_if<IdleOp>(&op)) {
                    // The dispatch slot registered, now carrying relaxation
                    //
                    for (const KrausChannel& ch :
                             noise.idle_channels(idle->qubit, idle->duration_ns)) {
                        KrausChannel sited = ch;
                        sited.target_qubit  = site_of_qubit_[ch.target_qubit];
                        sited.target_qubit2 = site_of_qubit_[ch.target_qubit2];
                        noise_site(sited, traj_op_idx);
                    }
                } else if (const auto* m = std::get_if<MeasureOp>(&op)) {
                    // Only reached when the circuit reuses a measured qubit;
                    // otherwise measure_map above carries the deferred draw and
                    // this op changes nothing.
                    if (midcircuit) {
                        const int outcome = detail::mps_measure_site(
                            state, site_of_qubit_[m->qubit], rng);
                        // Readout confusion flips the REPORTED bit only
                        //: the collapse is the true one, and a classical
                        // controller reading this register sees the noisy value --
                        // which is exactly what a later IfOp should branch on.
                        int reported = outcome;
                        if (readout_active) {
                            const auto [p01, p10] = noise.readout_error(m->qubit);
                            if (p01 > 0.0 || p10 > 0.0) {
                                // A readout confusion is a noise site too.
                                std::uniform_real_distribution<double> u01(0.0, 1.0);
                                const double u_ro = u01(rng);
                                if (!site_cur.next(traj_op_idx, u_ro)) {
                                    reported = apply_readout_error(outcome, p01, p10, u_ro);
                                    site_cur.set_branch(reported != outcome ? 1u : 0u);
                                }
                            }
                        }
                        if (m->cubit < total_c) {
                            cubit_vals[static_cast<std::size_t>(m->cubit)] = reported;
                        }
                    }
                } else if (const auto* cond = std::get_if<IfOp>(&op)) {
                    // Branch on THIS trajectory's classical bits. The
                    // noise draws and the branch therefore share one shot
                    // context, which is what section 12.4 requires of a noisy
                    // conditional circuit.
                    if (cond->creg >= cregs.size()) continue;
                    if (register_value(cregs[cond->creg], cubit_vals) != cond->value) continue;
                    const GateOp& body = cond->body;
                    if (body.qubits.empty()) continue;
                    if (body.type == GateType::Reset) {
                        detail::mps_reset_site(state, site_of_qubit_[body.qubits[0]], rng);
                        // Apply reset channel (— same as unconditional path).
                        if (noise.has_channels_for_gate("reset")) {
                            noise.channels_for_gate("reset", body.qubits, chan_buf);
                            for (const KrausChannel& ch : chan_buf) {
                                KrausChannel sited = ch;
                                sited.target_qubit  = site_of_qubit_[ch.target_qubit];
                                sited.target_qubit2 = site_of_qubit_[ch.target_qubit2];
                                noise_site(sited, traj_op_idx);
                            }
                        }
                        continue;
                    }
                    const std::vector<std::complex<double>> bu = detail::gate_full_matrix(body);
                    if (body.qubits.size() == 1) {
                        state.apply_1q(bu, site_of_qubit_[body.qubits[0]]);
                    } else {
                        state.apply_2q(bu, site_of_qubit_[body.qubits[0]],
                                           site_of_qubit_[body.qubits[1]]);
                    }
                    // Gate-attached channels fire only when the body actually ran
                    // -- a conditional gate that did not execute cannot have
                    // decohered the qubit it did not touch (..06).
                    if (noise.has_channels_for_gate(body.name)) {
                        noise.channels_for_gate(body.name, body.qubits, chan_buf);
                        for (const KrausChannel& ch : chan_buf) {
                            KrausChannel sited = ch;
                            sited.target_qubit  = site_of_qubit_[ch.target_qubit];
                            sited.target_qubit2 = site_of_qubit_[ch.target_qubit2];
                            noise_site(sited, traj_op_idx);
                        }
                    }
                }
                // BarrierOp: no state change on this path.
            }

            // The sampler's environments ARE the norm contraction, so when a
            // shot is drawn the norm comes off them for free — and at
            // O(n * chi^3) instead of squared_norm()'s O(n * chi^4), which is
            // what makes a 100-qubit trajectory finish at all.
            std::optional<detail::MpsSampler> sampler;
            if (!measure_map.empty()) sampler.emplace(state.graph());

            // guard, split by whether truncation actually lost weight
            // The per-gate rescale is LOCAL, so on a lossy run the
            // global norm drifts as an accuracy symptom — treating that as
            // divergence made every capped large circuit throw, which is the
            // regime this backend exists for. With no loss the old 1e-6 threshold
            // stands: a drift there is a bug, not an approximation.
            const double norm = sampler ? sampler->squared_norm() : state.squared_norm();
            const double eps_total = state.total_truncation_error();
            const bool lossy = eps_total > 0.0;
            const bool diverged =
                !std::isfinite(norm) || norm <= 0.0 ||
                (lossy ? (norm < 1e-3 || norm > 1e3) : std::abs(norm - 1.0) > 1e-6);
            if (diverged) {
                throw SimulatorError{kBackend,
                    "numerical divergence at trajectory " + std::to_string(traj) +
                    ": |psi|^2 = " + std::to_string(norm) +
                    " (accumulated truncation error " + std::to_string(eps_total) + ")"};
            }

            traj_errors[static_cast<std::size_t>(traj)] = state.truncation_errors();
            traj_totals[static_cast<std::size_t>(traj)] = state.total_truncation_error_l1();
            traj_bonds[static_cast<std::size_t>(traj)]  = state.peak_bond_dimension();
            traj_norms[static_cast<std::size_t>(traj)]  = norm;

            // One shot drawn from this trajectory's final state — natively, so
            // the 2^n vector this used to build is never allocated.
            //
            // The draw comes BEFORE the normalisation below on purpose: the
            // sampler's environments are a snapshot of the chain (tn_sampling.hpp)
            // and rescaling a site behind its back would break that contract. The
            // draw itself does not care, because every site conditional is
            // normalised by p0 + p1 and is therefore scale invariant.
            std::string bs(static_cast<std::size_t>(total_c), '0');
            if (sampler) {
                std::vector<int> site_bits;
                sampler->sample_into(rng, site_bits);
                std::uniform_real_distribution<double> u01(0.0, 1.0);
                for (std::size_t mi = 0; mi < measure_map.size(); ++mi) {
                    const auto [q, c] = measure_map[mi];
                    const int outcome = site_bits[site_of_qubit_[q]];
                    // Readout confusion: the sampled basis
                    // state IS the true collapse, and only the reported classical
                    // bit is flipped. Applying it to the state instead would
                    // change later reads of the same qubit, which is the whole
                    // distinction the requirement draws.
                    int reported = outcome;
                    if (readout_active) {
                        const auto [p01, p10] = noise.readout_error(q);
                        if (p01 > 0.0 || p10 > 0.0) {
                            // A readout confusion is a noise site too.
                            const double u_ro = u01(rng);
                            if (!site_cur.next(measure_op_index[mi], u_ro)) {
                                reported = apply_readout_error(outcome, p01, p10, u_ro);
                                site_cur.set_branch(reported != outcome ? 1u : 0u);
                            }
                        }
                    }
                    cubit_vals[static_cast<std::size_t>(c)] = reported;
                }
                bs = counts_key_from_cubits(cubit_vals);
            } else if (midcircuit) {
                // Already collapsed in place; cubit_vals is the whole record.
                bs = counts_key_from_cubits(cubit_vals);
            }

            // Restore |psi|^2 = 1. Free in the common case — a
            // lossless run is already at 1 — and it is what makes the "state is
            // used as stored" convention of mps_expectation_value() true again.
            // Without it a capped run reports <P> scaled by the drift, which at
            // chi_max 16 was measured at 1.59: a 59% systematic error in exactly
            // the regime that has no FullState reference to catch it.
            if (std::abs(norm - 1.0) > 1e-12) state.rescale(1.0 / std::sqrt(norm));

            // This trajectory's contribution to each registered <P>
            // mps_expectation_value() is a contraction, so this works
            // qubit counts where the dense sampling above is impossible.
            for (std::size_t t = 0; t < exp_site_ops.size(); ++t) {
                exp_samples[t * static_cast<std::size_t>(nshots) +
                            static_cast<std::size_t>(traj)] =
                    detail::mps_expectation_value(state.graph(), exp_site_ops[t]);
            }

#ifdef _OPENMP
            const int tid = omp_get_thread_num();
#else
            const int tid = 0;
#endif
            traj_site_counts[static_cast<std::size_t>(traj)] = site_cur.count();
            if (tid >= 0 && static_cast<std::size_t>(tid) < local_counts.size()) {
                ++local_counts[static_cast<std::size_t>(tid)][bs];
            } else {
                std::lock_guard<std::mutex> lk(counts_mutex);
                ++counts[bs];
            }
        } catch (const std::exception& ex) {
            if (!error_flag.exchange(true, std::memory_order_acq_rel)) {
                std::lock_guard<std::mutex> lk(error_mutex);
                error_msg = ex.what();
            }
        }
    }

    if (error_flag.load()) throw SimulatorError{kBackend, error_msg};
    if (const std::int64_t bad = detail::first_unreached_site(noise_sites_, traj_site_counts);
        bad >= 0) {
        throw SimulatorError{kBackend,
            "noise intervention site " + std::to_string(bad) +
            " is beyond the noise draws this run made"};
    }

    for (const auto& lm : local_counts) {
        for (const auto& kv : lm) counts[kv.first] += kv.second;
    }

    Result r;
    result_set_noise_draws(r, noise_sites_, std::move(traj_ledgers));
    result_set_shots(r, nshots);
    if (has_measure) result_set_counts(r, std::move(counts));

    // Worst trajectory by accumulated error, lowest index breaking ties.
    std::size_t worst = 0;
    double worst_total = -1.0;
    for (std::size_t t = 0; t < traj_errors.size(); ++t) {
        double sum_sq = 0.0;
        for (double e : traj_errors[t]) sum_sq += e * e;
        if (sum_sq > worst_total) { worst_total = sum_sq; worst = t; }
    }
    if (!traj_errors.empty()) result_set_truncation_errors(r, traj_errors[worst]);

    last_traj_bond_ = 0;
    for (std::uint32_t b : traj_bonds) last_traj_bond_ = std::max(last_traj_bond_, b);

    std::vector<double> traj_drifts(traj_norms.size());
    last_traj_norm_drift_ = 0.0;
    for (std::size_t i = 0; i < traj_norms.size(); ++i) {
        traj_drifts[i] = std::abs(traj_norms[i] - 1.0);
        last_traj_norm_drift_ = std::max(last_traj_norm_drift_, traj_drifts[i]);
    }
    // The per-shot record, not just the max: a max cannot tell "every
    // shot stayed bounded" from "one shot spiked", and that distinction is the
    // content of the claim this diagnostic exists to check.
    result_set_trajectory_diagnostics(r, traj_bonds, std::move(traj_drifts),
                                      std::move(traj_totals));
    if (last_traj_norm_drift_ > 1e-6) {
        log::warn("MPS truncation drifted the trajectory norm by up to " +
                  std::to_string(last_traj_norm_drift_) +
                  "; the state was renormalised before reading observables, but the "
                  "accuracy loss stands — inspect Result::truncation_errors() "
                  "(chi reached " + std::to_string(last_traj_bond_) + ")");
    }

    // nshot average per target, summed in trajectory order. A
    // non-empty dict is also how QRegister::run() knows the backend already did
    // the work and skips its own single-state evaluation.
    if (!exp_site_ops.empty()) {
        std::map<std::string, double> exp_avg;
        for (std::size_t t = 0; t < exp_site_ops.size(); ++t) {
            double acc = 0.0;
            for (std::uint32_t sh = 0; sh < nshots; ++sh) {
                acc += exp_samples[t * static_cast<std::size_t>(nshots) + sh];
            }
            exp_avg[exp_targets[t]] = (nshots > 0) ? acc / static_cast<double>(nshots) : 0.0;
        }
        result_set_expectations(r, std::move(exp_avg));
    }
    return r;
}

// -- Per-shot path for mid-circuit measurement ------------
//
// Same shape as the trajectory loop above and for the same reason: once a
// collapse can happen mid-circuit the shots stop sharing a final state, so each
// one has to evolve its own chain. What differs is that fusion IS used here --
// there is no noise model, so no step needs a gate name, and tn_fuse() already
// breaks its runs at MeasureOp so no fused matrix straddles
// a collapse. The fused stream is built once and replayed by every shot.
Result TensorNetworkBackend::execute_per_shot(const Circuit& circuit, std::uint32_t nshots) {
    // Rejects IfOp, opaque and 3Q+ up front, before any shot starts.
    // Measure/reset/conditional steps bound tn_sweep_order()'s reordering, so each
    // shot's collapses consume its RNG in the original order.
    const std::vector<detail::TnStep> steps =
        state_->config().gauge != MpsGauge::None
            ? detail::tn_sweep_order(detail::tn_fuse(circuit.ops()), site_of_qubit_)
            : detail::tn_fuse(circuit.ops());

    // nshots chains evolve and none of them is "the" state, so state access must
    // refuse afterwards exactly as it does after trajectories. Set
    // before any work so an exception mid-run leaves the flag consistent.
    trajectory_state_unavailable_ = true;
    last_traj_bond_ = 0;
    last_traj_norm_drift_ = 0.0;

    const detail::MpsTruncationConfig cfg = state_->config();
    const ClassicalBitIndex total_c  = circuit.total_cubits();
    const std::int64_t      nshots_i = static_cast<std::int64_t>(nshots);
    // Bound outside the parallel region: IfOp::creg indexes this vector, and the
    // shot loop only ever reads it.
    const std::vector<ClassicalRegister>& cregs = circuit.classical_registers();

    bool has_measure = false;
    for (const detail::TnStep& step : steps) {
        if (step.kind == detail::TnStep::Kind::Measure) { has_measure = true; break; }
    }

    // Registered <P> targets, compiled to SITE order BEFORE the parallel region:
    // compile_pauli_ops() throws on a malformed string and an exception cannot
    // cross an OMP boundary.
    const std::vector<std::string>& exp_targets = circuit.expectation_targets();
    std::vector<std::vector<detail::PauliOp>> exp_site_ops;
    exp_site_ops.reserve(exp_targets.size());
    for (const std::string& pauli : exp_targets) {
        const std::vector<detail::PauliOp> by_qubit =
            detail::compile_pauli_ops(pauli, num_qubits_);
        std::vector<detail::PauliOp> by_site(num_qubits_, detail::PauliOp::I);
        for (QubitIndex q = 0; q < num_qubits_; ++q) by_site[site_of_qubit_[q]] = by_qubit[q];
        exp_site_ops.push_back(std::move(by_site));
    }
    // Flat [target][shot]; slot (t, shot) is written by exactly one thread and
    // summed in shot order, so the average does not depend on the team size.
    std::vector<double> exp_samples(exp_site_ops.size() *
                                    static_cast<std::size_t>(std::max<std::int64_t>(nshots_i, 0)),
                                    0.0);

    // Thread team sized by the MPS footprint. samples_shots is false: the
    // collapse is inline, so this path never builds a sampler's environments.
    std::uint64_t shot_threads = 1;
    {
#ifdef _OPENMP
        const std::uint64_t omp_max =
            static_cast<std::uint64_t>(omp_get_max_threads() > 0 ? omp_get_max_threads() : 1);
#else
        const std::uint64_t omp_max = 1;
#endif
        const std::uint64_t per_thread =
            mps_trajectory_bytes(num_qubits_, cfg.max_bond, /*samples_shots=*/false);
        const std::uint64_t want =
            std::min<std::uint64_t>(omp_max, std::max<std::uint64_t>(nshots, 1));
        const std::uint64_t budget = detail::available_memory_bytes();
        const std::uint64_t capacity =
            (budget == UINT64_MAX || per_thread == 0) ? want : budget / per_thread;
        if (capacity == 0) {
            detail::check_memory_budget(per_thread, kBackend, num_qubits_);
        }
        shot_threads = std::min(want, capacity);
        // Never drive BLAS from more calling threads than it was built for
        // -- the 2Q gate path runs zgesvd_ inside this region.
        const std::uint64_t blas_limit = openblas_calling_thread_limit();
        if (blas_limit > 0) shot_threads = std::min(shot_threads, blas_limit);
        if (shot_threads < want && (blas_limit == 0 || shot_threads < blas_limit)) {
            log::warn("TensorNetwork per-shot threads reduced from " + std::to_string(want) +
                      " to " + std::to_string(shot_threads) +
                      " to fit the memory budget (chi_max " + std::to_string(cfg.max_bond) + ")");
        }
    }

    std::map<std::string, std::uint64_t> counts;
    std::mutex counts_mutex;
    std::vector<std::map<std::string, std::uint64_t>> local_counts(
        static_cast<std::size_t>(shot_threads));

    // Worst shot by accumulated error is the one reported, for the same reason
    // the trajectory path reports the worst trajectory (shape).
    std::vector<std::vector<double>> shot_errors(static_cast<std::size_t>(nshots_i));
    // Cumulative L1 per shot, same role as traj_totals above.
    std::vector<double>              shot_totals(static_cast<std::size_t>(nshots_i), 0.0);
    // Per-shot peak chi and pre-normalisation norm, reported via Result
    // Peak, not end-of-run -- see the correction note on the
    // trajectory path above.
    std::vector<std::uint32_t>       shot_bonds(static_cast<std::size_t>(nshots_i), 0u);
    std::vector<double>              shot_norms(static_cast<std::size_t>(nshots_i), 1.0);

    std::atomic<bool> error_flag{false};
    std::mutex        error_mutex;
    std::string       error_msg;

    // : BLAS threads pinned to 1 for the duration of the region.
    const ScopedSingleThreadedBlas blas_guard;

#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic) num_threads(static_cast<int>(shot_threads))
#endif
    for (std::int64_t shot = 0; shot < nshots_i; ++shot) {
        if (error_flag.load(std::memory_order_relaxed)) continue;
        try {
            detail::MpsState state(num_qubits_, cfg);
            // Shot seed derived from the shot INDEX, so counts do not depend on
            // how many threads drew them.
            std::mt19937_64 rng(detail::thread_seed(seed_, static_cast<std::uint32_t>(shot)));
            std::vector<int> cubit_vals(static_cast<std::size_t>(total_c), 0);

            for (const detail::TnStep& step : steps) {
                if (step.kind == detail::TnStep::Kind::Measure) {
                    // Born-rule collapse HERE rather than at the end -- the whole
                    // point of this path.
                    const int outcome = detail::mps_measure_site(
                        state, site_of_qubit_[step.measure.qubit], rng);
                    if (step.measure.cubit < total_c) {
                        cubit_vals[static_cast<std::size_t>(step.measure.cubit)] = outcome;
                    }
                    continue;
                }
                if (step.kind == detail::TnStep::Kind::Reset) {
                    // Collapse + conditional X, drawn per shot so an entangled
                    // reset leaves each shot's partners in their own branch
                    // No classical bit is written.
                    detail::mps_reset_site(state, site_of_qubit_[step.qubits[0]], rng);
                    continue;
                }
                if (step.kind == detail::TnStep::Kind::If) {
                    // Branch on THIS shot's classical bits.
                    // Nothing is evaluated ahead of time: the register only has a
                    // value once this shot's measurements have run, which is the
                    // whole reason a conditional circuit cannot use the fast path.
                    if (step.creg >= cregs.size()) continue;
                    if (register_value(cregs[step.creg], cubit_vals) != step.cond_value) continue;
                    if (step.body_kind == detail::TnStep::Kind::Reset) {
                        detail::mps_reset_site(state, site_of_qubit_[step.qubits[0]], rng);
                    } else if (step.qubits.size() == 1) {
                        state.apply_1q(step.matrix, site_of_qubit_[step.qubits[0]]);
                    } else {
                        state.apply_2q(step.matrix,
                                       site_of_qubit_[step.qubits[0]],
                                       site_of_qubit_[step.qubits[1]]);
                    }
                    continue;
                }
                if (step.qubits.size() == 3) {
                    state.apply_3q(step.gate,
                                   site_of_qubit_[step.qubits[0]],
                                   site_of_qubit_[step.qubits[1]],
                                   site_of_qubit_[step.qubits[2]]);
                } else if (step.qubits.size() == 1) {
                    state.apply_1q(step.matrix, site_of_qubit_[step.qubits[0]]);
                } else {
                    state.apply_2q(step.matrix,
                                   site_of_qubit_[step.qubits[0]],
                                   site_of_qubit_[step.qubits[1]]);
                }
            }

            // guard, split by whether truncation actually lost weight
            // -- identical policy to the trajectory path.
            const double norm      = state.squared_norm();
            const double eps_total = state.total_truncation_error();
            const bool   lossy     = eps_total > 0.0;
            const bool   diverged =
                !std::isfinite(norm) || norm <= 0.0 ||
                (lossy ? (norm < 1e-3 || norm > 1e3) : std::abs(norm - 1.0) > 1e-6);
            if (diverged) {
                throw SimulatorError{kBackend,
                    "numerical divergence at shot " + std::to_string(shot) +
                    ": |psi|^2 = " + std::to_string(norm) +
                    " (accumulated truncation error " + std::to_string(eps_total) + ")"};
            }

            shot_errors[static_cast<std::size_t>(shot)] = state.truncation_errors();
            shot_totals[static_cast<std::size_t>(shot)] = state.total_truncation_error_l1();
            shot_bonds[static_cast<std::size_t>(shot)]  = state.peak_bond_dimension();
            shot_norms[static_cast<std::size_t>(shot)]  = norm;

            // Restore |psi|^2 = 1 before reading observables.
            if (std::abs(norm - 1.0) > 1e-12) state.rescale(1.0 / std::sqrt(norm));

            for (std::size_t t = 0; t < exp_site_ops.size(); ++t) {
                exp_samples[t * static_cast<std::size_t>(nshots) +
                            static_cast<std::size_t>(shot)] =
                    detail::mps_expectation_value(state.graph(), exp_site_ops[t]);
            }

            const std::string bs = counts_key_from_cubits(cubit_vals);
#ifdef _OPENMP
            const int tid = omp_get_thread_num();
#else
            const int tid = 0;
#endif
            if (tid >= 0 && static_cast<std::size_t>(tid) < local_counts.size()) {
                ++local_counts[static_cast<std::size_t>(tid)][bs];
            } else {
                std::lock_guard<std::mutex> lk(counts_mutex);
                ++counts[bs];
            }
        } catch (const std::exception& ex) {
            if (!error_flag.exchange(true, std::memory_order_acq_rel)) {
                std::lock_guard<std::mutex> lk(error_mutex);
                error_msg = ex.what();
            }
        }
    }

    if (error_flag.load()) throw SimulatorError{kBackend, error_msg};

    for (const auto& lm : local_counts) {
        for (const auto& kv : lm) counts[kv.first] += kv.second;
    }

    Result r;
    result_set_shots(r, nshots);
    if (has_measure) result_set_counts(r, std::move(counts));

    std::size_t worst = 0;
    double worst_total = -1.0;
    for (std::size_t t = 0; t < shot_errors.size(); ++t) {
        double sum_sq = 0.0;
        for (double e : shot_errors[t]) sum_sq += e * e;
        if (sum_sq > worst_total) { worst_total = sum_sq; worst = t; }
    }
    if (!shot_errors.empty()) result_set_truncation_errors(r, shot_errors[worst]);

    for (std::uint32_t b : shot_bonds) last_traj_bond_ = std::max(last_traj_bond_, b);

    std::vector<double> shot_drifts(shot_norms.size());
    for (std::size_t i = 0; i < shot_norms.size(); ++i) {
        shot_drifts[i] = std::abs(shot_norms[i] - 1.0);
        last_traj_norm_drift_ = std::max(last_traj_norm_drift_, shot_drifts[i]);
    }
    result_set_trajectory_diagnostics(r, shot_bonds, std::move(shot_drifts),
                                      std::move(shot_totals));
    if (last_traj_norm_drift_ > 1e-6) {
        log::warn("MPS truncation drifted the per-shot norm by up to " +
                  std::to_string(last_traj_norm_drift_) +
                  "; the state was renormalised before reading observables, but the "
                  "accuracy loss stands -- inspect Result::truncation_errors() "
                  "(chi reached " + std::to_string(last_traj_bond_) + ")");
    }

    // nshot average per target, summed in shot order.
    if (!exp_site_ops.empty()) {
        std::map<std::string, double> exp_avg;
        for (std::size_t t = 0; t < exp_site_ops.size(); ++t) {
            double acc = 0.0;
            for (std::uint32_t sh = 0; sh < nshots; ++sh) {
                acc += exp_samples[t * static_cast<std::size_t>(nshots) + sh];
            }
            exp_avg[exp_targets[t]] = (nshots > 0) ? acc / static_cast<double>(nshots) : 0.0;
        }
        result_set_expectations(r, std::move(exp_avg));
    }
    return r;
}
// ── MPO density-matrix path ─────────────────────

Result TensorNetworkBackend::execute_mpo(const Circuit& circuit,
                                         std::uint32_t nshots,
                                         const NoiseModel& noise) {
    reject_unsupported_noisy_ops(circuit);
    // The MPO path has no 3Q application (MpoState offers 1Q/2Q unitaries only),
    // so 's relaxation is MPS-only. Reject here rather than letting a
    // 64-entry matrix reach apply_unitary_2q and surface as an arity complaint.
    for (const Op& op : circuit.ops()) {
        if (const auto* g = std::get_if<GateOp>(&op)) {
            if (g->qubits.size() > 2) {
                throw SimulatorError{kBackend,
                    "gates on more than 2 qubits are not supported on the "
                    "TensorNetwork MPO path; use the Monte Carlo path"};
            }
        }
    }
    // The Monte Carlo route collapses a pure chain in place; rho cannot
    // borrow that. Projecting a density matrix touches the ket and bra legs of a
    // site together and renormalises by Tr(P rho), which is a global read -- a
    // separate piece of work, not a reuse. Deferring instead would hand the
    // following gate an unprojected rho and return a plausible wrong answer, so
    // this refuses rather than approximates.
    if (circuit.requires_per_shot()) {
        throw SimulatorError{kBackend,
            "mid-circuit measurement, Reset and classical conditionals are not"
            " supported on the TensorNetwork MPO route; use the Monte Carlo route"
            " (QPLAYER_TN_NOISE_PATH=mc or set_tn_noise_path(\"mc\")), which"
            " collapses the chain in place and carries a per-shot classical register"};
    }
    // rho is not a state vector, so the state-access family must refuse after
    // this run for the same reason it does after trajectories — there is no
    // single |psi> to hand back, and returning the prepared |0...0> would be a
    // plausible-looking wrong answer.
    trajectory_state_unavailable_ = true;

    detail::MpoTruncationConfig cfg = detail::MpoTruncationConfig::from_env();
    if (forced_mpo_max_bond_) cfg.max_bond = *forced_mpo_max_bond_;
    if (forced_mpo_cutoff_)   cfg.cutoff   = *forced_mpo_cutoff_;

    detail::MpoState rho(num_qubits_, cfg);

    // One exact pass. Fusion is skipped for the same reason the trajectory path
    // skips it: a fused matrix has no gate name, and channels_for_gate() is
    // keyed by name.
    std::vector<KrausChannel> chan_buf;
    std::vector<std::pair<QubitIndex, ClassicalBitIndex>> measure_map;
    for (const Op& op : circuit.ops()) {
        if (const auto* g = std::get_if<GateOp>(&op)) {
            if (g->qubits.empty()) continue;
            const std::vector<std::complex<double>> u = detail::gate_full_matrix(*g);
            if (g->qubits.size() == 1) {
                rho.apply_unitary_1q(u, site_of_qubit_[g->qubits[0]]);
            } else {
                rho.apply_unitary_2q(u, site_of_qubit_[g->qubits[0]],
                                        site_of_qubit_[g->qubits[1]]);
            }
            if (noise.has_channels_for_gate(g->name)) {
                noise.channels_for_gate(g->name, g->qubits, chan_buf);
                for (const KrausChannel& ch : chan_buf) {
                    // Applied in FULL — every Kraus operator contributes, which
                    // is the difference from the trajectory path picking one.
                    if (ch.operators.empty()) continue;
                    if (ch.operators.front().size() == 2) {
                        rho.apply_channel_1q(ch.operators, site_of_qubit_[ch.target_qubit]);
                    } else {
                        rho.apply_channel_2q(ch.operators, site_of_qubit_[ch.target_qubit],
                                                           site_of_qubit_[ch.target_qubit2]);
                    }
                }
            }
        } else if (const auto* idle = std::get_if<IdleOp>(&op)) {
            for (const KrausChannel& ch : noise.idle_channels(idle->qubit, idle->duration_ns)) {
                if (ch.operators.empty()) continue;
                if (ch.operators.front().size() == 2) {
                    rho.apply_channel_1q(ch.operators, site_of_qubit_[ch.target_qubit]);
                } else {
                    rho.apply_channel_2q(ch.operators, site_of_qubit_[ch.target_qubit],
                                                       site_of_qubit_[ch.target_qubit2]);
                }
            }
        } else if (const auto* m = std::get_if<MeasureOp>(&op)) {
            measure_map.emplace_back(m->qubit, m->cubit);
        }
    }

    last_mpo_bond_ = rho.max_bond_dimension();

    // health check. Channels are trace preserving and truncation
    // renormalises, so a deviation here means divergence rather than accuracy
    // loss — accuracy is reported through truncation_errors().
    const double tr = rho.trace();
    if (!std::isfinite(tr) || std::abs(tr - 1.0) > 1e-6) {
        throw SimulatorError{kBackend,
            "numerical divergence on the MPO path: Tr(rho) = " + std::to_string(tr)};
    }

    Result r;
    result_set_shots(r, nshots);
    result_set_truncation_errors(r, rho.truncation_errors());

    const ClassicalBitIndex total_c = circuit.total_cubits();
    if (!measure_map.empty() && total_c > 0) {
        std::map<std::string, std::uint64_t> counts;
        const bool readout_active = noise.has_readout_errors();

        // Shots are drawn from a FIXED rho, so they are independent and uniform
        // in cost -- static scheduling, and no per-thread state copy (unlike the
        // trajectory path, where each thread evolves its own chain). The seed is
        // derived from the shot INDEX so counts do not depend on how many threads
        // drew them (same convention as the MPS sampler).
        int threads = 1;
#ifdef _OPENMP
        threads = omp_get_max_threads() > 0 ? omp_get_max_threads() : 1;
#endif
        threads = std::max(1, std::min<int>(
            threads, static_cast<int>(std::max<std::uint32_t>(nshots, 1))));
        std::vector<std::map<std::string, std::uint64_t>> local(
            static_cast<std::size_t>(threads));
        // Reduced with min, not overwritten: the fatal/report verdict below reads
        // the WORST violation across all shots.
        std::vector<double> local_negative(static_cast<std::size_t>(threads), 0.0);

        std::atomic<bool> error_flag{false};
        std::mutex        error_mutex;
        std::string       error_msg;
        const std::int64_t shots_i = static_cast<std::int64_t>(nshots);

#ifdef _OPENMP
#pragma omp parallel for schedule(static) num_threads(threads)
#endif
        for (std::int64_t shot = 0; shot < shots_i; ++shot) {
            if (error_flag.load(std::memory_order_relaxed)) continue;
            try {
                std::mt19937_64 rng(
                    detail::thread_seed(seed_, static_cast<std::uint32_t>(shot)));
                std::uniform_real_distribution<double> u01(0.0, 1.0);
                double negative = 0.0;
                // sample() is const and touches no shared mutable state, so
                // concurrent draws from the same rho are safe.
                const std::vector<int> site_bits = rho.sample(rng, &negative);

                std::vector<int> cubit_vals(static_cast<std::size_t>(total_c), 0);
                for (const auto& [q, c] : measure_map) {
                    int reported = site_bits[site_of_qubit_[q]];
                    if (readout_active) {
                        const auto [p01, p10] = noise.readout_error(q);
                        if (p01 > 0.0 || p10 > 0.0) {
                            reported = apply_readout_error(reported, p01, p10, u01(rng));
                        }
                    }
                    cubit_vals[static_cast<std::size_t>(c)] = reported;
                }
#ifdef _OPENMP
                const int tid = omp_get_thread_num();
#else
                const int tid = 0;
#endif
                const std::size_t slot =
                    static_cast<std::size_t>(tid) % local.size();
                ++local[slot][counts_key_from_cubits(cubit_vals)];
                local_negative[slot] = std::min(local_negative[slot], negative);
            } catch (const std::exception& ex) {
                if (!error_flag.exchange(true, std::memory_order_acq_rel)) {
                    std::lock_guard<std::mutex> lk(error_mutex);
                    error_msg = ex.what();
                }
            }
        }

        if (error_flag.load()) throw SimulatorError{kBackend, error_msg};

        double worst_negative = 0.0;
        for (std::size_t t = 0; t < local.size(); ++t) {
            worst_negative = std::min(worst_negative, local_negative[t]);
            for (const auto& kv : local[t]) counts[kv.first] += kv.second;
        }
        // Truncation does not preserve positivity, and the size
        // the violation decides what that means. A mild negative is
        // an approximation with a visible defect and is reported; one past
        // kMpoNegativeProbabilityFatal is not a distribution at all, and counts
        // drawn by clamping it carry no information — so they are not returned.
        if (worst_negative < -kMpoNegativeProbabilityFatal) {
            throw SimulatorError{kBackend,
                "MPO truncation broke positivity beyond use: worst conditional "
                "probability " + std::to_string(worst_negative) +
                " (chi_rho " + std::to_string(last_mpo_bond_) +
                ", accumulated truncation error " +
                std::to_string(rho.total_truncation_error()) +
                "). Raise QPLAYER_MPO_MAX_BOND, or use the Monte Carlo route, which"
                " is positive by construction and is the cheaper path at this noise"
                " strength"};
        }
        if (worst_negative < -kMpoNegativeProbabilityTolerance) {
            log::warn("MPO truncation produced a negative conditional probability (" +
                      std::to_string(worst_negative) +
                      "); rho is no longer positive — lower QPLAYER_MPO_CUTOFF or raise"
                      " QPLAYER_MPO_MAX_BOND");
        }
        result_set_counts(r, std::move(counts));
    }

    // <P> = Tr(rho P): one exact number, not an nshot average. Compiled outside
    // any loop because compile_pauli_ops() throws on a malformed string.
    const std::vector<std::string>& exp_targets = circuit.expectation_targets();
    if (!exp_targets.empty()) {
        std::map<std::string, double> exp_values;
        for (const std::string& pauli : exp_targets) {
            const std::vector<detail::PauliOp> by_qubit =
                detail::compile_pauli_ops(pauli, num_qubits_);
            std::vector<detail::PauliOp> by_site(num_qubits_, detail::PauliOp::I);
            for (QubitIndex q = 0; q < num_qubits_; ++q) by_site[site_of_qubit_[q]] = by_qubit[q];
            exp_values[pauli] = rho.expectation_value(by_site);
        }
        result_set_expectations(r, std::move(exp_values));
    }
    return r;
}

void TensorNetworkBackend::set_seed(std::uint64_t seed) {
    seed_ = seed;
}

void TensorNetworkBackend::set_mps_truncation(std::uint32_t max_bond, double cutoff) {
    detail::MpsTruncationConfig probe;
    probe.max_bond = max_bond;
    probe.cutoff   = cutoff;
    probe.validate();  // reject at the call that set it, not inside a later SVD

    forced_max_bond_ = max_bond;
    forced_cutoff_   = cutoff;
    if (state_ != nullptr) state_ = std::make_unique<detail::MpsState>(num_qubits_, probe);
}

void TensorNetworkBackend::set_mpo_truncation(std::uint32_t max_bond, double cutoff) {
    detail::MpoTruncationConfig probe;
    probe.max_bond = max_bond;
    probe.cutoff   = cutoff;
    probe.validate();  // reject at the call that set it, not inside a later SVD

    forced_mpo_max_bond_ = max_bond;
    forced_mpo_cutoff_   = cutoff;
}

void TensorNetworkBackend::set_noise_interventions(const std::vector<std::uint32_t>& sites,
                                                   bool ledger) {
    noise_sites_  = sites;
    noise_ledger_ = ledger;
}

void TensorNetworkBackend::set_tn_use_mpo(bool use_mpo) {
    use_mpo_ = use_mpo;
}

// Interactive surface — permanent policy rejection (section 8.3 matrix).
// TN updates a state by re-contracting site tensors, which does not fit the
// immediate per-op model Interactive requires.
void TensorNetworkBackend::init_state(QubitIndex /*num_qubits*/) {
    throw SimulatorError{kBackend, kNoInteractive};
}

void TensorNetworkBackend::apply_gate(const GateOp& /*op*/) {
    throw SimulatorError{kBackend, kNoInteractive};
}

int TensorNetworkBackend::apply_measure(QubitIndex /*qubit*/) {
    throw SimulatorError{kBackend, kNoInteractive};
}

void TensorNetworkBackend::apply_reset(QubitIndex /*qubit*/) {
    throw SimulatorError{kBackend, kNoInteractive};
}

// Full contraction to a dense vector, bounded by MAX_DENSE_EXPORT_QUBITS
// O(2^n) — an export path, not the point of MPS.
StateVector TensorNetworkBackend::get_statevector() {
    require_single_state("get_statevector");
    return to_qubit_order(state_->to_dense());
}

// No SoA buffer exists to view — the state is a tensor graph (section 11.7).
RawStateView TensorNetworkBackend::get_statevector_raw() {
    throw SimulatorError{kBackend, "get_statevector_raw is not available on a tensor network"};
}

// <psi|P|psi> by partial contraction (section 11.7). Supported
// every qubit count: the sweep never densifies, so this is the only expectation
// path available above MAX_DENSE_EXPORT_QUBITS. The Pauli string is parsed by
// the shared FullState primitive, so "ZZ" means the same thing on both backends
// (Qiskit big-endian, empty string = all-Z); the resulting per-qubit factors are
// then permuted into site order, exactly as amplitude() permutes its bitstring.
double TensorNetworkBackend::expectation_value(std::string_view pauli) {
    require_single_state("expectation_value");
    const std::vector<detail::PauliOp> by_qubit =
        detail::compile_pauli_ops(pauli, num_qubits_);

    std::vector<detail::PauliOp> by_site(num_qubits_, detail::PauliOp::I);
    for (QubitIndex q = 0; q < num_qubits_; ++q) by_site[site_of_qubit_[q]] = by_qubit[q];
    return detail::mps_expectation_value(state_->graph(), by_site);
}

// nonzero_amplitudes() — TensorNetwork does not support this API.
// TN is batch-only with no incremental state; throws SimulatorError (not stub).
std::vector<std::pair<StateIndex, std::complex<double>>>
TensorNetworkBackend::nonzero_amplitudes() const {
    throw SimulatorError{"TensorNetwork", "nonzero_amplitudes not supported"};
}

// num_qubits() — TN is batch-only (no init_state); always returns 0.
// Reports the prepared width (0 before prepare()). Interactive is unsupported,
// so this is a batch-run property rather than a live session one.
QubitIndex TensorNetworkBackend::num_qubits() const {
    return num_qubits_;
}

SimType          TensorNetworkBackend::sim_type()       const { return SimType::TensorNetwork; }
bool             TensorNetworkBackend::supports_noise() const { return true;  }
bool             TensorNetworkBackend::supports_gpu()   const { return true;  }
std::string_view TensorNetworkBackend::name()           const { return "TensorNetwork"; }

// max_qubits() — TN storage is unbounded; report the dense-equivalent host
// bound as a densification guidance value (inheriting the policy).
std::uint32_t TensorNetworkBackend::max_qubits(detail::WorkingSet /*ws*/) const noexcept {
    // Same answer SparseState gives, for the same reason: neither is bounded by
    // 2^n, and both enforce this constant (tn_cpu.cpp, tn_mpo.cpp). The working
    // set is unused — it parameterises a dense budget that does not apply.
    return kMaxTensorNetworkQubits;
}

}  // namespace qplayer
