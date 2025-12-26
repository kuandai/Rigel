#pragma once

/**
 * @file BlockType.h
 * @brief Block type definition for the voxel system.
 *
 * BlockType defines the visual and behavioral properties of a block type.
 * Types are registered with the BlockRegistry and referenced by BlockID.
 */

#include "Block.h"

#include <string>
#include <array>
#include <any>

namespace Rigel::Voxel {

/**
 * @brief Texture assignment per face.
 *
 * Maps each of the 6 faces to a texture path. Factory methods provide
 * common patterns (uniform, top/bottom/sides).
 */
struct FaceTextures {
    /// Per-face texture paths, indexed by Direction
    std::array<std::string, DirectionCount> faces;

    /// Create with same texture on all faces
    static FaceTextures uniform(const std::string& path) {
        FaceTextures ft;
        ft.faces.fill(path);
        return ft;
    }

    /// Create with different top, bottom, and side textures
    static FaceTextures topBottomSides(
        const std::string& top,
        const std::string& bottom,
        const std::string& sides
    ) {
        FaceTextures ft;
        ft.faces[static_cast<size_t>(Direction::PosX)] = sides;
        ft.faces[static_cast<size_t>(Direction::NegX)] = sides;
        ft.faces[static_cast<size_t>(Direction::PosY)] = top;
        ft.faces[static_cast<size_t>(Direction::NegY)] = bottom;
        ft.faces[static_cast<size_t>(Direction::PosZ)] = sides;
        ft.faces[static_cast<size_t>(Direction::NegZ)] = sides;
        return ft;
    }

    /// Get texture for a specific face
    const std::string& forFace(Direction dir) const {
        return faces[static_cast<size_t>(dir)];
    }

    /// Set texture for a specific face
    void setFace(Direction dir, const std::string& path) {
        faces[static_cast<size_t>(dir)] = path;
    }
};

/**
 * @brief Block type definition.
 *
 * Defines all properties of a block type including geometry, rendering,
 * and extensibility. Registered with BlockRegistry.
 *
 * @section example Example Registration
 * @code
 * BlockType stone;
 * stone.identifier = "rigel:stone";
 * stone.model = "cube";
 * stone.isOpaque = true;
 * stone.textures = FaceTextures::uniform("textures/blocks/stone.png");
 * registry.registerBlock(stone.identifier, std::move(stone));
 * @endcode
 */
struct BlockType {
    /// Unique identifier (e.g., "rigel:stone", "mymod:custom_block")
    std::string identifier;

    /// Geometry model type ("cube", "cross", "slab", or custom model ID)
    std::string model = "cube";

    /// Whether this block fully occludes adjacent faces
    bool isOpaque = true;

    /// Whether this block has collision
    bool isSolid = true;

    /// Cull faces when adjacent to the same block type
    bool cullSameType = false;

    /// Per-face texture assignments
    FaceTextures textures;

    /// Rendering layer for draw order
    RenderLayer layer = RenderLayer::Opaque;

    /// Light emission level (0-15, 0 = no light)
    uint8_t emittedLight = 0;

    /// Light attenuation (0-15, 15 = fully blocks light)
    uint8_t lightAttenuation = 15;

    /// User-defined extension data
    std::any customData;
};

} // namespace Rigel::Voxel
