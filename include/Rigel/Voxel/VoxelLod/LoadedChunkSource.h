#pragma once

#include "Rigel/Voxel/Chunk.h"
#include "Rigel/Voxel/ChunkCoord.h"
#include "Rigel/Voxel/ChunkManager.h"
#include "Rigel/Voxel/VoxelLod/VoxelSource.h"

#include <array>
#include <span>
#include <vector>

namespace Rigel::Voxel {

// Worker-safe voxel source backed by immutable snapshots of resident chunks.
//
// IMPORTANT:
// - This source does not touch ChunkManager/Chunk at sample time.
// - Snapshot creation must occur on the main thread (ChunkManager is not thread-safe).
class LoadedChunkSource final : public IVoxelSource {
public:
    struct ChunkSnapshot {
        ChunkCoord coord;
        std::array<BlockState, Chunk::VOLUME> blocks{};
    };

    // Collect snapshots for all chunks intersecting the sampled brick.
    //
    // This function reads live Chunk instances via ChunkManager and must be called
    // with external synchronization (typically the main thread).
    static std::vector<ChunkSnapshot> snapshotForBrick(const ChunkManager& chunks,
                                                       const BrickSampleDesc& desc);

    explicit LoadedChunkSource(std::vector<ChunkSnapshot> snapshots)
        : m_snapshots(std::move(snapshots)) {}

    BrickSampleStatus sampleBrick(const BrickSampleDesc& desc,
                                  std::span<VoxelId> out,
                                  const std::atomic_bool* cancel = nullptr) const override;

private:
    const ChunkSnapshot* findSnapshot(ChunkCoord coord) const;

    std::vector<ChunkSnapshot> m_snapshots;
};

} // namespace Rigel::Voxel

