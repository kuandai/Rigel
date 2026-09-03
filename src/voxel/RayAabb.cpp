#include "Rigel/Voxel/RayAabb.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace Rigel::Voxel {
namespace {

bool finite(const glm::vec3& value) {
    return std::isfinite(value.x) &&
        std::isfinite(value.y) &&
        std::isfinite(value.z);
}

bool preferFace(Direction candidate, Direction current) {
    return static_cast<unsigned>(candidate) <
        static_cast<unsigned>(current);
}

Direction negativeFace(size_t axis) {
    constexpr std::array directions = {
        Direction::NegX, Direction::NegY, Direction::NegZ};
    return directions[axis];
}

Direction positiveFace(size_t axis) {
    constexpr std::array directions = {
        Direction::PosX, Direction::PosY, Direction::PosZ};
    return directions[axis];
}

} // namespace

std::optional<RayAabbInterval> intersectRayAabb(
    const glm::vec3& origin,
    const glm::vec3& direction,
    const glm::vec3& boundsMin,
    const glm::vec3& boundsMax,
    float maxDistance
) {
    if (!finite(origin) || !finite(direction) ||
        !finite(boundsMin) || !finite(boundsMax) ||
        !std::isfinite(maxDistance) || maxDistance < 0.0f) {
        return std::nullopt;
    }

    double lengthSquared = 0.0;
    for (size_t axis = 0; axis < 3; ++axis) {
        if (boundsMin[axis] > boundsMax[axis]) {
            return std::nullopt;
        }
        lengthSquared += static_cast<double>(direction[axis]) *
            static_cast<double>(direction[axis]);
    }
    if (std::abs(lengthSquared - 1.0) >
        static_cast<double>(BlockRayIntersectionTolerance) * 2.0) {
        return std::nullopt;
    }

    double entry = -std::numeric_limits<double>::infinity();
    double exit = std::numeric_limits<double>::infinity();
    Direction entryFace = Direction::PosX;
    Direction exitFace = Direction::PosX;
    bool hasEntryFace = false;
    bool hasExitFace = false;

    for (size_t axis = 0; axis < 3; ++axis) {
        const double axisOrigin = origin[axis];
        const double axisDirection = direction[axis];
        const double minimum = boundsMin[axis];
        const double maximum = boundsMax[axis];

        if (std::abs(axisDirection) <= BlockRayIntersectionTolerance) {
            if (axisOrigin < minimum - BlockRayIntersectionTolerance ||
                axisOrigin > maximum + BlockRayIntersectionTolerance) {
                return std::nullopt;
            }
            continue;
        }

        double nearDistance = (minimum - axisOrigin) / axisDirection;
        double farDistance = (maximum - axisOrigin) / axisDirection;
        Direction nearFace = negativeFace(axis);
        Direction farFace = positiveFace(axis);
        if (nearDistance > farDistance) {
            std::swap(nearDistance, farDistance);
            std::swap(nearFace, farFace);
        }

        if (!hasEntryFace ||
            nearDistance > entry + BlockRayIntersectionTolerance ||
            (std::abs(nearDistance - entry) <=
                 BlockRayIntersectionTolerance &&
             preferFace(nearFace, entryFace))) {
            entry = nearDistance;
            entryFace = nearFace;
            hasEntryFace = true;
        }
        if (!hasExitFace ||
            farDistance < exit - BlockRayIntersectionTolerance ||
            (std::abs(farDistance - exit) <=
                 BlockRayIntersectionTolerance &&
             preferFace(farFace, exitFace))) {
            exit = farDistance;
            exitFace = farFace;
            hasExitFace = true;
        }

        if (entry > exit + BlockRayIntersectionTolerance) {
            return std::nullopt;
        }
    }

    if (!hasEntryFace || !hasExitFace ||
        exit < -BlockRayIntersectionTolerance ||
        entry > static_cast<double>(maxDistance) +
            BlockRayIntersectionTolerance) {
        return std::nullopt;
    }

    bool startsInside = true;
    for (size_t axis = 0; axis < 3; ++axis) {
        startsInside = startsInside &&
            boundsMin[axis] < boundsMax[axis] &&
            origin[axis] >
                boundsMin[axis] + BlockRayIntersectionTolerance &&
            origin[axis] <
                boundsMax[axis] - BlockRayIntersectionTolerance;
    }

    return RayAabbInterval{
        .entryDistance = static_cast<float>(entry),
        .exitDistance = static_cast<float>(exit),
        .entryFace = entryFace,
        .exitFace = exitFace,
        .startsInside = startsInside,
    };
}

} // namespace Rigel::Voxel
