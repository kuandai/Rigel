#pragma once

#include "Rigel/Voxel/Chunk.h"
#include "Rigel/Voxel/ChunkCoord.h"
#include "Rigel/Voxel/VoxelLod/VoxelSource.h"

#include <array>
#include <atomic>
#include <functional>
#include <span>
#include <vector>

namespace Rigel::Voxel {

// Worker-safe voxel source that synthesizes chunk data via a supplied generator callback.
//
// This is the MVP "worldgen fallback" source for the voxel SVO system. It may be
// replaced later with a more direct density-function sampler and/or caching.
class GeneratorSource final : public IVoxelSource {
public:
    using ChunkGenerateCallback = std::function<void(
        ChunkCoord coord,
        std::array<BlockState, Chunk::VOLUME>& outBlocks,
        const std::atomic_bool* cancel)>;

    explicit GeneratorSource(ChunkGenerateCallback generator)
        : m_generator(std::move(generator)) {}

    BrickSampleStatus sampleBrick(const BrickSampleDesc& desc,
                                  std::span<VoxelId> out,
                                  const std::atomic_bool* cancel = nullptr) const override;

private:
    struct GeneratedChunk {
        ChunkCoord coord;
        std::array<BlockState, Chunk::VOLUME> blocks{};
    };

    const GeneratedChunk* findChunk(std::span<const GeneratedChunk> chunks, ChunkCoord coord) const;

    ChunkGenerateCallback m_generator;
};

} // namespace Rigel::Voxel

