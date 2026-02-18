#include "TestFramework.h"

#include "Rigel/Voxel/VoxelLod/LoadedChunkSource.h"

#include <random>

namespace Rigel::Voxel {
namespace {

BlockState makeBlock(uint16_t type) {
    BlockState s;
    s.id = BlockID{type};
    return s;
}

size_t brickIndex(int x, int y, int z, const glm::ivec3& dims) {
    return static_cast<size_t>(x)
        + static_cast<size_t>(y) * static_cast<size_t>(dims.x)
        + static_cast<size_t>(z) * static_cast<size_t>(dims.x) * static_cast<size_t>(dims.y);
}

TEST_CASE(VoxelLoadedChunkSource_DeterministicBrickAcrossChunkBoundaryMatches) {
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

    std::vector<VoxelId> out(desc.outVoxelCount());
    LoadedChunkSource source(LoadedChunkSource::snapshotForBrick(manager, desc));
    CHECK_EQ(source.sampleBrick(desc, out), BrickSampleStatus::Hit);

    const glm::ivec3 dims = desc.outDims();
    for (int z = 0; z < dims.z; ++z) {
        for (int y = 0; y < dims.y; ++y) {
            for (int x = 0; x < dims.x; ++x) {
                const VoxelId expected = (x < Chunk::SIZE) ? 1u : 2u;
                CHECK_EQ(out[brickIndex(x, y, z, dims)], expected);
            }
        }
    }
}

TEST_CASE(VoxelLoadedChunkSource_MissingChunkReturnsMiss) {
    ChunkManager manager;
    Chunk& c0 = manager.getOrCreateChunk(ChunkCoord{0, 0, 0});
    c0.fill(makeBlock(1));

    BrickSampleDesc desc;
    desc.worldMinVoxel = {0, 0, 0};
    desc.brickDimsVoxels = {64, 32, 32};
    desc.stepVoxels = 1;
    CHECK(desc.isValid());

    std::vector<VoxelId> out(desc.outVoxelCount(), 123);
    std::vector<LoadedChunkSource::ChunkSnapshot> onlyOne;
    {
        LoadedChunkSource::ChunkSnapshot snap;
        snap.coord = ChunkCoord{0, 0, 0};
        c0.copyBlocks(snap.blocks);
        onlyOne.push_back(std::move(snap));
    }

    LoadedChunkSource source(std::move(onlyOne));
    CHECK_EQ(source.sampleBrick(desc, out), BrickSampleStatus::Miss);
}

TEST_CASE(VoxelLoadedChunkSource_RandomBrickMatchesReferenceSampling) {
    std::mt19937 rng(1337);
    std::uniform_int_distribution<int> dist(0, 5);

    ChunkManager manager;
    Chunk& c0 = manager.getOrCreateChunk(ChunkCoord{0, 0, 0});
    Chunk& c1 = manager.getOrCreateChunk(ChunkCoord{1, 0, 0});

    for (int z = 0; z < Chunk::SIZE; ++z) {
        for (int y = 0; y < Chunk::SIZE; ++y) {
            for (int x = 0; x < Chunk::SIZE; ++x) {
                c0.setBlock(x, y, z, makeBlock(static_cast<uint16_t>(dist(rng))));
                c1.setBlock(x, y, z, makeBlock(static_cast<uint16_t>(dist(rng))));
            }
        }
    }

    BrickSampleDesc desc;
    desc.worldMinVoxel = {0, 0, 0};
    desc.brickDimsVoxels = {64, 32, 32};
    desc.stepVoxels = 1;
    CHECK(desc.isValid());

    std::vector<VoxelId> out(desc.outVoxelCount());
    LoadedChunkSource source(LoadedChunkSource::snapshotForBrick(manager, desc));
    CHECK_EQ(source.sampleBrick(desc, out), BrickSampleStatus::Hit);

    const glm::ivec3 dims = desc.outDims();
    for (int z = 0; z < dims.z; ++z) {
        for (int y = 0; y < dims.y; ++y) {
            for (int x = 0; x < dims.x; ++x) {
                const int wx = desc.worldMinVoxel.x + x;
                const int wy = desc.worldMinVoxel.y + y;
                const int wz = desc.worldMinVoxel.z + z;
                const BlockState ref = manager.getBlock(wx, wy, wz);
                CHECK_EQ(out[brickIndex(x, y, z, dims)], toVoxelId(ref.id));
            }
        }
    }
}

} // namespace
} // namespace Rigel::Voxel

