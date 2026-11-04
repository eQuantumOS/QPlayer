// test_sparse_partition_store.cpp — Unit tests for PersistentAmplitudeStore
// (Phase 8-E): the persistent, in-place, prime-modulo amplitude store
// that replaces the /0923 double-buffer store. These tests exercise the
// data structure in isolation — partition distribution, round-trip, canonical
// ordering, and the lock-free gate scratch — independent of the gate kernels.
// (Arrange/Act/Assert).

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"

#include <complex>
#include <cstdint>
#include <set>
#include <vector>

#include "../../../include/backend/sparse_state/partitioned_store.hpp"
#include "../../../include/types.hpp"

using qplayer::PersistentAmplitudeStore;
using qplayer::GateBuckets;
using qplayer::StateIndex;

namespace {

using cd = std::complex<double>;

// set_ground() seeds exactly one amplitude 1+0i at basis index 0 (partition 0).
TEST(SparsePartitionStore, SetGround_SingleAmplitudeAtZero) {
    // Arrange
    PersistentAmplitudeStore store;
    store.configure(5);

    // Act
    store.set_ground();

    // Assert
    EXPECT_EQ(store.size(), static_cast<std::size_t>(1));
    EXPECT_FALSE(store.empty());
    EXPECT_NEAR(store.get(StateIndex{0}).real(), 1.0, 1e-15);
    EXPECT_NEAR(store.get(StateIndex{0}).imag(), 0.0, 1e-15);
    EXPECT_EQ(store.partition_of(StateIndex{0}), static_cast<std::size_t>(0));
}

// put/get/erase are the sparse round-trip primitives; absent keys read as 0.
TEST(SparsePartitionStore, PutGetErase_RoundTrip) {
    // Arrange
    PersistentAmplitudeStore store;
    store.configure(8);

    // Act
    store.put(StateIndex{5}, cd{0.5, -0.5});
    store.put(StateIndex{200}, cd{0.1, 0.0});

    // Assert
    EXPECT_EQ(store.size(), static_cast<std::size_t>(2));
    EXPECT_NEAR(store.get(StateIndex{5}).real(), 0.5, 1e-15);
    EXPECT_NEAR(store.get(StateIndex{5}).imag(), -0.5, 1e-15);
    EXPECT_NEAR(store.get(StateIndex{999}).real(), 0.0, 1e-15);  // absent → 0

    store.erase(StateIndex{5});
    EXPECT_EQ(store.size(), static_cast<std::size_t>(1));
    EXPECT_NEAR(store.get(StateIndex{5}).real(), 0.0, 1e-15);    // erased → 0
}

// configure() resolves a prime partition count (default 1171) and clears state.
TEST(SparsePartitionStore, Configure_PartitionCount_PositiveAndClears) {
    // Arrange
    PersistentAmplitudeStore store;
    store.configure(10);
    store.put(StateIndex{3}, cd{1.0, 0.0});

    // Act — reconfigure clears all amplitudes.
    store.configure(10);

    // Assert
    EXPECT_TRUE(store.empty());
    EXPECT_TRUE(store.num_partitions() > static_cast<std::size_t>(1));
    // partition_of is consistent with the reported partition count.
    EXPECT_EQ(store.partition_of(StateIndex{12345}),
              static_cast<std::size_t>(StateIndex{12345} % StateIndex{store.num_partitions()}));
}

// Prime modulo spreads consecutive basis indices across many partitions (no
// top-bit skew): indices 0..N with N < P land in N+1 distinct partitions.
TEST(SparsePartitionStore, PartitionOf_PrimeModulo_Distributes) {
    // Arrange
    PersistentAmplitudeStore store;
    store.configure(12);
    const std::size_t P = store.num_partitions();

    // Act — collect the partitions used by the first min(500, P) indices.
    const std::uint64_t n = static_cast<std::uint64_t>(P < 500 ? P : 500);
    std::set<std::size_t> used;
    for (std::uint64_t i = 0; i < n; ++i) {
        used.insert(store.partition_of(StateIndex{i}));
    }

    // Assert — consecutive indices below P are all in distinct partitions.
    EXPECT_EQ(used.size(), static_cast<std::size_t>(n));
    EXPECT_TRUE(used.size() > static_cast<std::size_t>(1));
}

// Gate kernels mutate amplitudes in place via direct shard access.
TEST(SparsePartitionStore, InPlaceUpdate_ViaShard) {
    // Arrange
    PersistentAmplitudeStore store;
    store.configure(6);
    store.put(StateIndex{3}, cd{1.0, 0.0});

    // Act — in-place amplitude update under the owning shard.
    auto& shard = store.shard(store.partition_of(StateIndex{3}));
    shard[StateIndex{3}] = cd{2.0, 0.0};

    // Assert
    EXPECT_NEAR(store.get(StateIndex{3}).real(), 2.0, 1e-15);
}

// The lock-free gate scratch is sized per thread × target partition, and
// clear_scratch() empties it (keeping capacity) for reuse.
TEST(SparsePartitionStore, GateScratch_SizedAndClearable) {
    // Arrange
    PersistentAmplitudeStore store;
    store.configure(8);
    const std::size_t P = store.num_partitions();

    // Act — a thread stages a set and a delete into its buckets, then clear.
    auto& scratch = store.scratch();
    EXPECT_TRUE(scratch.size() >= static_cast<std::size_t>(1));  // >=1 thread
    EXPECT_EQ(scratch[0].set.size(), P);   // keyed by target partition
    EXPECT_EQ(scratch[0].del.size(), P);
    scratch[0].set[store.partition_of(StateIndex{7})].emplace_back(StateIndex{7}, cd{0.5, 0.0});
    scratch[0].del[store.partition_of(StateIndex{9})].push_back(StateIndex{9});

    store.clear_scratch();

    // Assert — buckets emptied.
    EXPECT_EQ(scratch[0].set[store.partition_of(StateIndex{7})].size(), static_cast<std::size_t>(0));
    EXPECT_EQ(scratch[0].del[store.partition_of(StateIndex{9})].size(), static_cast<std::size_t>(0));
}

// sorted_entries() returns amplitudes in ascending StateIndex order regardless
// of shard placement — the canonical reduction order for reproducibility.
TEST(SparsePartitionStore, SortedEntries_AscendingByStateIndex) {
    // Arrange
    PersistentAmplitudeStore store;
    store.configure(10);
    store.put(StateIndex{500}, cd{0.1, 0.0});
    store.put(StateIndex{2},   cd{0.2, 0.0});
    store.put(StateIndex{77},  cd{0.3, 0.0});

    // Act
    const auto entries = store.sorted_entries();

    // Assert — ascending index order, amplitudes preserved.
    EXPECT_EQ(entries.size(), static_cast<std::size_t>(3));
    EXPECT_TRUE(entries[0].first < entries[1].first);
    EXPECT_TRUE(entries[1].first < entries[2].first);
    EXPECT_TRUE(entries[0].first == StateIndex{2});    // StateIndex is __uint128 (no ostream<<)
    EXPECT_TRUE(entries[2].first == StateIndex{500});
    EXPECT_NEAR(entries[1].second.real(), 0.3, 1e-15);  // index 77
}

// The deterministic parallel sort produces a fully sorted, unique order for a
// large input (exercises the multi-block merge path, not just the serial cap).
TEST(SparsePartitionStore, SortedEntries_LargeInput_FullyOrdered) {
    // Arrange — enough entries to exceed the serial-sort cutoff (2^16).
    PersistentAmplitudeStore store;
    store.configure(20);
    const std::uint64_t N = 100000;
    for (std::uint64_t i = 0; i < N; ++i) {
        // Scatter indices so shards and blocks are non-trivially interleaved.
        const std::uint64_t idx = (i * 2654435761ULL) % 1000003ULL;
        store.put(StateIndex{idx}, cd{static_cast<double>(i % 7) + 1.0, 0.0});
    }

    // Act
    const auto entries = store.sorted_entries();

    // Assert — strictly ascending (unique keys), count matches distinct inserts.
    EXPECT_EQ(entries.size(), store.size());
    bool ordered = true;
    for (std::size_t k = 1; k < entries.size(); ++k) {
        if (!(entries[k - 1].first < entries[k].first)) { ordered = false; break; }
    }
    EXPECT_TRUE(ordered);
}

}  // namespace
