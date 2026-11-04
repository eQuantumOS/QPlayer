// noise_model.cpp — NoiseModel channel registration, query, and Kraus operator
// construction (..06). Every add_* method validates its
// parameters immediately (out-of-range -> CircuitError) and lowers the channel
// to a list of Kraus operators so backends dispatch uniformly via
// channels_for_gate(). load_device() (T1/T2 ->
// Kraus) lands with the T1T2Calculator in Phase 8.

#include "noise/noise_model.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <utility>

#include "error.hpp"
#include "log.hpp"
#include "noise/detail/kraus_builders.hpp"
#include "noise/device_model.hpp"
#include "noise/t1t2_calculator.hpp"

namespace qplayer {

namespace {

using Complex = std::complex<double>;

// Reject probabilities / damping rates outside [0, 1] at registration time
// (input-validation policy) so backends never see a malformed channel.
void check_unit_interval(double v, const char* param, const char* method) {
    if (!(v >= 0.0 && v <= 1.0)) {
        throw CircuitError{std::string{method} + ": " + param +
                           " must be in [0, 1], got " + std::to_string(v)};
    }
}

// Validate caller-supplied operators are square, equal-dimensioned, and
// trace-preserving (sum_i K_i^† K_i ≈ I within 1e-10) — the same condition the
// Circuit::define_opaque(KrausChannel) path enforces (circuit.cpp verify_kraus).
constexpr double kTraceTol = 1e-10;

void verify_trace_preserving(const KrausChannel& ch, const char* method) {
    if (ch.operators.empty()) {
        throw CircuitError{std::string{method} + ": KrausChannel has no operators"};
    }
    const std::size_t dim = ch.operators.front().size();
    if (dim == 0) {
        throw CircuitError{std::string{method} + ": Kraus operator has zero rows"};
    }
    for (const auto& K : ch.operators) {
        if (K.size() != dim) {
            throw CircuitError{std::string{method} + ": Kraus operators have inconsistent row counts"};
        }
        for (const auto& row : K) {
            if (row.size() != dim) {
                throw CircuitError{std::string{method} + ": Kraus operator is not square"};
            }
        }
    }
    // (sum K†K)[i][j] = sum_op sum_k conj(K_op[k][i]) * K_op[k][j]; expect delta_ij.
    for (std::size_t i = 0; i < dim; ++i) {
        for (std::size_t j = 0; j < dim; ++j) {
            Complex acc{0.0, 0.0};
            for (const auto& K : ch.operators) {
                for (std::size_t k = 0; k < dim; ++k) {
                    acc += std::conj(K[k][i]) * K[k][j];
                }
            }
            const Complex target = (i == j) ? Complex{1.0, 0.0} : Complex{0.0, 0.0};
            if (std::abs(acc - target) > kTraceTol) {
                throw CircuitError{std::string{method} +
                                   ": Kraus channel is not trace-preserving "
                                   "(|sum K†K - I| > 1e-10)"};
            }
        }
    }
}

}  // namespace

NoiseModel::NoiseModel() = default;

NoiseModel& NoiseModel::add_depolarizing(const std::string&      gate_name,
                                         double                  p,
                                         std::vector<QubitIndex> qubits) {
    check_unit_interval(p, "p", "add_depolarizing");
    KrausChannel ch;
    ch.operators = detail::depolarizing_ops(p);
    ch.type      = ChannelType::Depolarizing;
    gate_channels_[gate_name].push_back(GateEntry{std::move(ch), std::move(qubits)});
    return *this;
}

NoiseModel& NoiseModel::add_amplitude_damping(const std::string&      gate_name,
                                              double                  gamma,
                                              std::vector<QubitIndex> qubits) {
    check_unit_interval(gamma, "gamma", "add_amplitude_damping");
    KrausChannel ch;
    ch.operators = detail::amplitude_damping_ops(gamma);
    ch.type      = ChannelType::AmplitudeDamping;
    gate_channels_[gate_name].push_back(GateEntry{std::move(ch), std::move(qubits)});
    return *this;
}

NoiseModel& NoiseModel::add_phase_damping(const std::string&      gate_name,
                                          double                  lam,
                                          std::vector<QubitIndex> qubits) {
    check_unit_interval(lam, "lambda", "add_phase_damping");
    KrausChannel ch;
    ch.operators = detail::phase_damping_ops(lam);
    ch.type      = ChannelType::PhaseDamping;
    gate_channels_[gate_name].push_back(GateEntry{std::move(ch), std::move(qubits)});
    return *this;
}

NoiseModel& NoiseModel::add_bit_flip(const std::string&      gate_name,
                                     double                  p,
                                     std::vector<QubitIndex> qubits) {
    check_unit_interval(p, "p", "add_bit_flip");
    KrausChannel ch;
    ch.operators = detail::bit_flip_ops(p);
    ch.type      = ChannelType::BitFlip;
    gate_channels_[gate_name].push_back(GateEntry{std::move(ch), std::move(qubits)});
    return *this;
}

NoiseModel& NoiseModel::add_phase_flip(const std::string&      gate_name,
                                       double                  p,
                                       std::vector<QubitIndex> qubits) {
    check_unit_interval(p, "p", "add_phase_flip");
    KrausChannel ch;
    ch.operators = detail::phase_flip_ops(p);
    ch.type      = ChannelType::PhaseFlip;
    gate_channels_[gate_name].push_back(GateEntry{std::move(ch), std::move(qubits)});
    return *this;
}

NoiseModel& NoiseModel::add_readout_error(QubitIndex qubit,
                                          double     p_meas_0_given_1,
                                          double     p_meas_1_given_0) {
    check_unit_interval(p_meas_0_given_1, "p_meas_0_given_1", "add_readout_error");
    check_unit_interval(p_meas_1_given_0, "p_meas_1_given_0", "add_readout_error");
    readout_errors_[qubit] = ReadoutError{p_meas_0_given_1, p_meas_1_given_0};
    return *this;
}

// Readout error query. Backends call has_readout_errors()
// once per execute() and readout_error(q) per MeasureOp to flip the reported bit.
bool NoiseModel::has_readout_errors() const {
    return !readout_errors_.empty();
}

std::pair<double, double> NoiseModel::readout_error(QubitIndex q) const {
    auto it = readout_errors_.find(q);
    if (it == readout_errors_.end()) return {0.0, 0.0};
    return {it->second.p_meas_0_given_1, it->second.p_meas_1_given_0};
}

NoiseModel& NoiseModel::add_kraus(const std::string&      gate_name,
                                  const KrausChannel&     channel,
                                  std::vector<QubitIndex> qubits) {
    verify_trace_preserving(channel, "add_kraus");
    // Preserve the caller-supplied type tag (defaults to Custom). target_qubit
    // is overwritten per operand in channels_for_gate().
    gate_channels_[gate_name].push_back(GateEntry{channel, std::move(qubits)});
    return *this;
}

namespace {

// Average gate infidelity of the single-qubit T1/T2 relaxation channel over
// t_gate (amplitude damping gamma composed with phase damping lambda), used to
// subtract the relaxation contribution already folded into the device gate_error
// so depolarizing only covers the excess (avoid double counting).
// F_avg = (sum_i |Tr K_i|^2 + d) / (d(d+1)), d=2 (Horodecki/Nielsen average gate
// fidelity). Composed Kraus traces reduce to (1 + sa*sp), 0, sqrt(lambda)*sa, 0
// with sa=sqrt(1-gamma), sp=sqrt(1-lambda). Matches Qiskit thermal_relaxation
// average_gate_error to machine precision (verified).
// sum_i |Tr K_i|^2 for the single-qubit relaxation channel (amp damping gamma
// composed with phase damping lambda). Traces: (1+sa*sp), 0, sqrt(lambda)*sa, 0.
double relaxation_sum_tr2_1q(double t1, double t2, double t_gate) {
    const double gamma  = T1T2Calculator::amplitude_damping_gamma(t1, t_gate);
    const double lambda = T1T2Calculator::dephasing_lambda(t1, t2, t_gate);
    const double sa = std::sqrt(std::max(0.0, 1.0 - gamma));
    const double sp = std::sqrt(std::max(0.0, 1.0 - lambda));
    return (1.0 + sa * sp) * (1.0 + sa * sp) + lambda * sa * sa;
}

double relaxation_avg_gate_error_1q(double t1, double t2, double t_gate) {
    const double f_avg = (relaxation_sum_tr2_1q(t1, t2, t_gate) + 2.0) / 6.0;  // d=2
    return 1.0 - f_avg;
}

// Average gate error of the two-qubit relaxation channel relax(a) (x) relax(b).
// Process fidelity is multiplicative: F_proc = (sum_tr2_a/4)(sum_tr2_b/4), and
// F_avg = (d*F_proc + 1)/(d+1) with d=4 (verified vs direct Kraus computation).
double relaxation_avg_gate_error_2q(double t1a, double t2a,
                                    double t1b, double t2b, double t_gate) {
    const double fa = relaxation_sum_tr2_1q(t1a, t2a, t_gate) / 4.0;
    const double fb = relaxation_sum_tr2_1q(t1b, t2b, t_gate) / 4.0;
    const double f_proc = fa * fb;
    const double f_avg  = (4.0 * f_proc + 1.0) / 5.0;   // d=4
    return 1.0 - f_avg;
}

// Invert an average gate error r to the depolarizing_ops(p) parameter. That
// builder is the Pauli-p convention {sqrt(1-p)I, sqrt(p/3)X,Y,Z}, whose average
// gate error is r = p*d/(d+1) (verified), so p = r*(d+1)/d. NOTE: this differs
// from /'s stated r=p(d-1)/d (the lambda-convention (1-p)ρ+pI/d),
// which does not match depolarizing_ops — the doc formula is being corrected.
double depol_param_from_avg_error(double r, double d) {
    return r * (d + 1.0) / d;
}

// Split a DeviceModel gate key into (name, qubits). "cx_0_1" -> ("cx", {0,1});
// "h" -> ("h", {}). Trailing all-digit underscore-separated tokens are taken as
// operand qubits (schema); the remaining prefix is the gate name.
std::pair<std::string, std::vector<QubitIndex>> split_gate_key(const std::string& key) {
    std::vector<std::string> tokens;
    std::string cur;
    for (char c : key) {
        if (c == '_') { tokens.push_back(cur); cur.clear(); }
        else          { cur += c; }
    }
    tokens.push_back(cur);

    auto is_uint = [](const std::string& s) {
        if (s.empty()) return false;
        return std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
    };

    std::size_t split = tokens.size();  // index of first trailing-numeric token
    while (split > 1 && is_uint(tokens[split - 1])) --split;

    std::string name = tokens[0];
    for (std::size_t i = 1; i < split; ++i) { name += '_'; name += tokens[i]; }

    std::vector<QubitIndex> qubits;
    for (std::size_t i = split; i < tokens.size(); ++i) {
        qubits.push_back(static_cast<QubitIndex>(std::stoul(tokens[i])));
    }
    return {name, qubits};
}

}  // namespace

NoiseModel& NoiseModel::load_device(const DeviceModel& device) {
    // Convert device T1/T2 + per-gate duration into Amplitude Damping +
    // Dephasing Kraus channels. Each described gate gets a
    // relaxation channel on every operand whose qubit has finite coherence;
    // backends then query channels_for_gate() uniformly (no separate device
    // path). Idle-period relaxation is handled separately by IdleNoiseInserter
    // NoiseModel is a friend of DeviceModel so it can enumerate the
    // gate table without widening the public DeviceModel surface.

    // Idempotent: reset any previously synthesised device state so re-loading
    // the same device does not double its channels (push_back accumulation) and
    // loading a second device does not leave the first's channels behind
    // (#2 fix). Callers that want device + manual channels must register
    // the manual channels AFTER load_device.
    gate_channels_.clear();
    readout_errors_.clear();
    t1_.clear();
    t2_.clear();
    has_t1_t2_ = false;

    // Store per-qubit T1/T2 so idle_channels() can serve IdleOp relaxation
    // directly. DeviceModel already stores infinity for
    // absent entries so the vector can be copied verbatim.
    if (device.num_qubits() > 0) {
        t1_ = device.t1_;
        t2_ = device.t2_;
    }

    for (const auto& kv : device.gate_props_) {
        auto [gate_name, key_qubits] = split_gate_key(kv.first);
        const double duration   = kv.second.duration_ns;
        const double gate_error = kv.second.error_rate;

        // No qubit suffix -> the gate property applies to every device qubit.
        std::vector<QubitIndex> targets = key_qubits;
        if (targets.empty()) {
            for (QubitIndex q = 0; q < device.num_qubits(); ++q) targets.push_back(q);
        }

        // ── relaxation: Amplitude Damping + Dephasing per operand ──
        // A 2Q gate key (e.g. cx_0_1) binds its per-operand relaxation to the
        // ordered edge so a qubit shared across edges is damped once per gate,
        // not once per incident edge, and the reversed direction / a different
        // edge does not pull it (#1 fix). 1Q gate + bare keys keep the
        // per-operand filter (a 1Q gate has a single operand, so no duplication).
        const bool is_edge = (key_qubits.size() == 2);
        for (QubitIndex q : targets) {
            const double t1 = device.T1(q);
            const double t2 = device.T2(q);
            // Infinite coherence -> identity channel; skip to avoid registering
            // no-op operators and to keep has_t1_t2_ meaningful.
            if (!std::isfinite(t1) && !std::isfinite(t2)) continue;

            std::vector<KrausChannel> relax =
                T1T2Calculator::compute(t1, t2, duration, q);
            for (KrausChannel& ch : relax) {
                // compute() pre-sets ch.target_qubit = q, so an edge entry emits
                // the correct operand at query time.
                if (is_edge) {
                    gate_channels_[gate_name].push_back(
                        GateEntry{std::move(ch), key_qubits, /*edge_signature=*/true});
                } else {
                    gate_channels_[gate_name].push_back(
                        GateEntry{std::move(ch), {q}, /*edge_signature=*/false});
                }
            }
            has_t1_t2_ = true;
        }

        // ── depolarizing: excess of gate_error over relaxation ──────
        // Single-qubit native gates only for now: the 2-qubit case needs a 4x4
        // Kraus apply path so its excess depolarizing is deferred
        // there. Bare-name keys carry no arity, so we cannot tell 1Q from 2Q and
        // skip depolarizing for them (real device JSON uses qubit-specific keys).
        if (key_qubits.size() == 1) {
            const QubitIndex q = key_qubits[0];
            const double r_relax  = relaxation_avg_gate_error_1q(
                device.T1(q), device.T2(q), duration);
            double r_excess = gate_error - r_relax;
            if (r_excess < 0.0) {
                // gate_error already explained by relaxation alone: clamp to
                // avoid a negative (unphysical) depolarizing parameter.
                log::warn("load_device: gate '" + gate_name + "' on qubit " +
                          std::to_string(q) + " has error_rate below its "
                          "relaxation floor; excess depolarizing clamped to 0");
                r_excess = 0.0;
            }
            if (r_excess > 0.0) {
                double p = depol_param_from_avg_error(r_excess, /*d=*/2.0);
                if (p > 1.0) p = 1.0;   // depolarizing_ops requires p in [0,1]
                add_depolarizing(gate_name, p, {q});
            }
        }
        // ── 2-qubit excess depolarizing (4x4 Kraus) ────────
        else if (key_qubits.size() == 2) {
            const QubitIndex q0 = key_qubits[0];
            const QubitIndex q1 = key_qubits[1];
            const double r_relax = relaxation_avg_gate_error_2q(
                device.T1(q0), device.T2(q0), device.T1(q1), device.T2(q1), duration);
            double r_excess = gate_error - r_relax;
            if (r_excess < 0.0) {
                log::warn("load_device: 2Q gate '" + gate_name + "' on qubits " +
                          std::to_string(q0) + "," + std::to_string(q1) +
                          " has error_rate below its relaxation floor; excess "
                          "depolarizing clamped to 0");
                r_excess = 0.0;
            }
            if (r_excess > 0.0) {
                double p = depol_param_from_avg_error(r_excess, /*d=*/4.0);
                if (p > 1.0) p = 1.0;
                KrausChannel ch;
                ch.operators = detail::depolarizing_2q_ops(p);
                ch.type      = ChannelType::Depolarizing;
                gate_channels_[gate_name].push_back(GateEntry{std::move(ch), {q0, q1}});
            }
        }
        // ── bare gate key (no operand suffix): arity unknown ────────────────
        else if (key_qubits.empty() && gate_error > 0.0) {
            // Relaxation is applied above, but excess depolarizing cannot be
            // synthesised without knowing the gate arity. Warn so a compact
            // device JSON does not silently drop its gate_error (#5).
            // Real device JSON keys are qubit-specific (e.g. "sx_0").
            log::warn("load_device: gate '" + gate_name + "' uses a bare key "
                      "(no qubit suffix); its error_rate is not synthesised into "
                      "depolarizing (arity unknown). Use qubit-specific keys "
                      "(e.g. '" + gate_name + "_0') to include gate error.");
        }
    }

    // ── readout: device confusion probabilities -> add_readout_error ─
    for (QubitIndex q = 0; q < device.num_qubits(); ++q) {
        if (device.has_readout_error(q)) {
            const auto ro = device.readout_error(q);   // {p_meas0_prep1, p_meas1_prep0}
            add_readout_error(q, ro.first, ro.second);
        }
    }
    return *this;
}

NoiseModel NoiseModel::project_to_dense(
    const std::unordered_map<QubitIndex, QubitIndex>& dense_to_physical) const {
    // Re-index a physical-keyed model into the dense [0,k) space of a truncated
    // circuit (handoff). Dense indices are contiguous [0,k) by
    // QubitTruncationPass construction, so k == map size..
    NoiseModel out;
    const std::size_t k = dense_to_physical.size();

    // Inverse map physical -> dense, used to relabel qubit-specific filters.
    std::unordered_map<QubitIndex, QubitIndex> physical_to_dense;
    physical_to_dense.reserve(k);
    for (const auto& [dense, phys] : dense_to_physical) {
        physical_to_dense.emplace(phys, dense);
    }

    // ── T1/T2: dense vector of length k, each pulled from its physical qubit ──
    // Absent physical entries stay at infinity (DeviceModel's no-coherence
    // sentinel) so idle_channels() treats them as noiseless.
    out.t1_.assign(k, std::numeric_limits<double>::infinity());
    out.t2_.assign(k, std::numeric_limits<double>::infinity());
    for (const auto& [dense, phys] : dense_to_physical) {
        if (static_cast<std::size_t>(phys) < t1_.size()) {
            out.t1_[static_cast<std::size_t>(dense)] = t1_[static_cast<std::size_t>(phys)];
        }
        if (static_cast<std::size_t>(phys) < t2_.size()) {
            out.t2_[static_cast<std::size_t>(dense)] = t2_[static_cast<std::size_t>(phys)];
        }
    }
    out.has_t1_t2_ = has_t1_t2_;

    // ── readout errors: re-key physical -> dense ─────────────────────────────
    for (const auto& [dense, phys] : dense_to_physical) {
        auto it = readout_errors_.find(phys);
        if (it != readout_errors_.end()) {
            out.readout_errors_[dense] = it->second;
        }
    }

    // ── gate channels: relabel qubit-specific filters, keep global ones ──────
    for (const auto& [gate_name, entries] : gate_channels_) {
        for (const GateEntry& entry : entries) {
            if (entry.qubits.empty()) {
                // Global rule: applies to every operand regardless of index.
                out.gate_channels_[gate_name].push_back(entry);
                continue;
            }
            // Qubit-specific: keep only if every physical operand survives the
            // truncation, then relabel each to its dense index.
            std::vector<QubitIndex> dense_qubits;
            dense_qubits.reserve(entry.qubits.size());
            bool all_present = true;
            for (QubitIndex phys : entry.qubits) {
                auto it = physical_to_dense.find(phys);
                if (it == physical_to_dense.end()) { all_present = false; break; }
                dense_qubits.push_back(it->second);
            }
            if (!all_present) continue;
            GateEntry dense_entry{entry.channel, std::move(dense_qubits),
                                  entry.edge_signature};
            // Edge-bound relaxation carries a pre-set physical target_qubit;
            // remap it into dense space too. It is one of the edge operands,
            // all of which survived the all_present check above.
            if (entry.edge_signature) {
                dense_entry.channel.target_qubit =
                    physical_to_dense.at(entry.channel.target_qubit);
            }
            out.gate_channels_[gate_name].push_back(std::move(dense_entry));
        }
    }

    return out;
}

bool NoiseModel::has_channels_for_gate(const std::string& gate_name) const {
    auto it = gate_channels_.find(gate_name);
    return it != gate_channels_.end() && !it->second.empty();
}

std::vector<KrausChannel>
NoiseModel::channels_for_gate(const std::string&      gate_name,
                              std::vector<QubitIndex> qubits) const {
    std::vector<KrausChannel> out;
    auto it = gate_channels_.find(gate_name);
    if (it == gate_channels_.end()) {
        return out;
    }
    // Ordered exact match of a gate instance's operands against a stored edge
    // signature (a directional device edge, e.g. cx_0_1). Direction matters:
    // cx(0,1) must not pick up the reversed calibration cx_1_0, nor a different
    // edge that merely shares a qubit (#1 fix).
    auto operands_match_edge = [&](const std::vector<QubitIndex>& edge) {
        return qubits.size() == edge.size() &&
               std::equal(qubits.begin(), qubits.end(), edge.begin());
    };

    for (const auto& entry : it->second) {
        const bool is_2q = !entry.channel.operators.empty() &&
                           entry.channel.operators.front().size() == 4;

        // ── Two-qubit (4x4) channel ───────────────────────
        // target_qubit = local bit 0, target_qubit2 = local bit 1.
        if (is_2q) {
            if (!entry.qubits.empty()) {
                // Directional edge: emit only when operands match in order
                // ( — was an unordered both-present test that fired the
                // reversed edge and any edge sharing a qubit).
                if (operands_match_edge(entry.qubits)) {
                    KrausChannel ch = entry.channel;
                    ch.target_qubit  = entry.qubits[0];
                    ch.target_qubit2 = entry.qubits[1];
                    out.push_back(std::move(ch));
                }
            } else if (qubits.size() >= 2) {
                // Global 2Q rule: bind to the first two operands in order.
                KrausChannel ch = entry.channel;
                ch.target_qubit  = qubits[0];
                ch.target_qubit2 = qubits[1];
                out.push_back(std::move(ch));
            }
            continue;
        }

        // ── Edge-bound single-qubit relaxation ──────────────────────
        // A 2Q gate's per-operand relaxation is stored against its ordered edge
        // so a qubit shared by several edges is damped once per gate (with that
        // edge's duration), not once per incident edge. channel.target_qubit is
        // pre-set to the operand, so emit as-is on an ordered edge match.
        if (entry.edge_signature) {
            if (operands_match_edge(entry.qubits)) {
                out.push_back(entry.channel);
            }
            continue;
        }

        // ── Plain single-qubit channel (user API + 1Q-gate relaxation) ──────
        if (entry.qubits.empty()) {
            // Global rule: apply to every operand of this gate instance, in
            // operand order, with target_qubit bound to each operand.
            for (QubitIndex q : qubits) {
                KrausChannel ch = entry.channel;
                ch.target_qubit = q;
                out.push_back(std::move(ch));
            }
        } else {
            // Qubit-specific: only the operands listed in the filter, still in
            // operand order so backend dispatch is deterministic.
            for (QubitIndex q : qubits) {
                if (std::find(entry.qubits.begin(), entry.qubits.end(), q) !=
                    entry.qubits.end()) {
                    KrausChannel ch = entry.channel;
                    ch.target_qubit = q;
                    out.push_back(std::move(ch));
                }
            }
        }
    }
    return out;
}

bool NoiseModel::has_t1_t2() const {
    return has_t1_t2_;
}

std::vector<KrausChannel>
NoiseModel::idle_channels(QubitIndex q, double duration_ns) const {
    // Guard: qubit index out of range means no coherence data available.
    // Return empty (no noise) so backends do not have to special-case the
    // missing-device case (idle_channels contract).
    if (static_cast<std::size_t>(q) >= t1_.size()) {
        return {};
    }
    const double t1 = t1_[static_cast<std::size_t>(q)];
    const double t2 = t2_[static_cast<std::size_t>(q)];
    // T1T2Calculator::compute returns {} for infinite coherence (no damping).
    return T1T2Calculator::compute(t1, t2, duration_ns, q);
}

}  // namespace qplayer
