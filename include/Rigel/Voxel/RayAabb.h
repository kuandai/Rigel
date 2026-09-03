#pragma once

#include "Block.h"

#include <glm/vec3.hpp>

#include <optional>

namespace Rigel::Voxel {

/**
 * The single tolerance used by block-model ray intersection.
 *
 * Directions at or below this magnitude are parallel, points within this
 * distance of a slab boundary are on that boundary, and distances within
 * this amount are ties. AABB edge and corner ties choose the lowest-valued
 * Direction, making their normals stable.
 */
inline constexpr float BlockRayIntersectionTolerance = 1.0e-5f;

/** The forward overlap of a world-distance ray and an axis-aligned box. */
struct RayAabbInterval {
    float entryDistance = 0.0f;
    float exitDistance = 0.0f;
    Direction entryFace = Direction::PosX;
    Direction exitFace = Direction::PosX;
    bool startsInside = false;

    bool operator==(const RayAabbInterval&) const = default;
};

/**
 * Intersect a finite ray with inclusive AABB slabs.
 *
 * direction must be normalized, and maxDistance is an inclusive world-space
 * limit. Entry distance remains negative for an inside origin so callers can
 * distinguish the nearest forward exit. Boundary origins are not inside.
 * Invalid/non-finite inputs and disjoint intervals return no result.
 */
std::optional<RayAabbInterval> intersectRayAabb(
    const glm::vec3& origin,
    const glm::vec3& direction,
    const glm::vec3& boundsMin,
    const glm::vec3& boundsMax,
    float maxDistance);

} // namespace Rigel::Voxel
