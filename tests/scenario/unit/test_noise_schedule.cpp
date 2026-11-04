/// @file test_noise_schedule.cpp
/// @brief Unit and integration tests for (canonical noise trajectory
/// order) and (reset-gate-attached noise channels).
///
/// Suite "NoiseSchedule" — unit tests for the canonical order function.
/// Suite "ResetNoise"    — integration tests for reset channel application.

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "../../../include/qregister.hpp"
#include "../../../include/circuit/circuit.hpp"
#include "../../../include/error.hpp"
#include "../../../include/detail/noise_schedule.hpp"

using qplayer::Circuit;
using qplayer::GateOp;
using qplayer::GateType;
using qplayer::NoiseModel;
using qplayer::ProcType;
using qplayer::QRegister;
using qplayer::QubitIndex;
using qplayer::SimType;

namespace {

std::map<std::string, std::uint64_t> run_noisy(SimType sim, const Circuit& circ,
                                               const NoiseModel& nm,
                                               std::uint32_t shots, std::uint64_t seed) {
    QRegister qr(sim, ProcType::CPU);
    qr.set_seed(seed);
    qr.set_noise_model(nm);
    return qr.run(circ, shots).counts();
}

double tvd(const std::map<std::string, std::uint64_t>& a,
           const std::map<std::string, std::uint64_t>& b, std::uint64_t shots) {
    double d = 0.0;
    for (const auto& kv : a) {
        const auto it = b.find(kv.first);
        const double pb = (it == b.end()) ? 0.0 : static_cast<double>(it->second) / shots;
        d += std::abs(static_cast<double>(kv.second) / shots - pb);
    }
    for (const auto& kv : b) {
        if (a.find(kv.first) == a.end()) d += static_cast<double>(kv.second) / shots;
    }
    return 0.5 * d;
}

// One-qubit device whose only noisy gate is reset; T1/T2 effectively infinite
// so load_device's relaxation floor is ~0 and the synthesized depolarizing
// carries the whole error_rate.
std::string write_reset_device_json(double error_rate, const std::string& suffix) {
    const char* dir = std::getenv("TMPDIR");
    const std::string path = (dir ? std::string(dir) : std::string("/tmp"))
                             + "/qplayer_test_reset_device" + suffix + ".json";
    std::ofstream f(path);
    f << "{\"qubits\": [{\"T1\": 1e15, \"T2\": 1e15}],"
      << " \"gates\": {\"x_0\": {\"duration_ns\": 25.0, \"error_rate\": 0.0},"
      << " \"reset_0\": {\"duration_ns\": 100.0, \"error_rate\": " << error_rate << "}}}";
    return path;
}

double reset_p1_with_device(SimType sim, double error_rate, const std::string& suffix) {
    QRegister qr(sim, ProcType::CPU);
    qr.set_seed(4242);
    qr.load_device_model(write_reset_device_json(error_rate, suffix));
    Circuit circ(1, 1);
    circ.x(0).reset(0).measure(0, 0);
    const auto counts = qr.run(circ, 50000).counts();
    return static_cast<double>(counts.count("1") ? counts.at("1") : 0) / 50000.0;
}

}  // namespace

// ── NoiseSchedule: canonical order unit tests ─────────────────────

TEST(NoiseSchedule, Deterministic_SameCircuitSameOrder) {
    // The function is pure — same circuit always yields the same order.
    // We verify it indirectly: two FullState CPU runs with identical seeds and
    // the same circuit must produce identical counts.
    Circuit circ(3, 3);
    circ.h(0).cx(0, 1).cx(1, 2).measure(0, 0).measure(1, 1).measure(2, 2);
    NoiseModel nm;
    nm.add_depolarizing("h",  0.02);
    nm.add_depolarizing("cx", 0.04);

    const auto r1 = run_noisy(SimType::FullState, circ, nm, 500, 1234);
    const auto r2 = run_noisy(SimType::FullState, circ, nm, 500, 1234);

    // Bit-identical: same seed + same order = same result.
    EXPECT_TRUE(r1 == r2);
}

TEST(NoiseSchedule, BarrierExcluded_DoesNotPreventParity) {
    // A circuit with per-layer barriers must produce statistically consistent
    // results across backends: barriers are excluded from canonical order but
    // are no-ops, so their presence must not break the distribution.
    // Bit-identity is NOT claimed here because TN defers measurements when
    // there is no classical control (midcircuit=false); TVD
    // is the appropriate metric.
    Circuit circ(2, 2);
    circ.h(0);
    circ.barrier({0, 1});
    circ.cx(0, 1);
    circ.barrier({0, 1});
    circ.measure(0, 0).measure(1, 1);
    NoiseModel nm;
    nm.add_depolarizing("h",  0.05);
    nm.add_depolarizing("cx", 0.06);
    const std::uint32_t shots = 8000;

    const auto fs = run_noisy(SimType::FullState,     circ, nm, shots, 7777);
    const auto tn = run_noisy(SimType::TensorNetwork, circ, nm, shots, 7777);

    // TVD ≤ 0.02 (sampling floor at 8000 shots ≈ 0.01 per outcome — TnNoiseParity bound).
    EXPECT_NEAR(tvd(fs, tn, shots), 0.0, 0.02);
}

