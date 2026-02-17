#pragma once

#include "Rigel/Voxel/Block.h"
#include "Rigel/Voxel/Chunk.h"
#include "Rigel/Voxel/ChunkCoord.h"

#include <array>
#include <cstdint>
#include <vector>

namespace Rigel::Voxel {

struct LodCellKey {
    int level = 0;
    int x = 0;
    int y = 0;
    int z = 0;

    bool operator==(const LodCellKey& other) const {
        return level == other.level &&
               x == other.x &&
               y == other.y &&
               z == other.z;
    }

    bool operator!=(const LodCellKey& other) const {
        return !(*this == other);
    }
};

struct LodCellKeyHash {
    size_t operator()(const LodCellKey& key) const noexcept;
};

enum class LodCellState : uint8_t {
    Missing,
    QueuedBuild,
    Building,
    Ready,
    Stale,
    Evicting
};

struct LodChunkSnapshot {
    ChunkCoord coord;
    std::array<BlockState, Chunk::VOLUME> blocks{};
};

struct LodBuildInput {
    LodCellKey key;
    uint64_t revision = 0;
    int spanChunks = 1;
    std::vector<LodChunkSnapshot> chunks;
};

struct LodBuildOutput {
    LodCellKey key;
    uint64_t revision = 0;
    uint32_t sampledChunks = 0;
    uint64_t nonAirVoxelCount = 0;
    uint64_t opaqueVoxelCount = 0;
    uint64_t nonOpaqueVoxelCount = 0;
    bool empty = true;
};

LodCellKey chunkToLodCell(ChunkCoord coord, int spanChunks, int lodLevel = 0);
std::vector<LodCellKey> touchedLodCellsForChunk(ChunkCoord coord,
                                                 int spanChunks,
                                                 int lodLevel = 0);

} // namespace Rigel::Voxel
