#pragma once

/**
 * @file VoxelVertex.h
 * @brief Vertex format for voxel meshes.
 *
 * Defines the vertex layout used for chunk meshes. The format is designed
 * to be compact while providing necessary data for rendering with textures,
 * lighting, and ambient occlusion.
 */

#include <cstdint>
#include <GL/glew.h>

namespace Rigel::Voxel {

/**
 * @brief Vertex format for voxel meshes.
 *
 * Total size: 24 bytes per vertex.
 *
 * Layout matches shader attributes:
 * - location 0: vec3 a_position (x, y, z)
 * - location 1: vec2 a_uv (u, v)
 * - location 2: vec4 a_packedData (normalIndex, aoLevel, textureLayer, flags)
 */
struct VoxelVertex {
    // Position (12 bytes)
    float x, y, z;

    // Texture coordinates (8 bytes)
    float u, v;

    // Packed data (4 bytes)
    uint8_t normalIndex;    ///< 0-5 for axis-aligned directions
    uint8_t aoLevel;        ///< Ambient occlusion level 0-3
    uint8_t textureLayer;   ///< Array texture layer index
    uint8_t flags;          ///< Reserved for future use

    /**
     * @brief Setup vertex attribute pointers for a VAO.
     *
     * Call this after binding the VBO containing VoxelVertex data.
     * Assumes a VAO is already bound.
     */
    static void setupAttributes();
};

// Ensure struct is packed as expected
static_assert(sizeof(VoxelVertex) == 24, "VoxelVertex must be 24 bytes");

} // namespace Rigel::Voxel
