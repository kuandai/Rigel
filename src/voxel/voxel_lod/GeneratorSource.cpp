#include "Rigel/Voxel/VoxelLod/GeneratorSource.h"

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

const GeneratorSource::GeneratedChunk* GeneratorSource::findChunk(
    std::span<const GeneratedChunk> chunks,
    ChunkCoord coord) const {
    for (const GeneratedChunk& chunk : chunks) {
        if (chunk.coord == coord) {
            return &chunk;
        }
    }
    return nullptr;
}

BrickSampleStatus GeneratorSource::sampleBrick(const BrickSampleDesc& desc,
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
    if (!m_generator) {
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

    std::vector<GeneratedChunk> generated;
    const size_t estimate = static_cast<size_t>(cx1 - cx0 + 1) *
        static_cast<size_t>(cy1 - cy0 + 1) *
        static_cast<size_t>(cz1 - cz0 + 1);
    generated.reserve(estimate);

    for (int cz = cz0; cz <= cz1; ++cz) {
        for (int cy = cy0; cy <= cy1; ++cy) {
            for (int cx = cx0; cx <= cx1; ++cx) {
                if (cancel && cancel->load(std::memory_order_relaxed)) {
                    return BrickSampleStatus::Cancelled;
                }
                GeneratedChunk chunk;
                chunk.coord = ChunkCoord{cx, cy, cz};
                m_generator(chunk.coord, chunk.blocks, cancel);
                if (cancel && cancel->load(std::memory_order_relaxed)) {
                    return BrickSampleStatus::Cancelled;
                }
                generated.push_back(std::move(chunk));
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
                const GeneratedChunk* chunk = findChunk(generated, chunkCoord);
                if (!chunk) {
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
                out[brickIndex(x, y, z, dims)] = toVoxelId(chunk->blocks[chunkIndex].id);
            }
        }
    }

    return BrickSampleStatus::Hit;
}

} // namespace Rigel::Voxel

