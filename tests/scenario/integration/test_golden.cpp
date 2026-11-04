// test_golden.cpp -- Golden test suite.
// Verifies four reference circuits against baseline JSON files stored in
// tests/data/golden/. Baseline creation is a one-time operation ( permits
// initial generation); subsequent updates require an ADR.
//
// Circuits covered:
//   Bell   : 2q, 10000 shots -- distribution (fraction tolerance), CPU+GPU
//   GHZ-5  : 5q, 10000 shots -- distribution (fraction tolerance), CPU+GPU
//   QFT-4  : 4q, 1 shot      -- statevector L2 amplitude error <= 1e-10, CPU+GPU
//   Clifford-10 : 10q, 1000 shots, fixed seed -- exact count match, CPU only
// (GPU sampling is not bit-identical to CPU)

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"
#include "../../framework/backend_params.hpp"  // TEST_BACKENDS,

#include <cmath>
#include <complex>
#include <cstdint>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "../../../include/qregister.hpp"
#include "../../../include/circuit/circuit.hpp"
#include "../../../include/circuit/detail/json_value.hpp"

using qplayer::Circuit;
using qplayer::ProcType;
using qplayer::QRegister;
using qplayer::SimType;
using qplayer::Result;
using qplayer::StateVector;
using qplayer::detail::JsonValue;
using qplayer::detail::parse_json;

// QPLAYER_GOLDEN_DIR is injected by CMakeLists.txt as an absolute path.
#ifndef QPLAYER_GOLDEN_DIR
#error "QPLAYER_GOLDEN_DIR must be defined by the build system"
#endif

