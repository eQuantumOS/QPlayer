/// @file test_noise_intervention.cpp
/// @brief Per-site noise interventions and the draw ledger.
///
/// Suite "NoiseIntervention". Each test pins one invariant or one
/// error. The parity tests use a classically controlled circuit because that is
/// where FullState CPU and TensorNetwork are bit-aligned to begin with (T29).

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"

#include <cstdint>
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
using qplayer::NoiseDrawEvent;
using qplayer::NoiseModel;
using qplayer::ProcType;
using qplayer::QRegister;
using qplayer::Result;
using qplayer::SimType;
using qplayer::SimulatorError;

namespace {

// Mid-circuit measurement + IfOp + reset with gate, 2Q and readout noise: every
// site kind except idle, and a gate order the canonical scheduler rewrites
// (the 1Q h on q2 is pulled ahead of the cx).
Circuit test_circuit() {
    Circuit c(3, 3);
    c.h(0).cx(0, 1).h(2).cz(1, 2).measure(0, 0);
    GateOp x_body;
    x_body.type   = GateType::X;
    x_body.name   = "x";
    x_body.qubits = {2};
    c.if_then(0, 1, x_body);
    c.reset(0).h(0).measure(0, 1).measure(2, 2);
    return c;
}

NoiseModel test_noise() {
    NoiseModel nm;
    nm.add_depolarizing("h", 0.08);
    nm.add_amplitude_damping("h", 0.1);
    nm.add_depolarizing("cx", 0.06);
    nm.add_depolarizing("cz", 0.06);
    nm.add_bit_flip("x", 0.05);
    nm.add_readout_error(0, 0.03, 0.04);
    nm.add_readout_error(2, 0.02, 0.05);
    return nm;
}

Result run(SimType sim, const std::vector<std::uint32_t>& sites, bool ledger,
           std::uint32_t shots = 400, std::uint64_t seed = 20261020) {
    QRegister qr(sim, ProcType::CPU);
    qr.set_seed(seed);
    qr.set_noise_model(test_noise());
    qr.set_noise_interventions(sites);
    qr.set_noise_draw_ledger(ledger);
    return qr.run(test_circuit(), shots, false);
}

std::vector<double> draws(const std::vector<NoiseDrawEvent>& shot) {
    std::vector<double> u;
    for (const auto& e : shot) u.push_back(e.u);
    return u;
}

}  // namespace

// ── ② : no intervention and the ledger cost nothing ─────────────────────

TEST(NoiseIntervention, EmptyListLedgerOff_CountsEqualPlainRun) {
    for (SimType sim : {SimType::FullState, SimType::TensorNetwork}) {
        QRegister plain(sim, ProcType::CPU);
        plain.set_seed(20261020);
        plain.set_noise_model(test_noise());
        const auto base = plain.run(test_circuit(), 400, false).counts();

        const Result r = run(sim, {}, false);

        EXPECT_TRUE(r.counts() == base);
        EXPECT_TRUE(r.noise_draw_ledger().empty());
    }
}

TEST(NoiseIntervention, LedgerOn_DoesNotChangeCounts) {
    for (SimType sim : {SimType::FullState, SimType::TensorNetwork}) {
        const Result off = run(sim, {}, false);
        const Result on  = run(sim, {}, true);

        EXPECT_TRUE(on.counts() == off.counts());
        EXPECT_EQ(on.noise_draw_ledger().size(), std::size_t{400});
    }
}

// ── ① : an intervened site still consumes its draw ──────────────────────

TEST(NoiseIntervention, Intervention_EveryShotKeepsEveryUniform) {
    const Result base = run(SimType::FullState, {}, true);
    const Result cut  = run(SimType::FullState, {3}, true);

    std::size_t same_u = 0, only_target = 0;
    for (std::size_t s = 0; s < base.noise_draw_ledger().size(); ++s) {
        const auto& b = base.noise_draw_ledger()[s];
        const auto& c = cut.noise_draw_ledger()[s];
        if (draws(b) == draws(c)) ++same_u;
        if (c.size() > 3 && c[3].intervened && c[3].branch == 0u) ++only_target;
    }

    EXPECT_EQ(same_u, base.noise_draw_ledger().size());
    EXPECT_EQ(only_target, base.noise_draw_ledger().size());
}

// ── : an intervention is the identity ──────────────────────────────────

TEST(NoiseIntervention, CertainBitFlip_InterventionRemovesIt) {
    Circuit c(1, 1);
    c.id(0).measure(0, 0);
    NoiseModel nm;
    nm.add_bit_flip("id", 1.0);
    QRegister qr(SimType::FullState, ProcType::CPU);
    qr.set_seed(7);
    qr.set_noise_model(nm);
    const auto flipped = qr.run(c, 50, false).counts();

    qr.set_noise_interventions({0});
    const auto kept = qr.run(c, 50, false).counts();

    EXPECT_EQ(flipped.at("1"), std::uint64_t{50});
    EXPECT_EQ(kept.at("0"), std::uint64_t{50});
}

