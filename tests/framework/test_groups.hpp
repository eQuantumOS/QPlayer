// test_groups.hpp — task-group mapping for the QPlayer test runner (SSOT).
// Maps each test suite name to exactly one task group so the runner can select
// and summarise tests by group (--group=parser,noise ...). Keeping the mapping
// central means TEST()/TEST_F()/TEST_BACKENDS() call sites need no edits when a
// group is added. A self-test (test_framework.cpp) asserts every registered
// suite resolves to a known group, enforcing this file as the single source of
// truth.
//
// Grouping is by exact suite name (deterministic). A newly added or renamed
// suite that is not listed here fails the completeness self-test, prompting the
// developer to classify it. Suites that only exist in GPU builds (e.g.
// FullStateGpuKernel) may be listed unconditionally; unlisted-but-absent suites
// are simply never looked up.

#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace qplayer::test {

// A task group: display name + the suites it owns (exact suite names).
struct GroupDef {
    std::string name;
    std::vector<std::string> suites;
};

// Ordered group definitions. Order controls display order in summaries and
// --list-groups. Each suite must appear in exactly one group.
inline const std::vector<GroupDef>& group_defs() {
    static const std::vector<GroupDef> defs = {
        {"parser", {
            "Lexer", "Parser", "ParserAst", "SemanticValidator", "CircuitBuilder",
            "ConventionFixture", "Broadcast", "Circuit", "CircuitIR", "CircuitMetadata",
        }},
        {"backend", {
            "FullStateFixture", "FullStateGpuKernel", "FullStateGpuExecute",
            "FullStateGpuFactory", "SparseStateFixture", "SparseState",
            "SparseStateParity", "SparsePartitionStore",
            "StatevectorDump", "Observable", "AddExpectation", "GoogleGates",
            "EcrGate", "MemoryCheckFixture", "BellState", "GHZState",
            "BellExpectation", "Interactive", "QRegisterPipeline", "LazySampling",
        }},
        {"golden", {
            "Golden", "GoldenGpu",
        }},
        {"optimizer", {
            "CancelInversePass", "CommutationReorderPass", "MergeSingleQubitPass",
            "MergeSingleQubitUnitaryPass", "MergeTwoQubitPass", "MergeSameGatePass",
            "DiagonalPhasePass", "RemoveIdentityPass", "RemoveIdleQubitPass",
            "PatternReplacePass", "InitialLayout", "SwapRouter", "SwapCancel",
            "BasisGateTranslation", "ALAPSchedule", "DurationAnnotation",
            "SemanticValidatorPass", "QubitTruncation", "PassManager", "PassChain",
            "Transpiler", "QRegisterTranspile", "TranspileAcceptanceQasm",
            "TranspileDeviceJson", "Compiler", "CompileSwapRoundtrip",
            "ExecuteFixture", "FusionFixture", "CacheBlockingFixture",
            "CacheBlocking", "QubitReorder", "ReorderFixture", "CircuitSerializer",
            "CircuitDraw", "QasmEmitter", "JsonValue",
        }},
        {"noise", {
            "NoiseModel", "NoiseModelReadout", "NoiseCpu", "NoiseGpu",
            "DeviceModel", "DeviceNoise", "T1T2Calculator", "QRegisterLoadDevice",
            "WillowDevice", "IdleNoiseInserter",
        }},
        {"parallel", {
            "NoiseParity", "SeedReproducibility", "SeedFixture", "Seed",
            "CpuDispatcher",
        }},
        {"primitives", {
            "Primitives", "PrimitivesVQE",
        }},
        {"framework", {
            "Framework", "Phase0S", "FilterSanity", "CounterFixture", "Log",
            "QTimer",
        }},
    };
    return defs;
}

// Reverse index: suite name -> group name. Built once from group_defs().
inline const std::unordered_map<std::string, std::string>& suite_to_group() {
    static const std::unordered_map<std::string, std::string> m = [] {
        std::unordered_map<std::string, std::string> out;
        for (const auto& g : group_defs()) {
            for (const auto& s : g.suites) {
                out.emplace(s, g.name);
            }
        }
        return out;
    }();
    return m;
}

// Returns the group owning `suite`, or empty string if unmapped.
inline std::string group_of(const std::string& suite) {
    const auto& m = suite_to_group();
    const auto it = m.find(suite);
    return it == m.end() ? std::string() : it->second;
}

// Returns true when `name` is a defined group.
inline bool is_known_group(const std::string& name) {
    for (const auto& g : group_defs()) {
        if (g.name == name) return true;
    }
    return false;
}

// Ordered group names (for --list-groups and summary ordering).
inline std::vector<std::string> group_names() {
    std::vector<std::string> names;
    names.reserve(group_defs().size());
    for (const auto& g : group_defs()) names.push_back(g.name);
    return names;
}

}  // namespace qplayer::test
