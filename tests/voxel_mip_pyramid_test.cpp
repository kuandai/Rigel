#include "TestFramework.h"

#include "Rigel/Voxel/VoxelLod/VoxelMipPyramid.h"

#include <random>

namespace Rigel::Voxel {
namespace {

size_t idx(int x, int y, int z, int dim) {
    return static_cast<size_t>(x)
        + static_cast<size_t>(y) * static_cast<size_t>(dim)
        + static_cast<size_t>(z) * static_cast<size_t>(dim) * static_cast<size_t>(dim);
}

TEST_CASE(VoxelMipPyramid_AllAirIsUniformEverywhere) {
    constexpr int dim = 8;
    std::vector<VoxelId> l0(static_cast<size_t>(dim) * dim * dim, kVoxelAir);

    VoxelMipPyramid pyramid = buildVoxelMipPyramid(l0, dim);
    CHECK(!pyramid.empty());
    CHECK_EQ(pyramid.baseDim, dim);
    CHECK_EQ(pyramid.levels.front().dim, dim);
    CHECK_EQ(pyramid.levels.back().dim, 1);

    for (const VoxelMipLevel& level : pyramid.levels) {
        for (uint32_t packed : level.cells) {
            CHECK(VoxelMipLevel::isUniform(packed));
            CHECK_EQ(VoxelMipLevel::value(packed), kVoxelAir);
        }
    }
}

TEST_CASE(VoxelMipPyramid_AllSolidIsUniformEverywhere) {
    constexpr int dim = 8;
    constexpr VoxelId solid = 7;
    std::vector<VoxelId> l0(static_cast<size_t>(dim) * dim * dim, solid);

    VoxelMipPyramid pyramid = buildVoxelMipPyramid(l0, dim);
    CHECK(!pyramid.empty());

    for (const VoxelMipLevel& level : pyramid.levels) {
        for (uint32_t packed : level.cells) {
            CHECK(VoxelMipLevel::isUniform(packed));
            CHECK_EQ(VoxelMipLevel::value(packed), solid);
        }
    }
}

TEST_CASE(VoxelMipPyramid_SingleVoxelChangeBreaksUniformAlongAncestry) {
    constexpr int dim = 8;
    constexpr VoxelId solid = 7;
    constexpr VoxelId other = 3;
    std::vector<VoxelId> l0(static_cast<size_t>(dim) * dim * dim, solid);
    l0[idx(0, 0, 0, dim)] = other;

    VoxelMipPyramid pyramid = buildVoxelMipPyramid(l0, dim);
    CHECK_EQ(pyramid.levels.size(), static_cast<size_t>(4)); // 8 -> 4 -> 2 -> 1

    // L0 is always uniform per cell.
    CHECK(VoxelMipLevel::isUniform(pyramid.levels[0].cells[idx(0, 0, 0, dim)]));

    // For each mip above L0, the (0,0,0) cell should be mixed (non-uniform), all others uniform.
    for (size_t levelIndex = 1; levelIndex < pyramid.levels.size(); ++levelIndex) {
        const VoxelMipLevel& level = pyramid.levels[levelIndex];
        for (int z = 0; z < level.dim; ++z) {
            for (int y = 0; y < level.dim; ++y) {
                for (int x = 0; x < level.dim; ++x) {
                    const uint32_t packed = level.cells[idx(x, y, z, level.dim)];
                    const bool shouldBeMixed = (x == 0 && y == 0 && z == 0);
                    CHECK_EQ(VoxelMipLevel::isUniform(packed), !shouldBeMixed);
                }
            }
        }
    }
}

TEST_CASE(VoxelMipPyramid_UniformImpliesAllChildrenUniformAndSameValue) {
    constexpr int dim = 16;
    std::mt19937 rng(2026);
    std::uniform_int_distribution<int> dist(0, 15);

    std::vector<VoxelId> l0(static_cast<size_t>(dim) * dim * dim);
    for (auto& v : l0) {
        v = static_cast<VoxelId>(dist(rng));
    }

    VoxelMipPyramid pyramid = buildVoxelMipPyramid(l0, dim);
    CHECK(pyramid.levelCount() >= 2);

    for (size_t levelIndex = 1; levelIndex < pyramid.levelCount(); ++levelIndex) {
        const VoxelMipLevel& parent = pyramid.levels[levelIndex];
        const VoxelMipLevel& child = pyramid.levels[levelIndex - 1];

        for (int z = 0; z < parent.dim; ++z) {
            for (int y = 0; y < parent.dim; ++y) {
                for (int x = 0; x < parent.dim; ++x) {
                    const uint32_t packed = parent.cells[idx(x, y, z, parent.dim)];
                    if (!VoxelMipLevel::isUniform(packed)) {
                        continue;
                    }
                    const VoxelId value = VoxelMipLevel::value(packed);

                    const int bx = x * 2;
                    const int by = y * 2;
                    const int bz = z * 2;
                    for (int oz = 0; oz < 2; ++oz) {
                        for (int oy = 0; oy < 2; ++oy) {
                            for (int ox = 0; ox < 2; ++ox) {
                                const uint32_t childPacked =
                                    child.cells[idx(bx + ox, by + oy, bz + oz, child.dim)];
                                CHECK(VoxelMipLevel::isUniform(childPacked));
                                CHECK_EQ(VoxelMipLevel::value(childPacked), value);
                            }
                        }
                    }
                }
            }
        }
    }
}

} // namespace
} // namespace Rigel::Voxel

