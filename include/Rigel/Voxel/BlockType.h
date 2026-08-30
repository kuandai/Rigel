#pragma once

/**
 * @file BlockType.h
 * @brief Block type definition for the voxel system.
 *
 * BlockType defines the visual and behavioral properties of a block type.
 * Types are registered with the BlockRegistry and referenced by BlockID.
 */

#include "Block.h"
#include "BlockModel.h"

#include <any>
#include <array>
#include <string>
#include <string_view>
#include <unordered_map>

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

    /// Bind a named texture slot used by a normalized model.
    void bind(std::string slot, std::string path) {
        namedSlots[std::move(slot)] = std::move(path);
    }

    /// Resolve a named model slot, including canonical cube face slots.
    const std::string* find(std::string_view slot) const {
        const auto named = namedSlots.find(std::string(slot));
        if (named != namedSlots.end()) {
            return &named->second;
        }
        if (const auto direction = BlockModel::directionFromName(slot)) {
            return &forFace(*direction);
        }
        return nullptr;
    }

    const std::unordered_map<std::string, std::string>& named() const {
        return namedSlots;
    }

private:
    std::unordered_map<std::string, std::string> namedSlots;
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
 * stone.model = BlockModel::fullCube();
 * stone.isOpaque = true;
 * stone.textures = FaceTextures::uniform("textures/blocks/stone.png");
 * registry.registerBlock(stone.identifier, std::move(stone));
 * @endcode
 */
struct BlockType {
    /// Unique identifier (e.g., "rigel:stone", "mymod:custom_block")
    std::string identifier;

    /// Immutable geometry reference and constrained per-registration orientation.
    BlockModelInstance model;

    /// Whether this block fully occludes adjacent faces
    bool isOpaque = true;

    /// Whether this block has collision
    bool isSolid = true;

    /// Cull faces when adjacent to the same block type
    bool cullSameType = false;

    /// Block-local texture bindings for the model's slots.
    FaceTextures textures;

    /// Rendering layer for draw order
    RenderLayer layer = RenderLayer::Opaque;

    /// Render-layer overrides for named texture slots in normalized models.
    std::unordered_map<std::string, RenderLayer> textureRenderLayers;

    RenderLayer renderLayerForTextureSlot(const std::string& slot) const {
        if (textureRenderLayers.empty()) return layer;
        const auto found = textureRenderLayers.find(slot);
        return found == textureRenderLayers.end() ? layer : found->second;
    }

    /// Light emission level (0-15, 0 = no light)
    uint8_t emittedLight = 0;

    /// Light attenuation (0-15, 15 = fully blocks light)
    uint8_t lightAttenuation = 15;

    /// User-defined extension data
    std::any customData;
};

} // namespace Rigel::Voxel
