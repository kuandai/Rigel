#include "TestFramework.h"

#include "Rigel/Voxel/VoxelLod/VoxelSurfaceExtraction.h"

#include <array>
#include <cstddef>
#include <vector>

namespace Rigel::Voxel {
namespace {

static size_t countNormal(const std::vector<SurfaceQuad>& quads, Direction n) {
    size_t count = 0;
    for (const SurfaceQuad& q : quads) {
        if (q.normal == n) {
            ++count;
        }
    }
    return count;
}

TEST_CASE(VoxelSurfaceExtraction_Neighbors_AllSolidSuppressesAllFaces) {
    MacroVoxelGrid center;
    center.dims = glm::ivec3(1, 1, 1);
    center.cellSizeVoxels = 4;
    center.cells = {static_cast<VoxelId>(1)};

    MacroVoxelGrid solid;
    solid.dims = center.dims;
    solid.cellSizeVoxels = center.cellSizeVoxels;
    solid.cells = {static_cast<VoxelId>(1)};

    MacroVoxelNeighbors neighbors;
    neighbors.negX = &solid;
    neighbors.posX = &solid;
    neighbors.negY = &solid;
    neighbors.posY = &solid;
    neighbors.negZ = &solid;
    neighbors.posZ = &solid;

    std::vector<SurfaceQuad> quads;
    extractSurfaceQuadsGreedy(center, neighbors, VoxelBoundaryPolicy::OutsideEmpty, quads);
    CHECK_EQ(quads.size(), static_cast<size_t>(0));
}

TEST_CASE(VoxelSurfaceExtraction_Neighbors_PosXSolidSuppressesOnlyPosXFace) {
    MacroVoxelGrid center;
    center.dims = glm::ivec3(1, 1, 1);
    center.cellSizeVoxels = 4;
    center.cells = {static_cast<VoxelId>(1)};

    MacroVoxelGrid solid;
    solid.dims = center.dims;
    solid.cellSizeVoxels = center.cellSizeVoxels;
    solid.cells = {static_cast<VoxelId>(1)};

    MacroVoxelNeighbors neighbors;
    neighbors.posX = &solid;

    std::vector<SurfaceQuad> quads;
    extractSurfaceQuadsGreedy(center, neighbors, VoxelBoundaryPolicy::OutsideEmpty, quads);
    CHECK_EQ(quads.size(), static_cast<size_t>(5));

    CHECK_EQ(countNormal(quads, Direction::PosX), static_cast<size_t>(0));
    CHECK_EQ(countNormal(quads, Direction::NegX), static_cast<size_t>(1));
    CHECK_EQ(countNormal(quads, Direction::PosY), static_cast<size_t>(1));
    CHECK_EQ(countNormal(quads, Direction::NegY), static_cast<size_t>(1));
    CHECK_EQ(countNormal(quads, Direction::PosZ), static_cast<size_t>(1));
    CHECK_EQ(countNormal(quads, Direction::NegZ), static_cast<size_t>(1));
}

} // namespace
} // namespace Rigel::Voxel

