#include "TestFramework.h"

#include "Rigel/Voxel/VoxelLod/VoxelSource.h"

namespace Rigel::Voxel {
namespace {

TEST_CASE(VoxelSource_BrickSampleDesc_ValidatesAndComputesOutputDims) {
    BrickSampleDesc desc;
    desc.worldMinVoxel = {0, 0, 0};
    desc.brickDimsVoxels = {64, 32, 16};
    desc.stepVoxels = 2;

    CHECK(desc.isValid());
    CHECK_EQ(desc.outDims(), glm::ivec3(32, 16, 8));
    CHECK_EQ(desc.outVoxelCount(), static_cast<size_t>(32 * 16 * 8));
}

TEST_CASE(VoxelSource_BrickSampleDesc_InvalidDimsOrStepReturnZeroOutput) {
    BrickSampleDesc desc;
    desc.worldMinVoxel = {0, 0, 0};
    desc.brickDimsVoxels = {0, 32, 32};
    desc.stepVoxels = 1;

    CHECK(!desc.isValid());
    CHECK_EQ(desc.outDims(), glm::ivec3(0));
    CHECK_EQ(desc.outVoxelCount(), static_cast<size_t>(0));

    desc.brickDimsVoxels = {32, 32, 32};
    desc.stepVoxels = 0;
    CHECK(!desc.isValid());
    CHECK_EQ(desc.outDims(), glm::ivec3(0));
    CHECK_EQ(desc.outVoxelCount(), static_cast<size_t>(0));

    desc.brickDimsVoxels = {33, 32, 32};
    desc.stepVoxels = 2;
    CHECK(!desc.isValid());
    CHECK_EQ(desc.outDims(), glm::ivec3(0));
    CHECK_EQ(desc.outVoxelCount(), static_cast<size_t>(0));
}

} // namespace
} // namespace Rigel::Voxel

