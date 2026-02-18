#include "Rigel/Voxel/VoxelLod/VoxelSurfaceMesher.h"

#include "Rigel/Voxel/VoxelVertex.h"

#include <algorithm>
#include <array>
#include <cstdint>

namespace Rigel::Voxel {
namespace {

// Position offsets for each face (indexed by Direction, then vertex 0-3).
// Matches MeshBuilder winding so chunk renderer culling behaves consistently.
constexpr float FACE_POSITIONS[6][4][3] = {
    // PosX
    {{1, 0, 1}, {1, 1, 1}, {1, 1, 0}, {1, 0, 0}},
    // NegX
    {{0, 0, 0}, {0, 1, 0}, {0, 1, 1}, {0, 0, 1}},
    // PosY
    {{0, 1, 0}, {1, 1, 0}, {1, 1, 1}, {0, 1, 1}},
    // NegY
    {{0, 0, 1}, {1, 0, 1}, {1, 0, 0}, {0, 0, 0}},
    // PosZ
    {{0, 0, 1}, {0, 1, 1}, {1, 1, 1}, {1, 0, 1}},
    // NegZ
    {{1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0, 0, 0}}
};

constexpr float FACE_UVS[4][2] = {
    {0, 0}, {0, 1}, {1, 1}, {1, 0}
};

constexpr std::array<uint32_t, 6> QUAD_INDICES = {0, 1, 2, 0, 2, 3};

glm::ivec3 quadExtents(Direction normal, const glm::ivec2& span, int cellSizeVoxels) {
    const int u = std::max(1, span.x) * cellSizeVoxels;
    const int v = std::max(1, span.y) * cellSizeVoxels;
    const int w = cellSizeVoxels;

    switch (normal) {
        case Direction::PosX:
        case Direction::NegX:
            // span = (z, y)
            return glm::ivec3(w, v, u);
        case Direction::PosY:
        case Direction::NegY:
            // span = (x, z)
            return glm::ivec3(u, w, v);
        case Direction::PosZ:
        case Direction::NegZ:
            // span = (x, y)
            return glm::ivec3(u, v, w);
    }
    return glm::ivec3(w);
}

uint8_t clampLayer(uint16_t layer) {
    return static_cast<uint8_t>(std::min<uint16_t>(layer, 255));
}

} // namespace

ChunkMesh buildSurfaceMeshFromQuads(
    std::span<const SurfaceQuad> quads,
    int cellSizeVoxels,
    std::span<const std::array<uint16_t, DirectionCount>> faceTextureLayersByVoxelId) {
    ChunkMesh mesh;
    if (quads.empty() || cellSizeVoxels <= 0) {
        return mesh;
    }

    mesh.vertices.reserve(quads.size() * 4);
    mesh.indices.reserve(quads.size() * 6);

    for (const SurfaceQuad& quad : quads) {
        if (quad.material == kVoxelAir) {
            continue;
        }

        const size_t faceIdx = static_cast<size_t>(quad.normal);
        const glm::ivec3 extent = quadExtents(quad.normal, quad.span, cellSizeVoxels);
        const glm::ivec3 base = quad.cellMin * cellSizeVoxels;

        uint16_t layer = 0;
        if (static_cast<size_t>(quad.material) < faceTextureLayersByVoxelId.size()) {
            layer = faceTextureLayersByVoxelId[static_cast<size_t>(quad.material)][faceIdx];
        }

        uint32_t baseVertex = static_cast<uint32_t>(mesh.vertices.size());
        for (size_t v = 0; v < 4; ++v) {
            VoxelVertex vert{};
            vert.x = static_cast<float>(base.x) + FACE_POSITIONS[faceIdx][v][0] * static_cast<float>(extent.x);
            vert.y = static_cast<float>(base.y) + FACE_POSITIONS[faceIdx][v][1] * static_cast<float>(extent.y);
            vert.z = static_cast<float>(base.z) + FACE_POSITIONS[faceIdx][v][2] * static_cast<float>(extent.z);
            vert.u = FACE_UVS[v][0];
            vert.v = FACE_UVS[v][1];
            vert.normalIndex = static_cast<uint8_t>(faceIdx);
            vert.aoLevel = 3;
            vert.textureLayer = clampLayer(layer);
            vert.flags = 0;
            mesh.vertices.push_back(vert);
        }

        for (uint32_t idx : QUAD_INDICES) {
            mesh.indices.push_back(baseVertex + idx);
        }
    }

    mesh.layers = {};
    mesh.layers[static_cast<size_t>(RenderLayer::Opaque)].indexStart = 0;
    mesh.layers[static_cast<size_t>(RenderLayer::Opaque)].indexCount =
        static_cast<uint32_t>(mesh.indices.size());
    return mesh;
}

} // namespace Rigel::Voxel

