#pragma once

/**
 * @file MeshBuilder.h
 * @brief Mesh generation for voxel chunks.
 *
 * MeshBuilder generates ChunkMesh data from block data, performing face
 * culling to eliminate hidden faces and reduce vertex count.
 */

#include "Block.h"
#include "Chunk.h"
#include "ChunkMesh.h"
#include "BlockRegistry.h"
#include "TextureAtlas.h"

#include <array>

namespace Rigel::Voxel {

/**
 * @brief Generates meshes from chunk block data.
 *
 * MeshBuilder performs:
 * - Face culling (hidden faces are not generated)
 * - Cross-chunk boundary checking
 * - Per-face texture coordinate assignment
 * - Ambient occlusion calculation (basic)
 *
 * @section usage Usage
 *
 * @code
 * MeshBuilder builder;
 *
 * MeshBuilder::BuildContext ctx{
 *     .chunk = myChunk,
 *     .registry = blockRegistry,
 *     .neighbors = {leftChunk, rightChunk, ...}  // May be nullptr
 * };
 *
 * ChunkMesh mesh = builder.build(ctx);
 * @endcode
 */
class MeshBuilder {
public:
    static constexpr int PaddedSize = Chunk::SIZE + 2;
    static constexpr int PaddedVolume = PaddedSize * PaddedSize * PaddedSize;

    /**
     * @brief Context for mesh building.
     */
    struct BuildContext {
        /// The chunk to build mesh for
        const Chunk& chunk;

        /// Block registry for type lookups
        const BlockRegistry& registry;

        /// Texture atlas for looking up texture layers
        const TextureAtlas* atlas = nullptr;

        /// Neighbor chunks for face culling at boundaries.
        /// Indexed by Direction enum. May be nullptr if neighbor not loaded.
        std::array<const Chunk*, DirectionCount> neighbors{};

        /// Optional padded block buffer (1-block border on all sides).
        /// When provided, AO and face culling sample from this buffer instead
        /// of crossing chunk boundaries directly.
        const std::array<BlockState, PaddedVolume>* paddedBlocks = nullptr;
    };

    /**
     * @brief Build mesh for a chunk.
     *
     * @param ctx Build context with chunk, registry, and neighbors
     * @return The generated mesh (CPU-side data)
     */
    ChunkMesh build(const BuildContext& ctx) const;

private:
    /**
     * @brief Check if a face should be rendered.
     *
     * A face is rendered if the neighboring block is air, or if the neighbor
     * is non-opaque and not a same-type cull target.
     *
     * @param ctx Build context
     * @param x Local X coordinate
     * @param y Local Y coordinate
     * @param z Local Z coordinate
     * @param face The face direction to check
     * @return True if face should be rendered
     */
    bool shouldRenderFace(
        const BuildContext& ctx,
        int x, int y, int z,
        Direction face,
        const BlockState& state,
        const BlockType& type
    ) const;

    /**
     * @brief Get block at position, handling chunk boundaries.
     *
     * @param ctx Build context
     * @param x Local X coordinate (may be outside 0-SIZE range)
     * @param y Local Y coordinate (may be outside 0-SIZE range)
     * @param z Local Z coordinate (may be outside 0-SIZE range)
     * @return Block state (air if outside loaded chunks)
     */
    BlockState getBlockAt(
        const BuildContext& ctx,
        int x, int y, int z
    ) const;

    /**
     * @brief Append cube faces for a block.
     *
     * Generates quads for each visible face of a cube-shaped block.
     *
     * @param ctx Build context
     * @param x Local X coordinate
     * @param y Local Y coordinate
     * @param z Local Z coordinate
     * @param type Block type for texture info
     * @param vertices Output vertex list
     * @param indices Output index list
     */
    void appendCubeFaces(
        const BuildContext& ctx,
        int x, int y, int z,
        const BlockType& type,
        std::vector<VoxelVertex>& vertices,
        std::vector<uint32_t>& indices
    ) const;

    /**
     * @brief Calculate ambient occlusion for a vertex.
     *
     * Uses the 3 adjacent blocks to determine AO level (0-3).
     *
     * @param ctx Build context
     * @param x Block X coordinate
     * @param y Block Y coordinate
     * @param z Block Z coordinate
     * @param face Face direction
     * @param corner Corner index (0-3)
     * @return AO level 0 (darkest) to 3 (brightest)
     */
    uint8_t calculateAO(
        const BuildContext& ctx,
        int x, int y, int z,
        Direction face,
        int corner
    ) const;
};

} // namespace Rigel::Voxel
