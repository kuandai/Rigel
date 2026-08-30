#pragma once

#include "Block.h"

#include <glm/vec3.hpp>

#include <optional>

namespace Rigel::Voxel {

class World;

/** Result of testing whole block cells along a world-space ray. */
struct BlockTarget {
    glm::ivec3 block{};
    glm::ivec3 normal{};
    BlockState state{};
    float distance = 0.0f;

    bool operator==(const BlockTarget&) const = default;
};

/**
 * Find the first non-air block cell along a ray using the gameplay DDA.
 * Model geometry inside or outside the cell is intentionally not considered.
 */
std::optional<BlockTarget> raycastBlock(
    const World& world,
    const glm::vec3& origin,
    const glm::vec3& direction,
    float maxDistance);

} // namespace Rigel::Voxel