TEST(NoiseSchedule, ClassicalControl_MidCircuitBoundary_BitIdentical) {
    // Classical control (IfOp) forces midcircuit=true in TN, so measurements
    // collapse in-place with the trajectory RNG — same as FullState CPU.
    // With canonical order, both backends consume the RNG
    // identical order and produce bit-identical counts.
    // Circuit: h(0) -> measure(0,0) -> if(c0==1){x(1)} -> measure(1,1)
    Circuit circ(2, 2);
    circ.h(0).measure(0, 0);
    GateOp x_body;
    x_body.type   = GateType::X;
    x_body.name   = "x";
    x_body.qubits = {1};
    circ.if_then(0, 1, x_body);
    circ.measure(1, 1);
    NoiseModel nm;
    nm.add_depolarizing("h", 0.05);
    nm.add_depolarizing("x", 0.04);
    const std::uint32_t shots = 6000;

    const auto fs = run_noisy(SimType::FullState,     circ, nm, shots, 8888);
    const auto tn = run_noisy(SimType::TensorNetwork, circ, nm, shots, 8888);

    // Bit-identical: same canonical order + same RNG seed.
    EXPECT_TRUE(fs == tn);
}

TEST(NoiseSchedule, NonPauliChannel_ClassicalControl_BitIdentical) {
    // Amplitude-damping (non-Pauli) in a classically-controlled circuit.
    // Classical control forces midcircuit=true → in-place measurement collapse →
    // bit-identical results after canonical order.
    Circuit circ(2, 2);
    circ.h(0).measure(0, 0);
    GateOp x_body;
    x_body.type   = GateType::X;
    x_body.name   = "x";
    x_body.qubits = {1};
    circ.if_then(0, 1, x_body);
    circ.measure(1, 1);
    NoiseModel nm;
    nm.add_amplitude_damping("h",  0.06);
    nm.add_amplitude_damping("x",  0.05);
    const std::uint32_t shots = 6000;

    const auto fs = run_noisy(SimType::FullState,     circ, nm, shots, 3141);
    const auto tn = run_noisy(SimType::TensorNetwork, circ, nm, shots, 3141);

    // Bit-identical.
    EXPECT_TRUE(fs == tn);
}

TEST(NoiseSchedule, ResetBoundary_BitIdentical) {
    // Reset is a boundary in the canonical order. The two
    // backends must still agree bit-for-bit on the reset+measure counts.
    Circuit circ(1, 1);
    circ.x(0).reset(0).measure(0, 0);
    NoiseModel nm;
    nm.add_depolarizing("x", 0.05);
    const std::uint32_t shots = 4000;

    const auto fs = run_noisy(SimType::FullState,     circ, nm, shots, 2718);
    const auto tn = run_noisy(SimType::TensorNetwork, circ, nm, shots, 2718);

    EXPECT_TRUE(fs == tn);
}

// ── ResetNoise: reset-gate-attached channel tests ─────────────────

TEST(ResetNoise, FullStateCpu_XResetMeasure_ProbNearErrorRate) {
    // x; reset; measure with reset error_rate r:
    // After reset the qubit is in |0> + depol channel.
    // Depolarizing on |0>: P(|1>) = 2r/3 (X and Y components flip).
    // Measurement readout is exact (no readout noise).
    // For r = 0.30: P(1) = 0.20. nshots=50000 for statistical precision.
    const double r = 0.30;
    Circuit circ(1, 1);
    circ.x(0).reset(0).measure(0, 0);
    NoiseModel nm;
    nm.add_depolarizing("reset", r);

    const auto counts = run_noisy(SimType::FullState, circ, nm, 50000, 99);
    const double p1 = static_cast<double>(counts.count("1") ? counts.at("1") : 0) / 50000.0;
    // Expected P(1) = 2r/3 for depolarizing on |0>.
    // tolerance 0.02 for 50k shots (3-sigma ≈ 0.006, conservative).
    EXPECT_NEAR(p1, 2.0 * r / 3.0, 0.02);
}

