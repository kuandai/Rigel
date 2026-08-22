#include "TestFramework.h"

#include "Rigel/Voxel/ChunkCoord.h"

#include <limits>

using namespace Rigel::Voxel;

TEST_CASE(ChunkCoord_WorldToChunkPositive) {
    CHECK_EQ(worldToChunk(0, 0, 0).x, 0);
    CHECK_EQ(worldToChunk(31, 0, 0).x, 0);
    CHECK_EQ(worldToChunk(32, 0, 0).x, 1);
}

TEST_CASE(ChunkCoord_WorldToChunkNegative) {
    CHECK_EQ(worldToChunk(-1, 0, 0).x, -1);
    CHECK_EQ(worldToChunk(-32, 0, 0).x, -1);
    CHECK_EQ(worldToChunk(-33, 0, 0).x, -2);
}

TEST_CASE(ChunkCoord_WorldToChunkMinimumCoordinate) {
    constexpr int minimum = std::numeric_limits<int>::min();
    CHECK_EQ(worldToChunk(minimum, 0, 0).x, minimum / ChunkSize);
    CHECK_EQ(worldToChunk(minimum + 1, 0, 0).x, minimum / ChunkSize);
}

TEST_CASE(ChunkCoord_LocalConversions) {
    int lx = 0;
    int ly = 0;
    int lz = 0;
    worldToLocal(-1, 0, 0, lx, ly, lz);
    CHECK_EQ(lx, ChunkSize - 1);
    CHECK_EQ(ly, 0);
    CHECK_EQ(lz, 0);

    int wx = 0;
    int wy = 0;
    int wz = 0;
    localToWorld({-1, 0, 0}, lx, ly, lz, wx, wy, wz);
    CHECK_EQ(wx, -1);
    CHECK_EQ(wy, 0);
    CHECK_EQ(wz, 0);
}
