// test_statevector_dump.cpp — Unit tests for QRegister::dump_statevector()
// (state-access family). Verifies the oracle.data text format: one
// line per nonzero-amplitude basis state
//   [P=%.6f] [%+.8f, %+.8f] |<bitstring>>
// with MSB->LSB bit ordering (q[n-1]..q[0]), followed by a trailing header line
// reporting the count of emitted (nonzero) states..

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"

#include <complex>
#include <cstddef>
#include <sstream>
#include <string>
#include <vector>

#include "../../../include/qregister.hpp"
#include "../../../include/circuit/circuit.hpp"

using qplayer::Circuit;
using qplayer::QRegister;
using qplayer::SimType;

namespace {

// Split a dump into its non-empty lines (header + per-state lines).
std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream iss{s};
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty()) out.push_back(line);
    }
    return out;
}

// Parse the probability field "[P=<f>]" from a dump body line.
double parse_p(const std::string& l) {
    const auto a = l.find("[P=") + 3;
    const auto b = l.find(']', a);
    return std::stod(l.substr(a, b - a));
}

}  // namespace

// Ground state |00>: only index 0 is nonzero, so a single state line is emitted
// and the trailing header reports 1 state.
TEST(StatevectorDump, GroundState_TwoQubits_FormatsHeaderAndAmplitudes) {
    // Arrange
    QRegister qr{SimType::FullState};
    Circuit   circ{2, 2};

    // Act
    qr.run(circ, 1);
    std::ostringstream oss;
    qr.dump_statevector(oss);
    const auto lines = split_lines(oss.str());

    // Assert
    EXPECT_EQ(lines.size(), static_cast<std::size_t>(2));  // 1 state + trailing header
    EXPECT_EQ(lines[0],
              std::string("[P=1.000000] [+1.00000000, "
                          "+0.00000000] |00>"));
    EXPECT_TRUE(lines[1].rfind("==============", 0) == 0);  // header starts with '='
    EXPECT_TRUE(lines[1].find("dump 1 states") != std::string::npos);
}

// X on q0 lands amplitude on basis index 1, whose bitstring is "01"
// (MSB->LSB): the rightmost column is q0. Guards bit ordering.
TEST(StatevectorDump, XOnQubit0_TwoQubits_SetsRightmostBit) {
    // Arrange
    QRegister qr{SimType::FullState};
    Circuit   circ{2, 2};
    circ.x(0);

    // Act
    qr.run(circ, 1);
    std::ostringstream oss;
    qr.dump_statevector(oss);
    const auto lines = split_lines(oss.str());

    // Assert
    EXPECT_EQ(lines.size(), static_cast<std::size_t>(2));  // 1 state + trailing header
    EXPECT_EQ(lines[0],
              std::string("[P=1.000000] [+1.00000000, "
                          "+0.00000000] |01>"));
}

// Hadamard superposition: each basis state has P printed at %.6f rounding to
// 0.500000, matching |amp|^2 = 0.5.
TEST(StatevectorDump, Hadamard_OneQubit_ProbabilityMatchesAmplitudeNorm) {
    // Arrange
    QRegister qr{SimType::FullState};
    Circuit   circ{1, 1};
    circ.h(0);

    // Act
    qr.run(circ, 1);
    std::ostringstream oss;
    qr.dump_statevector(oss);
    const auto lines = split_lines(oss.str());

    // Assert
    EXPECT_EQ(lines.size(), static_cast<std::size_t>(3));  // 2 states + trailing header
    EXPECT_NEAR(parse_p(lines[0]), 0.5, 1e-9);  // %.6f rounds to 0.500000
    EXPECT_NEAR(parse_p(lines[1]), 0.5, 1e-9);
    EXPECT_TRUE(lines[2].find("dump 2 states") != std::string::npos);  // trailing header
}

// ── : nonzero_amplitudes() tests ──────────────────────────────────────

// FullState CPU: get_statevector_nonzero() returns the two Bell basis states in
// ascending StateIndex order with |amp|^2 = 0.5 each.
TEST(StatevectorDump, GetNonzeroAmplitudes_FullState_BellState_TwoEntries) {
    // Arrange
    QRegister qr{SimType::FullState, qplayer::ProcType::CPU};
    Circuit   circ{2, 0};
    circ.h(0).cx(0, 1);

    // Act
    qr.run(circ, 1);
    const auto amps = qr.get_statevector_nonzero();

    // Assert
    EXPECT_EQ(amps.size(), static_cast<std::size_t>(2));  // |00> and |11>
    // StateIndex is __uint128_t; cast to uint64 for EXPECT_EQ (both fit in 64 bits here).
    // Ascending: idx 0 (|00>) before idx 3 (|11>).
    EXPECT_EQ(static_cast<std::uint64_t>(amps[0].first), std::uint64_t{0});
    EXPECT_EQ(static_cast<std::uint64_t>(amps[1].first), std::uint64_t{3});
    EXPECT_NEAR(std::norm(amps[0].second), 0.5, 1e-10);
    EXPECT_NEAR(std::norm(amps[1].second), 0.5, 1e-10);
}

