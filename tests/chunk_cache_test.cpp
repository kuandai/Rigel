#include "TestFramework.h"

#include "Rigel/Voxel/ChunkCache.h"

#include <unordered_set>

using namespace Rigel::Voxel;

TEST_CASE(ChunkCache_EvictsLRU) {
    ChunkCache cache;
    cache.setMaxChunks(2);

    ChunkCoord a{0, 0, 0};
    ChunkCoord b{1, 0, 0};
    ChunkCoord c{2, 0, 0};

    cache.touch(a);
    cache.touch(b);
    cache.touch(c);

    std::unordered_set<ChunkCoord, ChunkCoordHash> protectedSet;
    auto evicted = cache.evict(protectedSet);
    CHECK_EQ(evicted.size(), static_cast<size_t>(1));
    CHECK_EQ(evicted[0].x, 0);
    CHECK_EQ(cache.size(), static_cast<size_t>(2));
}

TEST_CASE(ChunkCache_ProtectedSet) {
    ChunkCache cache;
    cache.setMaxChunks(1);

    ChunkCoord a{0, 0, 0};
    ChunkCoord b{1, 0, 0};
    cache.touch(a);
    cache.touch(b);

    std::unordered_set<ChunkCoord, ChunkCoordHash> protectedSet;
    protectedSet.insert(a);

    auto evicted = cache.evict(protectedSet);
    CHECK_EQ(evicted.size(), static_cast<size_t>(1));
    CHECK_EQ(evicted[0].x, 1);
}

TEST_CASE(ChunkCache_AllProtectedEntriesRemainResidentBeyondLimit) {
    ChunkCache cache;
    cache.setMaxChunks(2);

    ChunkCoord a{0, 0, 0};
    ChunkCoord b{1, 0, 0};
    ChunkCoord c{2, 0, 0};
    ChunkCoord d{3, 0, 0};
    cache.touch(a);
    cache.touch(b);
    cache.touch(c);

    std::unordered_set<ChunkCoord, ChunkCoordHash> protectedSet{a, b, c};

    CHECK(cache.evict(protectedSet).empty());
    CHECK(cache.evict(protectedSet).empty());
    CHECK_EQ(cache.size(), static_cast<size_t>(3));

    cache.touch(d);
    auto evicted = cache.evict(protectedSet);
    CHECK_EQ(evicted.size(), static_cast<size_t>(1));
    CHECK_EQ(evicted[0], d);
    CHECK_EQ(cache.size(), static_cast<size_t>(3));
}

TEST_CASE(ChunkCache_EvictionScanAdvancesWithinInspectionBudget) {
    ChunkCache cache;
    cache.setMaxChunks(1);
    for (int x = 0; x < 5; ++x) {
        cache.touch(ChunkCoord{x, 0, 0});
    }

    std::unordered_set<ChunkCoord, ChunkCoordHash> protectedSet;
    size_t failedAttempts = 0;
    auto rejectEviction = [&](ChunkCoord) {
        ++failedAttempts;
        return false;
    };

    CHECK(cache.evict(protectedSet, rejectEviction, 2).empty());
    CHECK_EQ(cache.lastEvictionInspections(), static_cast<size_t>(2));

    CHECK(cache.evict(protectedSet, rejectEviction, 2).empty());
    CHECK_EQ(cache.lastEvictionInspections(), static_cast<size_t>(2));

    CHECK(cache.evict(protectedSet, rejectEviction, 2).empty());
    CHECK_EQ(cache.lastEvictionInspections(), static_cast<size_t>(1));
    CHECK_EQ(failedAttempts, static_cast<size_t>(5));
    CHECK_EQ(cache.size(), static_cast<size_t>(5));
}

TEST_CASE(ChunkCache_RepeatedEvictionRequestsCoalesceOneFollowUpPass) {
    ChunkCache cache;
    for (int x = 0; x < 5; ++x) {
        cache.touch(ChunkCoord{x, 0, 0});
    }
    cache.setMaxChunks(1);

    std::unordered_set<ChunkCoord, ChunkCoordHash> protectedSet;
    size_t failedAttempts = 0;
    auto rejectEviction = [&](ChunkCoord) {
        ++failedAttempts;
        return false;
    };

    CHECK(cache.evict(protectedSet, rejectEviction, 1).empty());
    cache.setMaxChunks(2);
    cache.setMaxChunks(3);
    cache.setMaxChunks(1);

    CHECK(cache.evict(protectedSet, rejectEviction).empty());
    CHECK_EQ(cache.lastEvictionInspections(), static_cast<size_t>(9));
    CHECK_EQ(failedAttempts, static_cast<size_t>(10));
}
