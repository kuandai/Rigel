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

static const SurfaceQuad* findNormal(const std::vector<SurfaceQuad>& quads, Direction n) {
    for (const SurfaceQuad& q : quads) {
        if (q.normal == n) {
            return &q;
        }
    }
    return nullptr;
}

TEST_CASE(VoxelSurfaceExtraction_GreedyMergesSolidVolumeToSixQuads) {
    MacroVoxelGrid grid;
    grid.dims = glm::ivec3(2, 2, 2);
    grid.cellSizeVoxels = 4;
    grid.cells.assign(static_cast<size_t>(2 * 2 * 2), static_cast<VoxelId>(1));

    std::vector<SurfaceQuad> quads;
    extractSurfaceQuadsGreedy(grid, VoxelBoundaryPolicy::OutsideEmpty, quads);
    CHECK_EQ(quads.size(), static_cast<size_t>(6));

    const std::array<Direction, 6> normals = {
        Direction::PosX,
        Direction::NegX,
        Direction::PosY,
        Direction::NegY,
        Direction::PosZ,
        Direction::NegZ,
    };
    for (Direction n : normals) {
        CHECK_EQ(countNormal(quads, n), static_cast<size_t>(1));
        const SurfaceQuad* q = findNormal(quads, n);
        CHECK(q != nullptr);
        CHECK_EQ(q->material, static_cast<VoxelId>(1));
        CHECK_EQ(q->span, glm::ivec2(2, 2));
    }
}

TEST_CASE(VoxelSurfaceExtraction_GreedyMergesFlatSlab) {
    MacroVoxelGrid grid;
    grid.dims = glm::ivec3(4, 1, 4);
    grid.cellSizeVoxels = 4;
    grid.cells.assign(static_cast<size_t>(4 * 1 * 4), static_cast<VoxelId>(1));

    std::vector<SurfaceQuad> quads;
    extractSurfaceQuadsGreedy(grid, VoxelBoundaryPolicy::OutsideEmpty, quads);
    CHECK_EQ(quads.size(), static_cast<size_t>(6));

    const SurfaceQuad* qPosY = findNormal(quads, Direction::PosY);
    const SurfaceQuad* qNegY = findNormal(quads, Direction::NegY);
    CHECK(qPosY != nullptr);
    CHECK(qNegY != nullptr);
    CHECK_EQ(qPosY->span, glm::ivec2(4, 4));
    CHECK_EQ(qNegY->span, glm::ivec2(4, 4));

    // Side faces (spans depend on our u/v convention per normal).
    const SurfaceQuad* qPosX = findNormal(quads, Direction::PosX);
    const SurfaceQuad* qNegX = findNormal(quads, Direction::NegX);
    const SurfaceQuad* qPosZ = findNormal(quads, Direction::PosZ);
    const SurfaceQuad* qNegZ = findNormal(quads, Direction::NegZ);
    CHECK(qPosX != nullptr);
    CHECK(qNegX != nullptr);
    CHECK(qPosZ != nullptr);
    CHECK(qNegZ != nullptr);
    CHECK_EQ(qPosX->span, glm::ivec2(4, 1));
    CHECK_EQ(qNegX->span, glm::ivec2(4, 1));
    CHECK_EQ(qPosZ->span, glm::ivec2(4, 1));
    CHECK_EQ(qNegZ->span, glm::ivec2(4, 1));
}

} // namespace
} // namespace Rigel::Voxel
