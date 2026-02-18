#include "TestFramework.h"

#include "Rigel/Voxel/VoxelLod/GeneratorSource.h"
#include "Rigel/Voxel/VoxelLod/LoadedChunkSource.h"
#include "Rigel/Voxel/VoxelLod/VoxelSourceChain.h"

namespace Rigel::Voxel {
namespace {

BlockState makeBlock(uint16_t type) {
    BlockState s;
    s.id = BlockID{type};
    return s;
}

TEST_CASE(VoxelSourceChain_PrefersLoadedSourceWhenHit) {
    ChunkManager manager;
    Chunk& c0 = manager.getOrCreateChunk(ChunkCoord{0, 0, 0});
    Chunk& c1 = manager.getOrCreateChunk(ChunkCoord{1, 0, 0});
    c0.fill(makeBlock(1));
    c1.fill(makeBlock(2));

    BrickSampleDesc desc;
    desc.worldMinVoxel = {0, 0, 0};
    desc.brickDimsVoxels = {64, 32, 32};
    desc.stepVoxels = 1;
    CHECK(desc.isValid());

    std::vector<VoxelId> out(desc.outVoxelCount(), 0);

    LoadedChunkSource loaded(LoadedChunkSource::snapshotForBrick(manager, desc));
    GeneratorSource generator([&](ChunkCoord,
                                  std::array<BlockState, Chunk::VOLUME>& outBlocks,
                                  const std::atomic_bool*) {
        outBlocks.fill(makeBlock(9));
    });

    VoxelSourceChain chain;
    chain.setLoaded(&loaded);
    chain.setGenerator(&generator);
    CHECK_EQ(chain.sampleBrick(desc, out), BrickSampleStatus::Hit);
    CHECK_EQ(chain.telemetry().loadedHits, static_cast<uint64_t>(1));
    CHECK_EQ(chain.telemetry().generatorHits, static_cast<uint64_t>(0));
}

TEST_CASE(VoxelSourceChain_FallsBackToGeneratorWhenLoadedMisses) {
    ChunkManager manager;
    Chunk& c0 = manager.getOrCreateChunk(ChunkCoord{0, 0, 0});
    c0.fill(makeBlock(1));

    BrickSampleDesc desc;
    desc.worldMinVoxel = {0, 0, 0};
    desc.brickDimsVoxels = {64, 32, 32};
    desc.stepVoxels = 1;
    CHECK(desc.isValid());

    std::vector<VoxelId> out(desc.outVoxelCount(), 0);

    // Only snapshot the first chunk; brick needs chunk (1,0,0) as well => miss.
    std::vector<LoadedChunkSource::ChunkSnapshot> snaps;
    {
        LoadedChunkSource::ChunkSnapshot snap;
        snap.coord = ChunkCoord{0, 0, 0};
        c0.copyBlocks(snap.blocks);
        snaps.push_back(std::move(snap));
    }
    LoadedChunkSource loaded(std::move(snaps));

    GeneratorSource generator([&](ChunkCoord,
                                  std::array<BlockState, Chunk::VOLUME>& outBlocks,
                                  const std::atomic_bool*) {
        outBlocks.fill(makeBlock(9));
    });

    VoxelSourceChain chain;
    chain.setLoaded(&loaded);
    chain.setGenerator(&generator);
    CHECK_EQ(chain.sampleBrick(desc, out), BrickSampleStatus::Hit);
    CHECK_EQ(chain.telemetry().loadedHits, static_cast<uint64_t>(0));
    CHECK_EQ(chain.telemetry().generatorHits, static_cast<uint64_t>(1));
}

TEST_CASE(VoxelSourceChain_CancelledTokenReturnsCancelled) {
    std::atomic_bool cancelled(true);

    BrickSampleDesc desc;
    desc.worldMinVoxel = {0, 0, 0};
    desc.brickDimsVoxels = {32, 32, 32};
    desc.stepVoxels = 1;
    CHECK(desc.isValid());

    std::vector<VoxelId> out(desc.outVoxelCount(), 0);
    VoxelSourceChain chain;
    CHECK_EQ(chain.sampleBrick(desc, out, &cancelled), BrickSampleStatus::Cancelled);
}

} // namespace
} // namespace Rigel::Voxel

