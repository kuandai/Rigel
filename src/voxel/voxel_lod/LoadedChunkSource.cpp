#include "Rigel/Voxel/VoxelLod/LoadedChunkSource.h"

#include "Rigel/Voxel/ChunkCoord.h"

#include <algorithm>

namespace Rigel::Voxel {
namespace {

size_t brickIndex(int x, int y, int z, const glm::ivec3& dims) {
    return static_cast<size_t>(x)
        + static_cast<size_t>(y) * static_cast<size_t>(dims.x)
        + static_cast<size_t>(z) * static_cast<size_t>(dims.x) * static_cast<size_t>(dims.y);
}

} // namespace

const LoadedChunkSource::ChunkSnapshot* LoadedChunkSource::findSnapshot(ChunkCoord coord) const {
    for (const ChunkSnapshot& snap : m_snapshots) {
        if (snap.coord == coord) {
            return &snap;
        }
    }
    return nullptr;
}

std::vector<LoadedChunkSource::ChunkSnapshot> LoadedChunkSource::snapshotForBrick(
    const ChunkManager& chunks,
    const BrickSampleDesc& desc) {
    std::vector<ChunkSnapshot> out;
    if (!desc.isValid()) {
        return out;
    }

    const glm::ivec3 outDims = desc.outDims();
    if (outDims.x <= 0 || outDims.y <= 0 || outDims.z <= 0) {
        return out;
    }

    const glm::ivec3 maxWorld = desc.worldMinVoxel +
        (outDims - glm::ivec3(1)) * desc.stepVoxels;

    ChunkCoord minChunk = worldToChunk(desc.worldMinVoxel.x,
                                      desc.worldMinVoxel.y,
                                      desc.worldMinVoxel.z);
    ChunkCoord maxChunk = worldToChunk(maxWorld.x, maxWorld.y, maxWorld.z);

    const int cx0 = std::min(minChunk.x, maxChunk.x);
    const int cy0 = std::min(minChunk.y, maxChunk.y);
    const int cz0 = std::min(minChunk.z, maxChunk.z);
    const int cx1 = std::max(minChunk.x, maxChunk.x);
    const int cy1 = std::max(minChunk.y, maxChunk.y);
    const int cz1 = std::max(minChunk.z, maxChunk.z);

    const size_t estimate = static_cast<size_t>(cx1 - cx0 + 1) *
        static_cast<size_t>(cy1 - cy0 + 1) *
        static_cast<size_t>(cz1 - cz0 + 1);
    out.reserve(estimate);

    for (int cz = cz0; cz <= cz1; ++cz) {
        for (int cy = cy0; cy <= cy1; ++cy) {
            for (int cx = cx0; cx <= cx1; ++cx) {
                ChunkCoord coord{cx, cy, cz};
                const Chunk* chunk = chunks.getChunk(coord);
                if (!chunk) {
                    continue;
                }

                ChunkSnapshot snap;
                snap.coord = coord;
                chunk->copyBlocks(snap.blocks);
                out.push_back(std::move(snap));
            }
        }
    }

    return out;
}

BrickSampleStatus LoadedChunkSource::sampleBrick(const BrickSampleDesc& desc,
                                                 std::span<VoxelId> out,
                                                 const std::atomic_bool* cancel) const {
    if (cancel && cancel->load(std::memory_order_relaxed)) {
        return BrickSampleStatus::Cancelled;
    }
    if (!desc.isValid()) {
        return BrickSampleStatus::Miss;
    }

    const glm::ivec3 dims = desc.outDims();
    const size_t expected = desc.outVoxelCount();
    if (expected == 0 || out.size() != expected) {
        return BrickSampleStatus::Miss;
    }

    const glm::ivec3 maxWorld = desc.worldMinVoxel +
        (dims - glm::ivec3(1)) * desc.stepVoxels;

    ChunkCoord minChunk = worldToChunk(desc.worldMinVoxel.x,
                                      desc.worldMinVoxel.y,
                                      desc.worldMinVoxel.z);
    ChunkCoord maxChunk = worldToChunk(maxWorld.x, maxWorld.y, maxWorld.z);

    const int cx0 = std::min(minChunk.x, maxChunk.x);
    const int cy0 = std::min(minChunk.y, maxChunk.y);
    const int cz0 = std::min(minChunk.z, maxChunk.z);
    const int cx1 = std::max(minChunk.x, maxChunk.x);
    const int cy1 = std::max(minChunk.y, maxChunk.y);
    const int cz1 = std::max(minChunk.z, maxChunk.z);

    for (int cz = cz0; cz <= cz1; ++cz) {
        for (int cy = cy0; cy <= cy1; ++cy) {
            for (int cx = cx0; cx <= cx1; ++cx) {
                if (!findSnapshot(ChunkCoord{cx, cy, cz})) {
                    return BrickSampleStatus::Miss;
                }
            }
        }
    }

    for (int z = 0; z < dims.z; ++z) {
        if (cancel && cancel->load(std::memory_order_relaxed)) {
            return BrickSampleStatus::Cancelled;
        }
        const int wz = desc.worldMinVoxel.z + z * desc.stepVoxels;
        for (int y = 0; y < dims.y; ++y) {
            if (cancel && cancel->load(std::memory_order_relaxed)) {
                return BrickSampleStatus::Cancelled;
            }
            const int wy = desc.worldMinVoxel.y + y * desc.stepVoxels;
            for (int x = 0; x < dims.x; ++x) {
                const int wx = desc.worldMinVoxel.x + x * desc.stepVoxels;
                const ChunkCoord chunkCoord = worldToChunk(wx, wy, wz);
                const ChunkSnapshot* snap = findSnapshot(chunkCoord);
                if (!snap) {
                    return BrickSampleStatus::Miss;
                }
                int lx = 0;
                int ly = 0;
                int lz = 0;
                worldToLocal(wx, wy, wz, lx, ly, lz);
                const size_t chunkIndex = static_cast<size_t>(lx)
                    + static_cast<size_t>(ly) * static_cast<size_t>(Chunk::SIZE)
                    + static_cast<size_t>(lz) * static_cast<size_t>(Chunk::SIZE) *
                    static_cast<size_t>(Chunk::SIZE);
                out[brickIndex(x, y, z, dims)] = toVoxelId(snap->blocks[chunkIndex].id);
            }
        }
    }

    return BrickSampleStatus::Hit;
}

} // namespace Rigel::Voxel

