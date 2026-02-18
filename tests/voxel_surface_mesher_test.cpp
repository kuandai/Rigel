#include "TestFramework.h"

#include "Rigel/Voxel/VoxelLod/VoxelSurfaceMesher.h"

#include <array>
#include <cstddef>
#include <vector>

namespace Rigel::Voxel {
namespace {

TEST_CASE(VoxelSurfaceMesher_BuildsQuadVerticesAndIndices) {
    SurfaceQuad quad;
    quad.normal = Direction::PosY;
    quad.cellMin = glm::ivec3(0, 0, 0);
    quad.span = glm::ivec2(2, 3); // (x,z) for PosY
    quad.material = static_cast<VoxelId>(1);

    std::vector<std::array<uint16_t, DirectionCount>> lut;
    lut.resize(2);
    lut[0].fill(0);
    lut[1] = {10, 11, 12, 13, 14, 15};

    ChunkMesh mesh = buildSurfaceMeshFromQuads({&quad, 1}, 4, lut);
    CHECK_EQ(mesh.vertices.size(), static_cast<size_t>(4));
    CHECK_EQ(mesh.indices.size(), static_cast<size_t>(6));
    CHECK_EQ(mesh.layers[static_cast<size_t>(RenderLayer::Opaque)].indexCount, static_cast<uint32_t>(6));

    // PosY quad spanning 2x3 macro cells at cellSize=4:
    // extent = (8,4,12), vertices use MeshBuilder's face order.
    const VoxelVertex& v0 = mesh.vertices[0];
    const VoxelVertex& v1 = mesh.vertices[1];
    const VoxelVertex& v2 = mesh.vertices[2];
    const VoxelVertex& v3 = mesh.vertices[3];

    CHECK_NEAR(v0.x, 0.0f, 0.0001f);
    CHECK_NEAR(v0.y, 4.0f, 0.0001f);
    CHECK_NEAR(v0.z, 0.0f, 0.0001f);

    CHECK_NEAR(v1.x, 8.0f, 0.0001f);
    CHECK_NEAR(v1.y, 4.0f, 0.0001f);
    CHECK_NEAR(v1.z, 0.0f, 0.0001f);

    CHECK_NEAR(v2.x, 8.0f, 0.0001f);
    CHECK_NEAR(v2.y, 4.0f, 0.0001f);
    CHECK_NEAR(v2.z, 12.0f, 0.0001f);

    CHECK_NEAR(v3.x, 0.0f, 0.0001f);
    CHECK_NEAR(v3.y, 4.0f, 0.0001f);
    CHECK_NEAR(v3.z, 12.0f, 0.0001f);

    for (const VoxelVertex& v : mesh.vertices) {
        CHECK_EQ(v.normalIndex, static_cast<uint8_t>(Direction::PosY));
        CHECK_EQ(v.aoLevel, static_cast<uint8_t>(3));
        CHECK_EQ(v.textureLayer, static_cast<uint8_t>(12));
    }
}

} // namespace
} // namespace Rigel::Voxel

