#include "TestFramework.h"

#include "Rigel/Voxel/VoxelLod/VoxelPageCpu.h"

namespace Rigel::Voxel {
namespace {

TEST_CASE(VoxelPageCpu_BuildsMipPyramidFromL0Brick) {
    constexpr int dim = 8;
    std::vector<VoxelId> l0(static_cast<size_t>(dim) * dim * dim);
    for (size_t i = 0; i < l0.size(); ++i) {
        l0[i] = static_cast<VoxelId>(i % 11u);
    }

    VoxelPageKey key;
    key.level = 2;
    key.x = -1;
    key.y = 3;
    key.z = 0;

    VoxelPageCpu page = buildVoxelPageCpu(key, l0, dim);
    CHECK_EQ(page.key, key);
    CHECK_EQ(page.dim, dim);
    CHECK_EQ(page.l0.size(), l0.size());
    CHECK(!page.mips.empty());
    CHECK_EQ(page.mips.baseDim, dim);
    CHECK_EQ(page.mips.levels.front().dim, dim);
    CHECK_EQ(page.mips.levels.back().dim, 1);

    // L0 mip should exactly mirror L0 brick values.
    const VoxelMipLevel& mip0 = page.mips.levels.front();
    CHECK_EQ(mip0.cells.size(), l0.size());
    for (size_t i = 0; i < l0.size(); ++i) {
        CHECK(VoxelMipLevel::isUniform(mip0.cells[i]));
        CHECK_EQ(VoxelMipLevel::value(mip0.cells[i]), l0[i]);
    }

    CHECK(page.cpuBytes() > 0);
}

TEST_CASE(VoxelPageCpu_InvalidInputsReturnEmpty) {
    std::vector<VoxelId> empty;
    VoxelPageKey key;
    CHECK(buildVoxelPageCpu(key, empty, 0).l0.empty());
    CHECK(buildVoxelPageCpu(key, empty, 8).l0.empty());

    std::vector<VoxelId> wrongSize(7);
    CHECK(buildVoxelPageCpu(key, wrongSize, 8).l0.empty());
}

} // namespace
} // namespace Rigel::Voxel

