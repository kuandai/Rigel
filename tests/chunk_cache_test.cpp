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