namespace {

// Read an entire file into a string.
std::string read_file(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        throw std::runtime_error("cannot open golden file: " + path);
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Return the fraction of a bitstring key in counts, or 0 if absent.
double frac(const std::map<std::string, std::uint64_t>& counts,
            const std::string& key,
            std::uint32_t nshots) {
    auto it = counts.find(key);
    return it == counts.end()
               ? 0.0
               : static_cast<double>(it->second) / static_cast<double>(nshots);
}

// Build the 4-qubit QFT circuit on |0000> (matching qft_4.qasm).
// H on each qubit plus controlled-phase (cu1) ladder plus swap bit-reversal.
Circuit make_qft4() {
    const double pi = 3.14159265358979323846;
    Circuit circ(4);

    // QFT layer: qubit 0
    circ.h(0);
    circ.cu1(pi / 2.0, 1, 0);
    circ.cu1(pi / 4.0, 2, 0);
    circ.cu1(pi / 8.0, 3, 0);

    // QFT layer: qubit 1
    circ.h(1);
    circ.cu1(pi / 2.0, 2, 1);
    circ.cu1(pi / 4.0, 3, 1);

    // QFT layer: qubit 2
    circ.h(2);
    circ.cu1(pi / 2.0, 3, 2);

    // QFT layer: qubit 3
    circ.h(3);

    // Bit-reversal swaps to match standard QFT output ordering.
    circ.swap(0, 3);
    circ.swap(1, 2);

    return circ;
}

// Build the Bell circuit: H(0), CX(0,1), measure both.
Circuit make_bell() {
    Circuit circ(2, 2);
    circ.h(0).cx(0, 1).measure(0, 0).measure(1, 1);
    return circ;
}

// Build the GHZ-5 circuit: H(0), CX ladder, measure_all.
Circuit make_ghz5() {
    Circuit circ(5, 5);
    circ.h(0).cx(0, 1).cx(1, 2).cx(2, 3).cx(3, 4).measure_all();
    return circ;
}

// Build the fixed 10-qubit Clifford circuit used for baseline capture.
// Circuit: H(0) + CX ladder (0->9) + S(0) + measure_all.
// Produces a GHZ-like distribution; exact counts are seed-deterministic.
Circuit make_clifford10() {
    Circuit circ(10, 10);
    circ.h(0);
    circ.cx(0, 1).cx(1, 2).cx(2, 3).cx(3, 4)
        .cx(4, 5).cx(5, 6).cx(6, 7).cx(7, 8).cx(8, 9);
    circ.s(0);
    circ.measure_all();
    return circ;
}

}  // namespace

// ── Golden.Bell ─────────────────────────────────────────────────────────────
// Verifies Bell pair measurement distribution against bell_state.json.
// Tolerance: |observed_frac - expected_frac| <= baseline tol (10000-shot).
// Runs on both CPU and GPU (TEST_BACKENDS); distribution is
// backend-independent within the generous statistical tolerance.

TEST_BACKENDS(Golden, Bell_Distribution_MatchesBaseline) {
    // Arrange: load baseline
    const std::string path = std::string(QPLAYER_GOLDEN_DIR) + "/bell_state.json";
    const JsonValue   json = parse_json(read_file(path));

    const auto& exp_counts_obj = json.at("expected_counts").as_object();
    const double tol     = json.at("tolerance").as_double();
    const auto   nshots  = static_cast<std::uint32_t>(json.at("nshots").as_int());

    QRegister qr(SimType::FullState, proc);  // proc is CPU or GPU
    qr.set_seed(42);
    Circuit circ = make_bell();

    // Act
    Result r = qr.run(circ, nshots);
    const auto& counts = r.counts();

    // Assert: each baseline entry must be within tolerance.
    // Also verify absent outcomes stay at zero (complete support check).
    for (const auto& [key, exp_val] : exp_counts_obj) {
        const double exp_frac = exp_val.as_double() / static_cast<double>(nshots);
        const double obs_frac = frac(counts, key, nshots);
        EXPECT_NEAR(obs_frac, exp_frac, tol);  // 10000-shot statistical tolerance
    }
    // Impossible outcomes must be absent.
    EXPECT_NEAR(frac(counts, "01", nshots), 0.0, 1e-9);
    EXPECT_NEAR(frac(counts, "10", nshots), 0.0, 1e-9);
}

// ── Golden.GHZ ──────────────────────────────────────────────────────────────
// Verifies GHZ-5 measurement distribution against ghz_5q.json.
// Runs on both CPU and GPU (TEST_BACKENDS).

TEST_BACKENDS(Golden, GHZ5_Distribution_MatchesBaseline) {
    // Arrange: load baseline
    const std::string path = std::string(QPLAYER_GOLDEN_DIR) + "/ghz_5q.json";
    const JsonValue   json = parse_json(read_file(path));

    const auto& exp_counts_obj = json.at("expected_counts").as_object();
    const double tol    = json.at("tolerance").as_double();
    const auto   nshots = static_cast<std::uint32_t>(json.at("nshots").as_int());

    QRegister qr(SimType::FullState, proc);  // proc is CPU or GPU
    qr.set_seed(99);
    Circuit circ = make_ghz5();

    // Act
    Result r = qr.run(circ, nshots);
    const auto& counts = r.counts();

    // Assert: each baseline entry within tolerance.
    for (const auto& [key, exp_val] : exp_counts_obj) {
        const double exp_frac = exp_val.as_double() / static_cast<double>(nshots);
        const double obs_frac = frac(counts, key, nshots);
        EXPECT_NEAR(obs_frac, exp_frac, tol);  // 10000-shot statistical tolerance
    }
    // GHZ has exactly two outcomes in its support.
    EXPECT_EQ(counts.size(), std::size_t{2});
}

// ── Golden.QFT ──────────────────────────────────────────────────────────────
// Applies 4-qubit QFT on |0000> and checks statevector L2 error <= 1e-10.
// Input |0000> -> QFT output is uniform: all 16 amplitudes = 0.25 + 0i.
// Runs on both CPU and GPU (TEST_BACKENDS); L2 error is exact
// arithmetic (no statistical sampling) so both procs must satisfy 1e-10.

TEST_BACKENDS(Golden, QFT4_Statevector_L2Error) {
    // Arrange: load baseline
    const std::string path = std::string(QPLAYER_GOLDEN_DIR) + "/qft_4q.json";
    const JsonValue   json = parse_json(read_file(path));

    const auto&  exp_amps  = json.at("expected_amplitudes").as_array();
    const double l2_tol    = json.at("l2_tolerance").as_double();

    QRegister qr(SimType::FullState, proc);  // proc is CPU or GPU
    Circuit   circ = make_qft4();

    // Act: nshots=1, no measurement -> get_statevector returns |psi_final>.
    qr.run(circ, 1);
    const StateVector sv = qr.get_statevector();

    // Assert: L2 error between computed amplitudes and baseline.
    // Baseline: all 16 entries are [0.25, 0.0] (uniform superposition).
    EXPECT_EQ(sv.size(), exp_amps.size());

    double l2_sq = 0.0;
    for (std::size_t i = 0; i < sv.size(); ++i) {
        const auto& amp_pair = exp_amps[i].as_array();
        const double exp_re = amp_pair[0].as_double();
        const double exp_im = amp_pair[1].as_double();
        const double dr = sv[i].real() - exp_re;
        const double di = sv[i].imag() - exp_im;
        l2_sq += dr * dr + di * di;
    }
    const double l2_err = std::sqrt(l2_sq);
    // L2 error must be <= 1e-10 (exact arithmetic).
    EXPECT_TRUE(l2_err <= l2_tol);
}

// ── Golden.Clifford ──────────────────────────────────────────────────────────
// Verifies Clifford-10 counts exactly match baseline (seed-deterministic).
// exact:true means serial sampling with the same seed must reproduce counts.
// CPU-only: GPU sampling uses a parallel CDF scan whose bit pattern differs
// from CPU serial sampling, so exact count equality cannot hold across procs
// Uses explicit ProcType::CPU to remain correct in GPU
// builds where ProcType::Auto would otherwise resolve to GPU.

TEST(Golden, Clifford10_ExactCounts_SeedDeterministic) {
    // Arrange: load baseline
    const std::string path = std::string(QPLAYER_GOLDEN_DIR) + "/clifford_10q.json";
    const JsonValue   json = parse_json(read_file(path));

    const auto& exp_counts_obj = json.at("expected_counts").as_object();
    const auto  seed   = static_cast<std::uint64_t>(json.at("seed").as_int());
    const auto  nshots = static_cast<std::uint32_t>(json.at("nshots").as_int());

    QRegister qr(SimType::FullState, ProcType::CPU);  // CPU: bit-exact seed
    qr.set_seed(seed);
    Circuit circ = make_clifford10();

    // Act
    Result r = qr.run(circ, nshots);
    const auto& counts = r.counts();

    // Assert: exact count match — serial sampling is deterministic.
    // (: exact:true => identical counts, not just fraction).
    EXPECT_EQ(counts.size(), exp_counts_obj.size());

    for (const auto& [key, exp_val] : exp_counts_obj) {
        const auto exp_count = static_cast<std::uint64_t>(exp_val.as_int());
        auto it = counts.find(key);
        EXPECT_TRUE(it != counts.end());
        if (it != counts.end()) {
            EXPECT_EQ(it->second, exp_count);  // exact integer match
        }
    }
}
