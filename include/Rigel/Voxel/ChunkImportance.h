#pragma once

#include "ChunkCoord.h"

#include <cstdint>
#include <limits>

namespace Rigel::Voxel {

struct ChunkImportance {
    bool cameraContaining = false;
    uint64_t distanceSquared = 0;
    ChunkCoord coord{};

    bool operator==(const ChunkImportance&) const = default;
};

struct ChunkImportancePrecedes {
    bool operator()(const ChunkImportance& lhs,
                    const ChunkImportance& rhs) const {
        if (lhs.cameraContaining != rhs.cameraContaining) {
            return lhs.cameraContaining;
        }
        if (lhs.distanceSquared != rhs.distanceSquared) {
            return lhs.distanceSquared < rhs.distanceSquared;
        }
        return lhs.coord < rhs.coord;
    }
};

inline ChunkImportance chunkImportance(ChunkCoord camera,
                                       ChunkCoord coord) {
    const auto magnitude = [](int64_t value) {
        return static_cast<uint64_t>(value < 0 ? -value : value);
    };
    const auto saturatedAdd = [](uint64_t lhs, uint64_t rhs) {
        const uint64_t maximum = std::numeric_limits<uint64_t>::max();
        return rhs > maximum - lhs ? maximum : lhs + rhs;
    };

    const uint64_t dx = magnitude(
        static_cast<int64_t>(coord.x) - camera.x);
    const uint64_t dy = magnitude(
        static_cast<int64_t>(coord.y) - camera.y);
    const uint64_t dz = magnitude(
        static_cast<int64_t>(coord.z) - camera.z);
    const uint64_t xy = saturatedAdd(dx * dx, dy * dy);
    return ChunkImportance{
        .cameraContaining = coord == camera,
        .distanceSquared = saturatedAdd(xy, dz * dz),
        .coord = coord
    };
}

} // namespace Rigel::Voxel
