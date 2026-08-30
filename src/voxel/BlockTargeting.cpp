#include "Rigel/Voxel/BlockTargeting.h"

#include "Rigel/Voxel/World.h"

#include <cmath>
#include <limits>

namespace Rigel::Voxel {
namespace {

void setupAxis(
    float origin,
    float direction,
    int block,
    int& step,
    float& nextBoundaryDistance,
    float& boundaryInterval
) {
    if (direction > 0.0f) {
        step = 1;
        nextBoundaryDistance =
            (static_cast<float>(block + 1) - origin) / direction;
        boundaryInterval = 1.0f / direction;
    } else if (direction < 0.0f) {
        step = -1;
        nextBoundaryDistance =
            (origin - static_cast<float>(block)) / -direction;
        boundaryInterval = 1.0f / -direction;
    } else {
        step = 0;
        nextBoundaryDistance = std::numeric_limits<float>::infinity();
        boundaryInterval = std::numeric_limits<float>::infinity();
    }
}

} // namespace

std::optional<BlockTarget> raycastBlock(
    const World& world,
    const glm::vec3& origin,
    const glm::vec3& direction,
    float maxDistance
) {
    const float directionLength = glm::length(direction);
    if (directionLength <= 0.0001f) {
        return std::nullopt;
    }

    const glm::vec3 rayDirection = direction / directionLength;
    glm::ivec3 block{
        static_cast<int>(std::floor(origin.x)),
        static_cast<int>(std::floor(origin.y)),
        static_cast<int>(std::floor(origin.z)),
    };
    glm::ivec3 step{};
    glm::vec3 nextBoundaryDistance{};
    glm::vec3 boundaryInterval{};
    setupAxis(
        origin.x,
        rayDirection.x,
        block.x,
        step.x,
        nextBoundaryDistance.x,
        boundaryInterval.x);
    setupAxis(
        origin.y,
        rayDirection.y,
        block.y,
        step.y,
        nextBoundaryDistance.y,
        boundaryInterval.y);
    setupAxis(
        origin.z,
        rayDirection.z,
        block.z,
        step.z,
        nextBoundaryDistance.z,
        boundaryInterval.z);

    glm::ivec3 normal{};
    float distance = 0.0f;
    while (distance <= maxDistance) {
        const BlockState state = world.getBlock(block.x, block.y, block.z);
        if (!state.isAir()) {
            return BlockTarget{block, normal, state, distance};
        }

        if (nextBoundaryDistance.x < nextBoundaryDistance.y) {
            if (nextBoundaryDistance.x < nextBoundaryDistance.z) {
                block.x += step.x;
                distance = nextBoundaryDistance.x;
                nextBoundaryDistance.x += boundaryInterval.x;
                normal = {-step.x, 0, 0};
            } else {
                block.z += step.z;
                distance = nextBoundaryDistance.z;
                nextBoundaryDistance.z += boundaryInterval.z;
                normal = {0, 0, -step.z};
            }
        } else if (nextBoundaryDistance.y < nextBoundaryDistance.z) {
            block.y += step.y;
            distance = nextBoundaryDistance.y;
            nextBoundaryDistance.y += boundaryInterval.y;
            normal = {0, -step.y, 0};
        } else {
            block.z += step.z;
            distance = nextBoundaryDistance.z;
            nextBoundaryDistance.z += boundaryInterval.z;
            normal = {0, 0, -step.z};
        }
    }

    return std::nullopt;
}

} // namespace Rigel::Voxel