TEST(NoiseIntervention, CertainReadoutFlip_InterventionRemovesIt) {
    for (SimType sim : {SimType::FullState, SimType::TensorNetwork}) {
        Circuit c(1, 1);
        c.measure(0, 0);
        NoiseModel nm;
        nm.add_readout_error(0, 0.0, 1.0);   // a true 0 is always reported as 1
        QRegister qr(sim, ProcType::CPU);
        qr.set_seed(7);
        qr.set_noise_model(nm);
        qr.set_noise_interventions({0});
        qr.set_noise_draw_ledger(true);

        const Result r = qr.run(c, 50, false);

        EXPECT_EQ(r.counts().at("0"), std::uint64_t{50});
        EXPECT_TRUE(r.noise_draw_ledger()[0].size() == 1 &&
                    r.noise_draw_ledger()[0][0].intervened);
    }
}

// ── ④ : FullState CPU and TN share one site numbering ──────────────────

TEST(NoiseIntervention, FullStateAndTn_SameLedgerSameCounts) {
    const Result fs = run(SimType::FullState,     {2, 9}, true);
    const Result tn = run(SimType::TensorNetwork, {2, 9}, true);

    std::size_t same = 0;
    for (std::size_t s = 0; s < fs.noise_draw_ledger().size(); ++s) {
        const auto& a = fs.noise_draw_ledger()[s];
        const auto& b = tn.noise_draw_ledger()[s];
        bool eq = a.size() == b.size();
        for (std::size_t k = 0; eq && k < a.size(); ++k)
            eq = a[k].site == b[k].site && a[k].op_index == b[k].op_index &&
                 a[k].u == b[k].u && a[k].intervened == b[k].intervened;
        if (eq) ++same;
    }

    EXPECT_EQ(same, fs.noise_draw_ledger().size());
    EXPECT_TRUE(fs.counts() == tn.counts());
}

// ── ⓪ ⑤ : the ledger follows the canonical order, interventions or not ─

TEST(NoiseIntervention, LedgerOpOrder_IsCanonicalOrder_WithAndWithoutIntervention) {
    const Circuit circ = test_circuit();
    const std::vector<std::size_t> order =
        qplayer::detail::noise_trajectory_canonical_order(circ);
    std::map<std::size_t, std::size_t> rank;
    for (std::size_t k = 0; k < order.size(); ++k) rank[order[k]] = k;

    const Result base = run(SimType::FullState, {}, true, 50);
    const Result cut  = run(SimType::FullState, {0, 1, 2, 5}, true, 50);

    std::size_t monotone = 0, same_ops = 0;
    for (std::size_t s = 0; s < base.noise_draw_ledger().size(); ++s) {
        const auto& b = base.noise_draw_ledger()[s];
        bool mono = true;
        for (std::size_t k = 1; k < b.size(); ++k)
            mono = mono && rank[b[k - 1].op_index] <= rank[b[k].op_index];
        if (mono) ++monotone;
        // An intervention may change the measured bit and so whether the IfOp body
        // (op 5) runs; its sites legitimately come and go. Every other site keeps
        // its place, which is what "the hook never reschedules" means.
        std::vector<std::size_t> ob, oc;
        for (const auto& e : b) if (e.op_index != 5) ob.push_back(e.op_index);
        for (const auto& e : cut.noise_draw_ledger()[s]) if (e.op_index != 5) oc.push_back(e.op_index);
        if (ob == oc) ++same_ops;
    }

    EXPECT_EQ(monotone, base.noise_draw_ledger().size());
    EXPECT_EQ(same_ops, base.noise_draw_ledger().size());
}

// ── ③ : the list is echoed, sorted and de-duplicated ───────────────────

TEST(NoiseIntervention, InterventionList_EchoedSortedUnique) {
    const Result r = run(SimType::FullState, {5, 1, 5, 3}, false, 20);

    EXPECT_TRUE(r.noise_interventions() == (std::vector<std::uint32_t>{1, 3, 5}));
}

// ── : errors ──────────────────────────────────────────────────────────

TEST(NoiseIntervention, NoNoiseModel_Throws) {
    QRegister qr(SimType::FullState, ProcType::CPU);
    qr.set_noise_interventions({0});

    EXPECT_THROW(qr.run(test_circuit(), 10, false), SimulatorError);
}

TEST(NoiseIntervention, SiteBeyondDraws_Throws) {
    for (SimType sim : {SimType::FullState, SimType::TensorNetwork}) {
        EXPECT_THROW(run(sim, {100000}, false, 10), SimulatorError);
    }
}

TEST(NoiseIntervention, TnMpoRoute_Throws) {
    QRegister qr(SimType::TensorNetwork, ProcType::CPU);
    qr.set_noise_model(test_noise());
    qr.set_tn_noise_path("mpo");
    qr.set_noise_interventions({0});
    Circuit c(2, 2);
    c.h(0).cx(0, 1).measure(0, 0).measure(1, 1);

    EXPECT_THROW(qr.run(c, 10, false), SimulatorError);
}

TEST(NoiseIntervention, SparseState_Throws) {
    QRegister qr(SimType::SparseState, ProcType::CPU);
    qr.set_noise_model(test_noise());
    qr.set_noise_interventions({0});

    EXPECT_THROW(qr.run(test_circuit(), 10, false), SimulatorError);
}
