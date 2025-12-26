#pragma once

/**
 * @file Block.h
 * @brief Core block type definitions for the voxel system.
 *
 * This file defines the fundamental types for block storage:
 * - BlockID: Type identifier (0 = air)
 * - BlockState: Per-instance state (ID + metadata + light)
 * - Direction: Face directions for culling and neighbor access
 * - RenderLayer: Draw order classification
 */

#include <cstddef>
#include <cstdint>
#include <array>

namespace Rigel::Voxel {

/**
 * @brief Identifies a block type.
 *
 * BlockID is a 16-bit value supporting up to 65,535 unique block types.
 * ID 0 is reserved for air (empty space).
 */
struct BlockID {
    uint16_t type = 0;

    bool operator==(const BlockID&) const = default;

    /// Check if this is air (empty)
    bool isAir() const { return type == 0; }
};

/**
 * @brief Per-block instance state.
 *
 * Each block in a chunk stores its type, metadata (rotation/variant),
 * and light level. Total size: 4 bytes per block.
 */
struct BlockState {
    BlockID id;
    uint8_t metadata = 0;    ///< Rotation, variant, connection state
    uint8_t lightLevel = 0;  ///< Packed: 4 bits sky + 4 bits block light

    bool operator==(const BlockState&) const = default;

    /// Check if this block is air
    bool isAir() const { return id.isAir(); }

    /// Get sky light level (0-15)
    uint8_t skyLight() const { return (lightLevel >> 4) & 0x0F; }

    /// Get block light level (0-15)
    uint8_t blockLight() const { return lightLevel & 0x0F; }

    /// Set sky light level (0-15)
    void setSkyLight(uint8_t level) {
        lightLevel = (lightLevel & 0x0F) | ((level & 0x0F) << 4);
    }

    /// Set block light level (0-15)
    void setBlockLight(uint8_t level) {
        lightLevel = (lightLevel & 0xF0) | (level & 0x0F);
    }
};

/**
 * @brief Axis-aligned face directions.
 *
 * Used for face culling, neighbor block access, and normal indexing.
 * Values 0-5 map directly to normal lookup tables in shaders.
 */
enum class Direction : uint8_t {
    PosX = 0,  ///< East  (+X)
    NegX = 1,  ///< West  (-X)
    PosY = 2,  ///< Up    (+Y)
    NegY = 3,  ///< Down  (-Y)
    PosZ = 4,  ///< South (+Z)
    NegZ = 5   ///< North (-Z)
};

/// Number of directions (for array sizing)
inline constexpr size_t DirectionCount = 6;

/**
 * @brief Get the opposite direction.
 * @param dir The direction to reverse
 * @return The opposite direction
 */
constexpr Direction opposite(Direction dir) {
    constexpr std::array<Direction, 6> opposites = {
        Direction::NegX,  // opposite of PosX
        Direction::PosX,  // opposite of NegX
        Direction::NegY,  // opposite of PosY
        Direction::PosY,  // opposite of NegY
        Direction::NegZ,  // opposite of PosZ
        Direction::PosZ   // opposite of NegZ
    };
    return opposites[static_cast<size_t>(dir)];
}

/**
 * @brief Get direction offset as integers.
 * @param dir The direction
 * @param dx Output X offset (-1, 0, or 1)
 * @param dy Output Y offset (-1, 0, or 1)
 * @param dz Output Z offset (-1, 0, or 1)
 */
constexpr void directionOffset(Direction dir, int& dx, int& dy, int& dz) {
    dx = dy = dz = 0;
    switch (dir) {
        case Direction::PosX: dx =  1; break;
        case Direction::NegX: dx = -1; break;
        case Direction::PosY: dy =  1; break;
        case Direction::NegY: dy = -1; break;
        case Direction::PosZ: dz =  1; break;
        case Direction::NegZ: dz = -1; break;
    }
}

/**
 * @brief Rendering layer for draw order.
 *
 * Blocks are sorted by layer for correct rendering:
 * - Opaque: Rendered first, writes depth, no blending
 * - Cutout: Alpha-tested (leaves, tall grass), writes depth
 * - Transparent: Alpha-blended, sorted back-to-front
 * - Emissive: Additive blending for glow effects
 */
enum class RenderLayer : uint8_t {
    Opaque = 0,
    Cutout = 1,
    Transparent = 2,
    Emissive = 3
};

/// Number of render layers
inline constexpr size_t RenderLayerCount = 4;

} // namespace Rigel::Voxel
