#include "Rigel/Voxel/BlockTargeting.h"

#include "BlockModelGeometry.h"
#include "Rigel/Voxel/RayAabb.h"
#include "Rigel/Voxel/World.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace Rigel::Voxel {
namespace {

struct DdaAxis {
    int step = 0;
    double nextBoundaryDistance =
        std::numeric_limits<double>::infinity();
    double boundaryInterval =
        std::numeric_limits<double>::infinity();
};

struct ModelHit {
    float distance = 0.0f;
    Direction face = Direction::PosX;
    size_t cuboidIndex = 0;
};

struct OwnerCandidateRange {
    std::array<int, 3> first{};
    std::array<int, 3> last{};

    bool contains(const glm::ivec3& owner) const {
        for (size_t axis = 0; axis < 3; ++axis) {
            if (owner[axis] < first[axis] || owner[axis] > last[axis]) {
                return false;
            }
        }
        return true;
    }
};

bool finite(const glm::vec3& value) {
    return std::isfinite(value.x) &&
        std::isfinite(value.y) &&
        std::isfinite(value.z);
}

std::optional<glm::vec3> normalized(const glm::vec3& direction) {
    if (!finite(direction)) {
        return std::nullopt;
    }
    double lengthSquared = 0.0;
    for (size_t axis = 0; axis < 3; ++axis) {
        lengthSquared += static_cast<double>(direction[axis]) *
            static_cast<double>(direction[axis]);
    }
    if (!std::isfinite(lengthSquared) ||
        lengthSquared <=
            static_cast<double>(BlockRayIntersectionTolerance) *
            BlockRayIntersectionTolerance) {
        return std::nullopt;
    }
    const double inverseLength = 1.0 / std::sqrt(lengthSquared);
    return glm::vec3{
        static_cast<float>(direction.x * inverseLength),
        static_cast<float>(direction.y * inverseLength),
        static_cast<float>(direction.z * inverseLength),
    };
}

size_t directionAxis(Direction direction) {
    return static_cast<size_t>(direction) / 2;
}

float directionSign(Direction direction) {
    return static_cast<size_t>(direction) % 2 == 0 ? 1.0f : -1.0f;
}

glm::ivec3 directionNormal(Direction direction) {
    glm::ivec3 normal{};
    normal[directionAxis(direction)] =
        directionSign(direction) > 0.0f ? 1 : -1;
    return normal;
}

bool preferModelHit(const ModelHit& candidate, const ModelHit& current) {
    if (candidate.distance <
        current.distance - BlockRayIntersectionTolerance) {
        return true;
    }
    if (std::abs(candidate.distance - current.distance) >
        BlockRayIntersectionTolerance) {
        return false;
    }
    if (candidate.cuboidIndex != current.cuboidIndex) {
        return candidate.cuboidIndex < current.cuboidIndex;
    }
    return static_cast<unsigned>(candidate.face) <
        static_cast<unsigned>(current.face);
}

bool strictlyInside(
    const glm::vec3& origin,
    const BlockModelBounds& bounds,
    const glm::ivec3& owner
) {
    for (size_t axis = 0; axis < 3; ++axis) {
        const double minimum = static_cast<double>(owner[axis]) +
            bounds.min[axis];
        const double maximum = static_cast<double>(owner[axis]) +
            bounds.max[axis];
        if (minimum >= maximum ||
            origin[axis] <= minimum + BlockRayIntersectionTolerance ||
            origin[axis] >= maximum - BlockRayIntersectionTolerance) {
            return false;
        }
    }
    return true;
}

bool hasArea(const BlockModelBounds& bounds, Direction face) {
    const size_t normalAxis = directionAxis(face);
    for (size_t axis = 0; axis < 3; ++axis) {
        if (axis != normalAxis &&
            bounds.min[axis] >= bounds.max[axis]) {
            return false;
        }
    }
    return true;
}

std::optional<ModelHit> intersectFullCube(
    const glm::ivec3& owner,
    const glm::vec3& origin,
    const glm::vec3& direction,
    float maxDistance
) {
    const glm::vec3 minimum(owner);
    const glm::vec3 maximum = minimum + glm::vec3(1.0f);
    const auto interval = intersectRayAabb(
        origin, direction, minimum, maximum, maxDistance);
    if (!interval) {
        return std::nullopt;
    }

    float distance = interval->entryDistance;
    Direction face = interval->entryFace;
    if (interval->startsInside) {
        distance = interval->exitDistance;
        face = interval->exitFace;
    } else if (distance < -BlockRayIntersectionTolerance) {
        if (interval->exitDistance > BlockRayIntersectionTolerance) {
            return std::nullopt;
        }
        distance = interval->exitDistance;
        face = interval->exitFace;
    }
    if (distance < -BlockRayIntersectionTolerance ||
        distance > maxDistance + BlockRayIntersectionTolerance) {
        return std::nullopt;
    }
    return ModelHit{
        .distance = std::max(0.0f, distance),
        .face = face,
        .cuboidIndex = 0,
    };
}

std::optional<ModelHit> intersectModel(
    const BlockModelInstance& instance,
    const glm::ivec3& owner,
    const glm::vec3& origin,
    const glm::vec3& direction,
    float maxDistance
) {
    if (!instance || instance->isEmpty()) {
        return std::nullopt;
    }
    if (instance->isFullCube()) {
        return intersectFullCube(
            owner, origin, direction, maxDistance);
    }

    std::optional<ModelHit> best;
    const auto& cuboids = instance->cuboids();
    for (size_t cuboidIndex = 0;
         cuboidIndex < cuboids.size(); ++cuboidIndex) {
        const BlockModelCuboid& cuboid = cuboids[cuboidIndex];
        const BlockModelBounds bounds = detail::orientedBounds(
            cuboid.bounds, instance.orientation);
        const bool inside = strictlyInside(origin, bounds, owner);

        for (size_t sourceFaceIndex = 0;
             sourceFaceIndex < DirectionCount; ++sourceFaceIndex) {
            if (!cuboid.faces[sourceFaceIndex]) {
                continue;
            }
            const Direction face = detail::orientedDirection(
                static_cast<Direction>(sourceFaceIndex),
                instance.orientation);
            if (!hasArea(bounds, face)) {
                continue;
            }

            const size_t normalAxis = directionAxis(face);
            const double axisDirection = direction[normalAxis];
            if (axisDirection == 0.0) {
                continue;
            }
            const double facing = axisDirection * directionSign(face);
            const double plane = static_cast<double>(owner[normalAxis]) +
                (directionSign(face) > 0.0f
                    ? bounds.max[normalAxis]
                    : bounds.min[normalAxis]);
            const double rawDistance =
                (plane - origin[normalAxis]) / axisDirection;
            if (rawDistance < -BlockRayIntersectionTolerance ||
                rawDistance > static_cast<double>(maxDistance) +
                    BlockRayIntersectionTolerance) {
                continue;
            }
            if (inside) {
                if (facing <= 0.0) {
                    continue;
                }
            } else if (facing >= 0.0 &&
                       std::abs(rawDistance) >
                           BlockRayIntersectionTolerance) {
                continue;
            }

            bool withinFace = true;
            for (size_t axis = 0; axis < 3; ++axis) {
                if (axis == normalAxis) {
                    continue;
                }
                const double coordinate = origin[axis] +
                    direction[axis] * rawDistance;
                const double minimum =
                    static_cast<double>(owner[axis]) + bounds.min[axis];
                const double maximum =
                    static_cast<double>(owner[axis]) + bounds.max[axis];
                if (coordinate <
                        minimum - BlockRayIntersectionTolerance ||
                    coordinate >
                        maximum + BlockRayIntersectionTolerance) {
                    withinFace = false;
                    break;
                }
            }
            if (!withinFace) {
                continue;
            }

            ModelHit candidate{
                .distance = static_cast<float>(
                    std::max(0.0, rawDistance)),
                .face = face,
                .cuboidIndex = cuboidIndex,
            };
            if (!best || preferModelHit(candidate, *best)) {
                best = candidate;
            }
        }
    }
    return best;
}

bool preferBlockTarget(
    const BlockTarget& candidate,
    const BlockTarget& current
) {
    if (candidate.distance <
        current.distance - BlockRayIntersectionTolerance) {
        return true;
    }
    if (std::abs(candidate.distance - current.distance) >
        BlockRayIntersectionTolerance) {
        return false;
    }
    if (candidate.block.x != current.block.x) {
        return candidate.block.x < current.block.x;
    }
    if (candidate.block.y != current.block.y) {
        return candidate.block.y < current.block.y;
    }
    if (candidate.block.z != current.block.z) {
        return candidate.block.z < current.block.z;
    }
    if (candidate.cuboidIndex != current.cuboidIndex) {
        return candidate.cuboidIndex < current.cuboidIndex;
    }
    return static_cast<unsigned>(candidate.face) <
        static_cast<unsigned>(current.face);
}

void setupAxis(
    float origin,
    float direction,
    int block,
    DdaAxis& axis
) {
    if (direction > 0.0f) {
        axis.step = 1;
        axis.nextBoundaryDistance =
            (static_cast<double>(block) + 1.0 - origin) / direction;
        axis.boundaryInterval = 1.0 / direction;
    } else if (direction < 0.0f) {
        axis.step = -1;
        axis.nextBoundaryDistance =
            (origin - static_cast<double>(block)) / -direction;
        axis.boundaryInterval = 1.0 / -direction;
    }
}

bool coordinateBlock(double coordinate, int& block) {
    if (!std::isfinite(coordinate)) {
        return false;
    }
    const double floored = std::floor(coordinate);
    if (floored < std::numeric_limits<int>::min() ||
        floored > std::numeric_limits<int>::max()) {
        return false;
    }
    block = static_cast<int>(floored);
    return true;
}

bool candidateRange(
    int cell,
    float extentMin,
    float extentMax,
    int& first,
    int& last
) {
    const double minimum = std::ceil(
        static_cast<double>(cell) - extentMax -
        BlockRayIntersectionTolerance);
    const double maximum = std::floor(
        static_cast<double>(cell) + 1.0 - extentMin +
        BlockRayIntersectionTolerance);
    if (maximum < std::numeric_limits<int>::min() ||
        minimum > std::numeric_limits<int>::max()) {
        return false;
    }
    first = static_cast<int>(std::max(
        minimum, static_cast<double>(std::numeric_limits<int>::min())));
    last = static_cast<int>(std::min(
        maximum, static_cast<double>(std::numeric_limits<int>::max())));
    return first <= last;
}

std::optional<OwnerCandidateRange> ownerCandidateRange(
    const glm::ivec3& cell,
    const BlockModelBounds& modelExtents
) {
    OwnerCandidateRange result;
    for (size_t axis = 0; axis < 3; ++axis) {
        if (!candidateRange(
                cell[axis], modelExtents.min[axis],
                modelExtents.max[axis], result.first[axis],
                result.last[axis])) {
            return std::nullopt;
        }
    }
    return result;
}

bool advanceCell(int& coordinate, const DdaAxis& axis) {
    if ((axis.step > 0 && coordinate == std::numeric_limits<int>::max()) ||
        (axis.step < 0 && coordinate == std::numeric_limits<int>::min())) {
        return false;
    }
    coordinate += axis.step;
    return true;
}

} // namespace

