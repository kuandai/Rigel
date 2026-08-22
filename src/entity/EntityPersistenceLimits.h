#pragma once

#include <glm/vec3.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace Rigel::Entity::detail {

inline constexpr uint32_t MaxChunksPerEntityRegion = 4'096;
inline constexpr uint32_t MaxEntitiesPerChunk = 1'048'576;
inline constexpr uint32_t MaxEntityStringBytes = 1'048'576;
inline constexpr size_t MaxEntityRegionBytes = 64 * 1024 * 1024;
inline constexpr size_t MinEncodedChunkBytes = 16;
inline constexpr size_t MinEncodedEntityBytes = 60;

inline bool isFiniteVector(const glm::vec3& value) {
    return std::isfinite(value.x) &&
        std::isfinite(value.y) &&
        std::isfinite(value.z);
}

inline bool isPersistablePosition(const glm::vec3& value) {
    constexpr double minimum =
        static_cast<double>(std::numeric_limits<int>::min());
    constexpr double maximum =
        static_cast<double>(std::numeric_limits<int>::max());
    return isFiniteVector(value) &&
        static_cast<double>(value.x) >= minimum &&
        static_cast<double>(value.x) <= maximum &&
        static_cast<double>(value.y) >= minimum &&
        static_cast<double>(value.y) <= maximum &&
        static_cast<double>(value.z) >= minimum &&
        static_cast<double>(value.z) <= maximum;
}

} // namespace Rigel::Entity::detail