TEST(ResetNoise, FullStateCpu_ResetMeasure_ProbNearErrorRate) {
    // reset; measure (starting from |0>): same depolarizing channel logic.
    // P(1) = 2r/3.
    const double r = 0.20;
    Circuit circ(1, 1);
    circ.reset(0).measure(0, 0);
    NoiseModel nm;
    nm.add_depolarizing("reset", r);

    const auto counts = run_noisy(SimType::FullState, circ, nm, 50000, 17);
    const double p1 = static_cast<double>(counts.count("1") ? counts.at("1") : 0) / 50000.0;
    EXPECT_NEAR(p1, 2.0 * r / 3.0, 0.02);
}

TEST(ResetNoise, TN_XResetMeasure_ProbNearErrorRate) {
    // Same as FullStateCpu_XResetMeasure_ProbNearErrorRate but on TN backend.
    const double r = 0.30;
    Circuit circ(1, 1);
    circ.x(0).reset(0).measure(0, 0);
    NoiseModel nm;
    nm.add_depolarizing("reset", r);

    const auto counts = run_noisy(SimType::TensorNetwork, circ, nm, 50000, 99);
    const double p1 = static_cast<double>(counts.count("1") ? counts.at("1") : 0) / 50000.0;
    EXPECT_NEAR(p1, 2.0 * r / 3.0, 0.02);
}

TEST(ResetNoise, FullStateCpu_TN_BitIdentical_WithResetChannel) {
    // With a reset channel both backends must still be bit-identical.
    Circuit circ(1, 1);
    circ.x(0).reset(0).measure(0, 0);
    NoiseModel nm;
    nm.add_depolarizing("reset", 0.25);
    const std::uint32_t shots = 6000;

    const auto fs = run_noisy(SimType::FullState,     circ, nm, shots, 555);
    const auto tn = run_noisy(SimType::TensorNetwork, circ, nm, shots, 555);

    EXPECT_TRUE(fs == tn);
}

TEST(ResetNoise, FullStateCpu_IfOpBodyReset_ProbNearErrorRate) {
    // h; measure; if(creg==1) reset(0); measure again.
    // After if-body reset the qubit is |0> + depol channel.
    // Conditioned on branch executing, P(1 in second measure) = 2r/3.
    // We check the unconditional P(second bit=1) < r+0.5*r (loose upper bound),
    // and > 0 for large r, confirming the channel fires.
    const double r = 0.30;
    // Circuit: q[0]: h -> meas(c[0]) -> if(c[0]==1){reset} -> meas(c[1])
    Circuit circ(1, 2);
    circ.h(0).measure(0, 0);
    GateOp reset_body;
    reset_body.type   = GateType::Reset;
    reset_body.name   = "reset";
    reset_body.qubits = {0};
    circ.if_then(0, 1, reset_body);
    circ.measure(0, 1);

    NoiseModel nm;
    nm.add_depolarizing("reset", r);

    const auto counts = run_noisy(SimType::FullState, circ, nm, 60000, 9191);

    // Count shots where c[1]=1 (second bit in string is c[1] for 2-bit output).
    // The string is c[0]c[1] or c[1]c[0] depending on order. Use TVD instead:
    // when c[0]=0 the IfOp branch doesn't fire, so q[0] stays |+> collapsed to
    // 0 by the earlier measure; measure2 yields 0 deterministically (noiseless path).
    // When c[0]=1 the branch fires: reset |1>->|0>, then depol, then measure.
    // P(c[1]=1 | c[0]=1) = 2r/3.
    // Overall: P(c[1]=1) = P(c[0]=1)*P(c[1]=1|c[0]=1) = 0.5 * 2r/3 = r/3.
    std::uint64_t c1_set = 0; // c[1] = 1
    for (const auto& kv : counts) {
        // The 2-bit string is indexed MSB: position 0 is c[1], position 1 is c[0].
        // In the run() result the format is "c[1]c[0]" (register 1 first = MSB).
        // Actually the format is creg index 0 = rightmost bit: "c1c0" where c0 is rightmost.
        // Check: kv.first[1] is c[0], kv.first[0] is c[1].
        if (kv.first.size() >= 1 && kv.first[0] == '1') c1_set += kv.second;
    }
    const double p_c1 = static_cast<double>(c1_set) / 60000.0;
    // Expected: r/3 = 0.10. Tolerance 0.02.
    EXPECT_NEAR(p_c1, r / 3.0, 0.02);
}

TEST(ResetNoise, DirectNoiseModel_AddBitFlip_ResetChannel) {
    // Directly add a bit-flip (Kraus) channel for "reset" to confirm the
    // low-level NoiseModel path works end-to-end.
    // A bit-flip on reset of |0> with rate r gives P(1) = r.
    const double r = 0.25;
    Circuit circ(1, 1);
    circ.reset(0).measure(0, 0);

    NoiseModel nm;
    // Use add_bit_flip which models a Pauli-X error with probability r.
    nm.add_bit_flip("reset", r);

    const auto counts = run_noisy(SimType::FullState, circ, nm, 50000, 42);
    const double p1 = static_cast<double>(counts.count("1") ? counts.at("1") : 0) / 50000.0;
    // Bit-flip channel on |0>: P(1) = r.
    EXPECT_NEAR(p1, r, 0.02);
}