std::optional<BlockTarget> raycastBlock(
    const World& world,
    const glm::vec3& origin,
    const glm::vec3& direction,
    float maxDistance
) {
    if (!finite(origin) || !std::isfinite(maxDistance) ||
        maxDistance < 0.0f) {
        return std::nullopt;
    }
    const auto normalizedDirection = normalized(direction);
    if (!normalizedDirection) {
        return std::nullopt;
    }

    // Reject the segment before traversal if any point at its far endpoint
    // falls outside the integer block-coordinate domain. Because the segment
    // is linear, representable endpoints also bound every intermediate cell.
    for (size_t axis = 0; axis < 3; ++axis) {
        const double endpoint = static_cast<double>(origin[axis]) +
            static_cast<double>((*normalizedDirection)[axis]) *
                static_cast<double>(maxDistance);
        int endpointCell = 0;
        if (!coordinateBlock(endpoint, endpointCell)) {
            return std::nullopt;
        }
    }

    const BlockRegistry& registry = world.blockRegistry();
    const auto& modelExtents = registry.modelExtents();
    if (!modelExtents) {
        return std::nullopt;
    }

    glm::ivec3 cell{};
    if (!coordinateBlock(origin.x, cell.x) ||
        !coordinateBlock(origin.y, cell.y) ||
        !coordinateBlock(origin.z, cell.z)) {
        return std::nullopt;
    }

    std::array<DdaAxis, 3> axes{};
    for (size_t axis = 0; axis < 3; ++axis) {
        setupAxis(
            origin[axis], (*normalizedDirection)[axis],
            cell[axis], axes[axis]);
    }

    std::optional<BlockTarget> best;
    std::optional<OwnerCandidateRange> previousCandidates;
    double cellEntryDistance = 0.0;
    while (cellEntryDistance <=
           static_cast<double>(maxDistance) +
               BlockRayIntersectionTolerance) {
        const auto candidates = ownerCandidateRange(cell, *modelExtents);
        if (candidates) {
            for (int64_t wideX = candidates->first[0];
                 wideX <= candidates->last[0]; ++wideX) {
                for (int64_t wideY = candidates->first[1];
                     wideY <= candidates->last[1]; ++wideY) {
                    for (int64_t wideZ = candidates->first[2];
                         wideZ <= candidates->last[2]; ++wideZ) {
                        const glm::ivec3 owner{
                            static_cast<int>(wideX),
                            static_cast<int>(wideY),
                            static_cast<int>(wideZ),
                        };
                        // Candidate boxes translate monotonically with the
                        // DDA. An owner can therefore enter only once, so a
                        // single previous box is an allocation-free visited
                        // set for the complete ray.
                        if (previousCandidates &&
                            previousCandidates->contains(owner)) {
                            continue;
                        }
                        const BlockState state = world.getBlock(
                            owner.x, owner.y, owner.z);
                        if (state.isAir()) {
                            continue;
                        }
                        const BlockType& type = registry.getType(state.id);
                        const auto modelHit = intersectModel(
                            type.model, owner, origin,
                            *normalizedDirection, maxDistance);
                        if (!modelHit) {
                            continue;
                        }

                        BlockTarget candidate{
                            .block = owner,
                            .normal = directionNormal(modelHit->face),
                            .state = state,
                            .distance = modelHit->distance,
                            .position = origin +
                                *normalizedDirection * modelHit->distance,
                            .face = modelHit->face,
                            .cuboidIndex = modelHit->cuboidIndex,
                        };
                        if (!best || preferBlockTarget(candidate, *best)) {
                            best = candidate;
                        }
                    }
                }
            }
        }
        previousCandidates = candidates;

        double nextDistance = std::numeric_limits<double>::infinity();
        for (const DdaAxis& axis : axes) {
            nextDistance = std::min(
                nextDistance, axis.nextBoundaryDistance);
        }
        // Every owner whose model can meet the ray before the next boundary
        // belongs to the current candidate box and has now been tested. Keep
        // walking through a boundary-distance tie; otherwise no untested
        // owner can replace the best hit.
        if (best && nextDistance >
                static_cast<double>(best->distance) +
                    BlockRayIntersectionTolerance) {
            return best;
        }
        if (!std::isfinite(nextDistance) ||
            nextDistance > static_cast<double>(maxDistance) +
                BlockRayIntersectionTolerance) {
            break;
        }

        bool advanced = false;
        for (size_t axis = 0; axis < 3; ++axis) {
            if (axes[axis].nextBoundaryDistance <=
                nextDistance + BlockRayIntersectionTolerance) {
                if (!advanceCell(cell[axis], axes[axis])) {
                    return best;
                }
                axes[axis].nextBoundaryDistance +=
                    axes[axis].boundaryInterval;
                advanced = true;
            }
        }
        if (!advanced) {
            break;
        }
        cellEntryDistance = nextDistance;
    }

    return best;
}

} // namespace Rigel::Voxel
