#pragma once

#include "Rigel/Voxel/Block.h"
#include "Rigel/Voxel/Chunk.h"
#include "Rigel/Voxel/ChunkCoord.h"

#include <array>
#include <cstdint>
#include <vector>

namespace Rigel::Voxel {

class BlockRegistry;

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

enum class LodNodeKind : uint8_t {
    Empty = 0,
    Solid = 1,
    Mixed = 2
};

enum class LodMaterialClass : uint8_t {
    None = 0,
    Opaque = 1,
    NonOpaque = 2,
    Mixed = 3
};

struct LodSvoNode {
    static constexpr uint32_t INVALID_INDEX = 0xFFFFFFFFu;

    LodNodeKind kind = LodNodeKind::Empty;
    LodMaterialClass materialClass = LodMaterialClass::None;
    uint8_t childMask = 0;
    std::array<uint32_t, 8> children{
        INVALID_INDEX,
        INVALID_INDEX,
        INVALID_INDEX,
        INVALID_INDEX,
        INVALID_INDEX,
        INVALID_INDEX,
        INVALID_INDEX,
        INVALID_INDEX
    };
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
    std::vector<ChunkCoord> missingCoords;
};

struct LodBuildOutput {
    LodCellKey key;
    uint64_t revision = 0;
    uint32_t sampledChunks = 0;
    uint64_t nonAirVoxelCount = 0;
    uint64_t opaqueVoxelCount = 0;
    uint64_t nonOpaqueVoxelCount = 0;
    std::vector<LodSvoNode> nodes;
    uint32_t rootNode = LodSvoNode::INVALID_INDEX;
    uint32_t nodeCount = 0;
    uint32_t leafCount = 0;
    uint32_t mixedNodeCount = 0;
    bool empty = true;
};

LodCellKey chunkToLodCell(ChunkCoord coord, int spanChunks, int lodLevel = 0);
std::vector<LodCellKey> touchedLodCellsForChunk(ChunkCoord coord,
                                                 int spanChunks,
                                                 int lodLevel = 0);
LodBuildOutput buildLodBuildOutput(const LodBuildInput& input,
                                   const BlockRegistry* registry,
                                   int chunkSampleStep = 1);

} // namespace Rigel::Voxel
