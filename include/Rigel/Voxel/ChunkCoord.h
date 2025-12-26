#pragma once

/**
 * @file ChunkCoord.h
 * @brief Chunk coordinate system for the voxel world.
 *
 * Chunks are fixed-size cubic regions. This file provides coordinate types
 * and utilities for converting between world and chunk-local coordinates.
 */

#include <cstddef>
#include <cstdint>
#include <functional>
#include <glm/vec3.hpp>

namespace Rigel::Voxel {

/// Forward declaration for SIZE constant
inline constexpr int ChunkSize = 32;

/**
 * @brief Integer coordinate identifying a chunk in the world.
 *
 * Chunk coordinates are in chunk units, not world units.
 * World position = ChunkCoord * ChunkSize.
 */
struct ChunkCoord {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;

    bool operator==(const ChunkCoord&) const = default;
    auto operator<=>(const ChunkCoord&) const = default;

    /// Offset by integer amounts
    ChunkCoord offset(int dx, int dy, int dz) const {
        return {x + dx, y + dy, z + dz};
    }

    /// Convert to world-space center point
    glm::vec3 toWorldCenter() const {
        constexpr float halfSize = ChunkSize / 2.0f;
        return glm::vec3(
            static_cast<float>(x * ChunkSize) + halfSize,
            static_cast<float>(y * ChunkSize) + halfSize,
            static_cast<float>(z * ChunkSize) + halfSize
        );
    }

    /// Convert to world-space minimum corner
    glm::vec3 toWorldMin() const {
        return glm::vec3(
            static_cast<float>(x * ChunkSize),
            static_cast<float>(y * ChunkSize),
            static_cast<float>(z * ChunkSize)
        );
    }

    /// Convert to world-space maximum corner
    glm::vec3 toWorldMax() const {
        return glm::vec3(
            static_cast<float>((x + 1) * ChunkSize),
            static_cast<float>((y + 1) * ChunkSize),
            static_cast<float>((z + 1) * ChunkSize)
        );
    }
};

/**
 * @brief Hash function for ChunkCoord.
 *
 * Uses spatial hashing with large primes for good distribution.
 */
struct ChunkCoordHash {
    std::size_t operator()(const ChunkCoord& c) const {
        // Interleaved hashing for spatial coherence
        return std::hash<int64_t>{}(
            (static_cast<int64_t>(c.x) * 73856093) ^
            (static_cast<int64_t>(c.y) * 19349663) ^
            (static_cast<int64_t>(c.z) * 83492791)
        );
    }
};

/**
 * @brief Convert world position to chunk coordinate.
 *
 * @param wx World X coordinate
 * @param wy World Y coordinate
 * @param wz World Z coordinate
 * @return The chunk containing this world position
 */
inline ChunkCoord worldToChunk(int wx, int wy, int wz) {
    // Handle negative coordinates correctly (floor division)
    auto floorDiv = [](int a, int b) {
        return (a >= 0) ? (a / b) : ((a - b + 1) / b);
    };
    return {
        floorDiv(wx, ChunkSize),
        floorDiv(wy, ChunkSize),
        floorDiv(wz, ChunkSize)
    };
}

/**
 * @brief Convert world position to local position within chunk.
 *
 * @param wx World X coordinate
 * @param wy World Y coordinate
 * @param wz World Z coordinate
 * @param[out] lx Local X coordinate (0 to ChunkSize-1)
 * @param[out] ly Local Y coordinate (0 to ChunkSize-1)
 * @param[out] lz Local Z coordinate (0 to ChunkSize-1)
 */
inline void worldToLocal(int wx, int wy, int wz, int& lx, int& ly, int& lz) {
    // Handle negative coordinates correctly (positive modulo)
    auto posMod = [](int a, int b) {
        int r = a % b;
        return (r < 0) ? (r + b) : r;
    };
    lx = posMod(wx, ChunkSize);
    ly = posMod(wy, ChunkSize);
    lz = posMod(wz, ChunkSize);
}

/**
 * @brief Convert chunk coordinate and local position to world position.
 *
 * @param chunk The chunk coordinate
 * @param lx Local X coordinate (0 to ChunkSize-1)
 * @param ly Local Y coordinate (0 to ChunkSize-1)
 * @param lz Local Z coordinate (0 to ChunkSize-1)
 * @param[out] wx World X coordinate
 * @param[out] wy World Y coordinate
 * @param[out] wz World Z coordinate
 */
inline void localToWorld(const ChunkCoord& chunk, int lx, int ly, int lz,
                         int& wx, int& wy, int& wz) {
    wx = chunk.x * ChunkSize + lx;
    wy = chunk.y * ChunkSize + ly;
    wz = chunk.z * ChunkSize + lz;
}

} // namespace Rigel::Voxel
