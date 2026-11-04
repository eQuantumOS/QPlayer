// test_compile_swap_roundtrip.cpp — Phase 8 integration test. Mirrors the
// compile_swap.qasm acceptance case: a logical CX between non-adjacent qubits on
// a linear coupling map forces SWAP insertion, yet the measured distribution
// must still be correct on the LOGICAL qubits. After transpile() the
// physical circuit gains SWAPs; run() output is logical-indexed because the
// measure operands follow their qubits' physical homes.

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"

#include <string>

#include "../../../include/circuit/circuit.hpp"
#include "../../../include/qregister.hpp"

using qplayer::Circuit;
using qplayer::QRegister;
using qplayer::SimType;

namespace {

// h q0 ; cx q0,q3 ; x q1 ; x q2 ; measure all — logical outcomes are
// {q0,q1,q2,q3} = {0,1,1,0} or {1,1,1,1}. Bitstring is c3 c2 c1 c0 (MSB-first).
Circuit make_compile_swap_circuit() {
    Circuit c(4, 4);
    c.h(0).cx(0, 3).x(1).x(2)
     .measure(0, 0).measure(1, 1).measure(2, 2).measure(3, 3);
    return c;
}

}  // namespace

TEST(CompileSwapRoundtrip, LinearChain_LogicalDistributionPreserved) {
    // Arrange — linear chain 0-1-2-3 forces routing of the logical cx(0,3).
    QRegister qr(SimType::FullState);
    qr.set_seed(42);
    Circuit routed = qr.transpile(make_compile_swap_circuit(), 1,
                                  R"({"coupling_map":[[0,1],[1,2],[2,3]]})");

    // Act
    const std::uint32_t nshots = 10000;
    auto result = qr.run(routed, nshots);
    const auto& counts = result.counts();

    // Assert — only the two logical outcomes appear, each ~50% (tol 5%).
    auto prob = [&](const std::string& bs) {
        auto it = counts.find(bs);
        return it == counts.end()
                   ? 0.0
                   : static_cast<double>(it->second) / static_cast<double>(nshots);
    };
    EXPECT_NEAR(prob("0110"), 0.5, 0.05);  // {q0,q1,q2,q3}={0,1,1,0}
    EXPECT_NEAR(prob("1111"), 0.5, 0.05);  // {1,1,1,1}

    // No leakage into other bitstrings beyond tolerance.
    double other = 0.0;
    for (const auto& [bs, cnt] : counts) {
        if (bs != "0110" && bs != "1111") {
            other += static_cast<double>(cnt) / static_cast<double>(nshots);
        }
    }
    EXPECT_NEAR(other, 0.0, 0.05);
}

TEST(CompileSwapRoundtrip, RoutingInsertsSwaps) {
    // Sanity: the routed circuit really contains extra ops (SWAPs) versus the
    // 8-op logical source, confirming routing happened (not a silent no-op).
    QRegister qr(SimType::FullState);
    Circuit routed = qr.transpile(make_compile_swap_circuit(), 1,
                                  R"({"coupling_map":[[0,1],[1,2],[2,3]]})");
    EXPECT_TRUE(routed.num_ops() > make_compile_swap_circuit().num_ops());
}