// SparseState: get_statevector_nonzero() works above MAX_STATEVECTOR_QUBITS=25
// without densifying. H on qubit 0 of a 30-qubit circuit produces exactly 2
// nonzero states at indices 0 and 1.
TEST(StatevectorDump, GetNonzeroAmplitudes_SparseState_30q_NoDensifyError) {
    // Arrange — 30 qubits is above the MAX_STATEVECTOR_QUBITS=25 densify cap.
    QRegister qr{SimType::SparseState};
    Circuit   circ{30, 0};
    circ.h(0);  // Only qubit 0 in superposition; all others in |0>.

    // Act — get_statevector() would throw here; nonzero_amplitudes() must not.
    qr.run(circ, 1);
    const auto amps = qr.get_statevector_nonzero();

    // Assert
    EXPECT_EQ(amps.size(), static_cast<std::size_t>(2));
    EXPECT_EQ(static_cast<std::uint64_t>(amps[0].first), std::uint64_t{0});
    EXPECT_EQ(static_cast<std::uint64_t>(amps[1].first), std::uint64_t{1});
    EXPECT_NEAR(std::norm(amps[0].second), 0.5, 1e-10);
    EXPECT_NEAR(std::norm(amps[1].second), 0.5, 1e-10);
}

// SparseState: dump_statevector() on a 30-qubit circuit completes without error
// and emits the correct 30-character bitstrings in the oracle format.
// This is the primary regression guard for the densify-bypass rewrite.
TEST(StatevectorDump, DumpStatevector_SparseState_30q_NoDensifyError) {
    // Arrange — 30 qubits is above the MAX_STATEVECTOR_QUBITS=25 densify cap.
    QRegister qr{SimType::SparseState};
    Circuit   circ{30, 0};
    circ.h(0);

    // Act
    qr.run(circ, 1);
    std::ostringstream oss;
    EXPECT_NO_THROW(qr.dump_statevector(oss));
    const auto lines = split_lines(oss.str());

    // Assert: 2 state lines + 1 trailing header.
    EXPECT_EQ(lines.size(), static_cast<std::size_t>(3));
    // Each bitstring is 30 characters wide (|<30 bits>>).
    const std::string& l0 = lines[0];
    const auto bar0 = l0.rfind('|');
    EXPECT_TRUE(bar0 != std::string::npos);
    // bitstring length = characters between '|' and '>': size - bar0 - 2.
    EXPECT_EQ(l0.size() - bar0 - 2u, static_cast<std::size_t>(30));
    EXPECT_TRUE(lines[2].find("dump 2 states") != std::string::npos);
}

// ── : nonzero filter threshold unification (|amp| >= 1e-12) ──────────

// FullState CPU: an amplitude below kNonzeroAmplitudeThreshold (1e-12) is treated
// as zero. ry(2e-13) puts ~1e-13 amplitude into |1> (sin(theta/2) ~ 1e-13), which
// is below the threshold, so only |0> is enumerated ( — previously the
// |amp|^2 > 0 filter would have returned 2 entries including the dust).
TEST(StatevectorDump, GetNonzeroAmplitudes_FullState_BelowThreshold_Excluded) {
    // Arrange — tiny rotation produces a sub-threshold |1> amplitude.
    QRegister qr{SimType::FullState, qplayer::ProcType::CPU};
    Circuit   circ{1, 0};
    circ.ry(2e-13, 0);  // |1> amplitude = sin(1e-13) ~ 1e-13 < 1e-12.

    // Act
    qr.run(circ, 1);
    const auto amps = qr.get_statevector_nonzero();

    // Assert — only |0> survives the threshold; the dust |1> is filtered out.
    EXPECT_EQ(amps.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(static_cast<std::uint64_t>(amps[0].first), std::uint64_t{0});
}

// Cross-backend consistency: the same circuit yields the identical nonzero index
// set on FullState CPU and SparseState now that both filter at |amp| >= 1e-12
// A meaningful (above-threshold) superposition must agree exactly.
TEST(StatevectorDump, GetNonzeroAmplitudes_FullStateVsSparse_SameIndexSet) {
    // Arrange — GHZ-like 3-qubit state: nonzero only at |000> and |111>.
    Circuit circ{3, 0};
    circ.h(0).cx(0, 1).cx(1, 2);

    QRegister qr_full{SimType::FullState, qplayer::ProcType::CPU};
    QRegister qr_sparse{SimType::SparseState};

    // Act
    qr_full.run(circ, 1);
    qr_sparse.run(circ, 1);
    const auto full   = qr_full.get_statevector_nonzero();
    const auto sparse = qr_sparse.get_statevector_nonzero();

    // Assert — identical index set (both ascending, both filtered at 1e-12).
    EXPECT_EQ(full.size(), sparse.size());
    EXPECT_EQ(full.size(), static_cast<std::size_t>(2));
    for (std::size_t i = 0; i < full.size(); ++i) {
        EXPECT_EQ(static_cast<std::uint64_t>(full[i].first),
                  static_cast<std::uint64_t>(sparse[i].first));
    }
}

// Interactive SparseState: get_statevector_nonzero() works on an active session
// without densifying.
TEST(StatevectorDump, Interactive_GetNonzeroAmplitudes_SparseState_30q) {
    // Arrange
    QRegister qr{SimType::SparseState, qplayer::ProcType::CPU};
    qr.interactive.initialize(30);
    qr.interactive.h(0);

    // Act
    const auto amps = qr.interactive.get_statevector_nonzero();

    // Assert
    EXPECT_EQ(amps.size(), static_cast<std::size_t>(2));
    EXPECT_EQ(static_cast<std::uint64_t>(amps[0].first), std::uint64_t{0});
    EXPECT_EQ(static_cast<std::uint64_t>(amps[1].first), std::uint64_t{1});
}
