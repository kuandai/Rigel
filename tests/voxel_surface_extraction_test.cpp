#include "TestFramework.h"

#include "Rigel/Voxel/VoxelLod/VoxelSurfaceExtraction.h"

#include <vector>

namespace Rigel::Voxel {
namespace {

TEST_CASE(VoxelSurfaceExtraction_SingleCubeEmitsSixQuads) {
    MacroVoxelGrid grid;
    grid.dims = glm::ivec3(1, 1, 1);
    grid.cellSizeVoxels = 4;
    grid.cells = {static_cast<VoxelId>(1)};

    std::vector<SurfaceQuad> quads;
    extractSurfaceQuads(grid, VoxelBoundaryPolicy::OutsideEmpty, quads);
    CHECK_EQ(quads.size(), static_cast<size_t>(6));
}

TEST_CASE(VoxelSurfaceExtraction_SolidVolumeOnlyEmitsOuterHullQuads) {
    MacroVoxelGrid grid;
    grid.dims = glm::ivec3(2, 2, 2);
    grid.cellSizeVoxels = 4;
    grid.cells.assign(static_cast<size_t>(2 * 2 * 2), static_cast<VoxelId>(1));

    std::vector<SurfaceQuad> quads;
    extractSurfaceQuads(grid, VoxelBoundaryPolicy::OutsideEmpty, quads);

    // 6 faces, each face has 2x2 macro cells -> 24 quads without greedy merge.
    CHECK_EQ(quads.size(), static_cast<size_t>(24));
}

TEST_CASE(VoxelSurfaceExtraction_InternalCavityProducesInternalSurfaces) {
    MacroVoxelGrid grid;
    grid.dims = glm::ivec3(3, 3, 3);
    grid.cellSizeVoxels = 4;
    grid.cells.assign(static_cast<size_t>(3 * 3 * 3), static_cast<VoxelId>(1));
    // Hollow out the center.
    grid.cells[static_cast<size_t>(1 + 1 * 3 + 1 * 3 * 3)] = kVoxelAir;

    std::vector<SurfaceQuad> quads;
    extractSurfaceQuads(grid, VoxelBoundaryPolicy::OutsideEmpty, quads);

    // Outer hull: 6 * (3x3) = 54 quads. Cavity: +6 quads (one for each neighbor).
    CHECK_EQ(quads.size(), static_cast<size_t>(60));
}

} // namespace
} // namespace Rigel::Voxel

