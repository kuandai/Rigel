#include "Rigel/Voxel/MeshBuilder.h"

#include <array>

namespace Rigel::Voxel {

namespace {

// Face vertex data for a unit cube
// Each face has 4 vertices in counter-clockwise order when viewed from outside
// Position offsets [vertex][xyz], UV coords [vertex][uv]

// Position offsets for each face (indexed by Direction, then vertex 0-3)
constexpr float FACE_POSITIONS[6][4][3] = {
    // PosX (+X, East)
    {{1, 0, 1}, {1, 1, 1}, {1, 1, 0}, {1, 0, 0}},
    // NegX (-X, West)
    {{0, 0, 0}, {0, 1, 0}, {0, 1, 1}, {0, 0, 1}},
    // PosY (+Y, Up)
    {{0, 1, 0}, {1, 1, 0}, {1, 1, 1}, {0, 1, 1}},
    // NegY (-Y, Down)
    {{0, 0, 1}, {1, 0, 1}, {1, 0, 0}, {0, 0, 0}},
    // PosZ (+Z, South)
    {{0, 0, 1}, {0, 1, 1}, {1, 1, 1}, {1, 0, 1}},
    // NegZ (-Z, North)
    {{1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0, 0, 0}}
};

// UV coordinates for each face (same for all faces currently)
constexpr float FACE_UVS[4][2] = {
    {0, 0}, {0, 1}, {1, 1}, {1, 0}
};

// Quad indices (two triangles per face)
constexpr std::array<uint32_t, 6> QUAD_INDICES = {0, 1, 2, 0, 2, 3};

} // anonymous namespace

ChunkMesh MeshBuilder::build(const BuildContext& ctx) const {
    ChunkMesh mesh;

    // Skip empty chunks
    if (ctx.chunk.isEmpty()) {
        return mesh;
    }

    // Reserve estimated capacity
    // Rough estimate: average visible blocks * faces * vertices
    const size_t estimatedBlocks = ctx.chunk.nonAirCount();
    mesh.vertices.reserve(estimatedBlocks * 6);  // ~1 face per block average
    mesh.indices.reserve(estimatedBlocks * 6);

    // Temporary storage for each layer
    std::array<std::vector<VoxelVertex>, RenderLayerCount> layerVertices;
    std::array<std::vector<uint32_t>, RenderLayerCount> layerIndices;

    // Iterate all blocks
    const auto& blocks = ctx.chunk.blocks();
    for (int z = 0; z < Chunk::SIZE; z++) {
        for (int y = 0; y < Chunk::SIZE; y++) {
            for (int x = 0; x < Chunk::SIZE; x++) {
                BlockState state = blocks[x + y * Chunk::SIZE + z * Chunk::SIZE * Chunk::SIZE];

                // Skip air
                if (state.isAir()) {
                    continue;
                }

                const BlockType& type = ctx.registry.getType(state.id);

                // Skip non-cube models for now (would need model system)
                if (type.model != "cube") {
                    continue;
                }

                // Get layer for this block type
                size_t layerIdx = static_cast<size_t>(type.layer);

                // Append faces for this block
                for (size_t faceIdx = 0; faceIdx < DirectionCount; faceIdx++) {
                    Direction face = static_cast<Direction>(faceIdx);

                    if (!shouldRenderFace(ctx, x, y, z, face, state, type)) {
                        continue;
                    }

                    // Add vertices for this face
                    uint32_t baseVertex = static_cast<uint32_t>(layerVertices[layerIdx].size());

                    // Look up texture layer for this face
                    uint16_t textureLayer = 0;
                    if (ctx.atlas) {
                        const std::string& texturePath = type.textures.forFace(face);
                        if (!texturePath.empty()) {
                            TextureHandle handle = ctx.atlas->findTexture(texturePath);
                            if (handle.isValid()) {
                                textureLayer = static_cast<uint16_t>(ctx.atlas->getLayer(handle));
                            }
                        }
                    }

                    for (size_t v = 0; v < 4; v++) {
                        VoxelVertex vertex;
                        vertex.x = static_cast<float>(x) + FACE_POSITIONS[faceIdx][v][0];
                        vertex.y = static_cast<float>(y) + FACE_POSITIONS[faceIdx][v][1];
                        vertex.z = static_cast<float>(z) + FACE_POSITIONS[faceIdx][v][2];
                        vertex.u = FACE_UVS[v][0];
                        vertex.v = FACE_UVS[v][1];
                        vertex.normalIndex = static_cast<uint8_t>(faceIdx);
                        vertex.aoLevel = calculateAO(ctx, x, y, z, face, static_cast<int>(v));
                        vertex.textureLayer = textureLayer;
                        vertex.flags = 0;

                        layerVertices[layerIdx].push_back(vertex);
                    }

                    // Add indices for this face (two triangles)
                    for (uint32_t idx : QUAD_INDICES) {
                        layerIndices[layerIdx].push_back(baseVertex + idx);
                    }
                }
            }
        }
    }

    // Combine layers into final mesh
    uint32_t vertexOffset = 0;
    uint32_t indexOffset = 0;

    for (size_t layer = 0; layer < RenderLayerCount; layer++) {
        mesh.layers[layer].indexStart = indexOffset;
        mesh.layers[layer].indexCount = static_cast<uint32_t>(layerIndices[layer].size());

        // Append vertices
        for (const auto& v : layerVertices[layer]) {
            mesh.vertices.push_back(v);
        }

        // Append indices (adjusted by vertex offset)
        for (uint32_t idx : layerIndices[layer]) {
            mesh.indices.push_back(idx + vertexOffset);
        }

        vertexOffset += static_cast<uint32_t>(layerVertices[layer].size());
        indexOffset += static_cast<uint32_t>(layerIndices[layer].size());
    }

    return mesh;
}

bool MeshBuilder::shouldRenderFace(
    const BuildContext& ctx,
    int x, int y, int z,
    Direction face,
    const BlockState& state,
    const BlockType& type
) const {
    // Get offset for this direction
    int dx, dy, dz;
    directionOffset(face, dx, dy, dz);

    // Get neighbor block
    BlockState neighbor = getBlockAt(ctx, x + dx, y + dy, z + dz);

    // Render face if neighbor is air
    if (neighbor.isAir()) {
        return true;
    }

    const BlockType& neighborType = ctx.registry.getType(neighbor.id);
    if (neighborType.isOpaque) {
        return false;
    }

    if (type.cullSameType && neighbor.id == state.id) {
        return false;
    }

    return true;
}

BlockState MeshBuilder::getBlockAt(
    const BuildContext& ctx,
    int x, int y, int z
) const {
    // Inside current chunk
    if (x >= 0 && x < Chunk::SIZE &&
        y >= 0 && y < Chunk::SIZE &&
        z >= 0 && z < Chunk::SIZE) {
        return ctx.chunk.getBlock(x, y, z);
    }

    // Determine which neighbor chunk and local coordinates
    Direction dir;
    int nx = x, ny = y, nz = z;

    if (x < 0) {
        dir = Direction::NegX;
        nx = x + Chunk::SIZE;
    } else if (x >= Chunk::SIZE) {
        dir = Direction::PosX;
        nx = x - Chunk::SIZE;
    } else if (y < 0) {
        dir = Direction::NegY;
        ny = y + Chunk::SIZE;
    } else if (y >= Chunk::SIZE) {
        dir = Direction::PosY;
        ny = y - Chunk::SIZE;
    } else if (z < 0) {
        dir = Direction::NegZ;
        nz = z + Chunk::SIZE;
    } else {
        dir = Direction::PosZ;
        nz = z - Chunk::SIZE;
    }

    const Chunk* neighbor = ctx.neighbors[static_cast<size_t>(dir)];
    if (!neighbor) {
        // Treat unloaded chunks as opaque (don't render faces at world edge)
        return BlockState{};  // Air, will cause face to render
    }

    return neighbor->getBlock(nx, ny, nz);
}

uint8_t MeshBuilder::calculateAO(
    const BuildContext& ctx,
    int x, int y, int z,
    Direction face,
    int corner
) const {
    // Simple AO: check 3 adjacent blocks for each corner
    // For now, return full brightness (3)
    // TODO: Implement proper AO calculation

    (void)ctx;
    (void)x;
    (void)y;
    (void)z;
    (void)face;
    (void)corner;

    return 3;  // No occlusion
}

} // namespace Rigel::Voxel
