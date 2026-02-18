#include "TestFramework.h"

#include "Rigel/Voxel/VoxelLod/GeneratorSource.h"

#include <cstdint>
#include <random>

namespace Rigel::Voxel {
namespace {

uint16_t coordHashId(int wx, int wy, int wz, uint32_t seed) {
    uint32_t h = seed;
    auto mix = [&h](uint32_t v) {
        h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
    };
    mix(static_cast<uint32_t>(wx) * 73856093u);
    mix(static_cast<uint32_t>(wy) * 19349663u);
    mix(static_cast<uint32_t>(wz) * 83492791u);
    // Keep IDs small; include air sometimes.
    return static_cast<uint16_t>(h % 8u);
}

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

TEST_CASE(VoxelGeneratorSource_DeterministicPatternMatchesReference) {
    constexpr uint32_t seed = 42;
    GeneratorSource source([&](ChunkCoord coord,
                               std::array<BlockState, Chunk::VOLUME>& out,
                               const std::atomic_bool* cancel) {
        (void)cancel;
        for (int z = 0; z < Chunk::SIZE; ++z) {
            for (int y = 0; y < Chunk::SIZE; ++y) {
                for (int x = 0; x < Chunk::SIZE; ++x) {
                    int wx = coord.x * Chunk::SIZE + x;
                    int wy = coord.y * Chunk::SIZE + y;
                    int wz = coord.z * Chunk::SIZE + z;
                    out[static_cast<size_t>(x) +
                        static_cast<size_t>(y) * Chunk::SIZE +
                        static_cast<size_t>(z) * Chunk::SIZE * Chunk::SIZE] =
                        makeBlock(coordHashId(wx, wy, wz, seed));
                }
            }
        }
    });

    BrickSampleDesc desc;
    desc.worldMinVoxel = {-10, 5, 7};
    desc.brickDimsVoxels = {64, 32, 32};
    desc.stepVoxels = 1;
    CHECK(desc.isValid());

    std::vector<VoxelId> out(desc.outVoxelCount());
    CHECK_EQ(source.sampleBrick(desc, out), BrickSampleStatus::Hit);

    const glm::ivec3 dims = desc.outDims();
    for (int z = 0; z < dims.z; ++z) {
        for (int y = 0; y < dims.y; ++y) {
            for (int x = 0; x < dims.x; ++x) {
                int wx = desc.worldMinVoxel.x + x;
                int wy = desc.worldMinVoxel.y + y;
                int wz = desc.worldMinVoxel.z + z;
                CHECK_EQ(out[brickIndex(x, y, z, dims)], coordHashId(wx, wy, wz, seed));
            }
        }
    }
}

TEST_CASE(VoxelGeneratorSource_RandomLookingPatternMatchesReference) {
    constexpr uint32_t seed = 1337;
    GeneratorSource source([&](ChunkCoord coord,
                               std::array<BlockState, Chunk::VOLUME>& out,
                               const std::atomic_bool* cancel) {
        (void)cancel;
        for (int z = 0; z < Chunk::SIZE; ++z) {
            for (int y = 0; y < Chunk::SIZE; ++y) {
                for (int x = 0; x < Chunk::SIZE; ++x) {
                    int wx = coord.x * Chunk::SIZE + x;
                    int wy = coord.y * Chunk::SIZE + y;
                    int wz = coord.z * Chunk::SIZE + z;
                    out[static_cast<size_t>(x) +
                        static_cast<size_t>(y) * Chunk::SIZE +
                        static_cast<size_t>(z) * Chunk::SIZE * Chunk::SIZE] =
                        makeBlock(coordHashId(wx, wy, wz, seed));
                }
            }
        }
    });

    BrickSampleDesc desc;
    desc.worldMinVoxel = {0, 0, 0};
    desc.brickDimsVoxels = {64, 64, 64};
    desc.stepVoxels = 2;
    CHECK(desc.isValid());

    std::vector<VoxelId> out(desc.outVoxelCount());
    CHECK_EQ(source.sampleBrick(desc, out), BrickSampleStatus::Hit);

    const glm::ivec3 dims = desc.outDims();
    for (int z = 0; z < dims.z; ++z) {
        for (int y = 0; y < dims.y; ++y) {
            for (int x = 0; x < dims.x; ++x) {
                int wx = desc.worldMinVoxel.x + x * desc.stepVoxels;
                int wy = desc.worldMinVoxel.y + y * desc.stepVoxels;
                int wz = desc.worldMinVoxel.z + z * desc.stepVoxels;
                CHECK_EQ(out[brickIndex(x, y, z, dims)], coordHashId(wx, wy, wz, seed));
            }
        }
    }
}

TEST_CASE(VoxelGeneratorSource_CancelledTokenReturnsCancelled) {
    std::atomic_bool cancelled(true);
    GeneratorSource source([&](ChunkCoord,
                               std::array<BlockState, Chunk::VOLUME>& out,
                               const std::atomic_bool*) {
        out.fill(makeBlock(1));
    });

    BrickSampleDesc desc;
    desc.worldMinVoxel = {0, 0, 0};
    desc.brickDimsVoxels = {32, 32, 32};
    desc.stepVoxels = 1;
    CHECK(desc.isValid());

    std::vector<VoxelId> out(desc.outVoxelCount());
    CHECK_EQ(source.sampleBrick(desc, out, &cancelled), BrickSampleStatus::Cancelled);
}

} // namespace
} // namespace Rigel::Voxel