TEST(ResetNoise, NoChannel_ResetBehaviorUnchanged) {
    // Without a reset channel the behavior is the same as before :
    // reset always gives |0>, so measure must always yield 0.
    Circuit circ(1, 1);
    circ.x(0).reset(0).measure(0, 0);
    NoiseModel nm;
    // No reset channel — only gate noise on x.
    nm.add_depolarizing("x", 0.05);

    const auto counts = run_noisy(SimType::FullState, circ, nm, 4000, 11);
    // x noise on x gate may cause |0> sometimes, then reset gives |0> always,
    // so measure is always 0.
    const double p1 = static_cast<double>(counts.count("1") ? counts.at("1") : 0) / 4000.0;
    EXPECT_NEAR(p1, 0.0, 1e-9);  // deterministically 0 without reset channel
}

// Device error_rate is an average gate error r; load_device turns it into a
// depolarizing of total Pauli probability 3r/2, whose X and Y parts flip |0>
// with probability exactly r — so P(1) after reset equals error_rate.
TEST(ResetNoise, DeviceErrorRate_FullStateCpu_ResidualOneEqualsErrorRate) {
    const double p1 = reset_p1_with_device(SimType::FullState, 0.10, "_fs");

    // 50k shots, binomial sigma = sqrt(0.1*0.9/50000) = 0.0013; 0.006 is ~4.5 sigma.
    EXPECT_NEAR(p1, 0.10, 0.006);
}

TEST(ResetNoise, DeviceErrorRate_TN_ResidualOneEqualsErrorRate) {
    const double p1 = reset_p1_with_device(SimType::TensorNetwork, 0.10, "_tn");

    // Same binomial tolerance as the FullState case.
    EXPECT_NEAR(p1, 0.10, 0.006);
}

// ── NoiseSchedule: the order function itself ───────────────────

namespace {

// (gate name, qubits) per scheduled op, so circuits whose op indices differ
// (e.g. with and without barriers) can be compared by what actually runs.
std::vector<std::string> scheduled_signature(const Circuit& c) {
    std::vector<std::string> sig;
    for (std::size_t i : qplayer::detail::noise_trajectory_canonical_order(c)) {
        const auto& op = c.ops()[i];
        std::string s;
        if (const auto* g = std::get_if<GateOp>(&op)) {
            s = g->name;
            for (QubitIndex q : g->qubits) s += "," + std::to_string(q);
        } else if (const auto* m = std::get_if<qplayer::MeasureOp>(&op)) {
            s = "measure," + std::to_string(m->qubit);
        } else {
            s = "other";
        }
        sig.push_back(s);
    }
    return sig;
}

}  // namespace

TEST(NoiseSchedule, BarriersDoNotSplitSegments) {
    // A barrier per layer must not change the schedule — otherwise layered
    // circuits (the Willow cycle) lose the reordering and the TN chi doubles.
    Circuit a(6, 0), b(6, 0);
    a.cx(0, 1).barrier({0, 1, 2, 3, 4, 5}).cx(4, 5).barrier({0, 1, 2, 3, 4, 5}).cx(2, 3).h(0);
    b.cx(0, 1).cx(4, 5).cx(2, 3).h(0);

    EXPECT_TRUE(scheduled_signature(a) == scheduled_signature(b));
}

TEST(NoiseSchedule, ReordersByCentreDistance_OneQubitFirst) {
    // The ready one-qubit h(6) runs first although it is last in the circuit;
    // after cx(0,1) the centre sits at 1, so cx(2,3) (distance 2) runs before
    // cx(4,5) (distance 4) despite coming later.
    Circuit c(7, 0);
    c.cx(0, 1).cx(4, 5).cx(2, 3).h(6);

    const std::vector<std::string> want = {"h,6", "cx,0,1", "cx,2,3", "cx,4,5"};
    EXPECT_TRUE(scheduled_signature(c) == want);
}

TEST(NoiseSchedule, MeasureIsBoundary_PerQubitOrderKept) {
    // Nothing crosses the measure, and per-qubit order (h then cx on qubit 0) holds.
    Circuit c(4, 1);
    c.h(0).cx(2, 3).cx(0, 1).measure(1, 0).cx(2, 3).h(0);

    const std::vector<std::string> got = scheduled_signature(c);
    const std::vector<std::string> want = {"h,0", "cx,0,1", "cx,2,3", "measure,1", "h,0", "cx,2,3"};
    EXPECT_TRUE(got == want);
}
