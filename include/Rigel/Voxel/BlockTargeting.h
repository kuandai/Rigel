#pragma once

#include "BlockModel.h"

#include <glm/vec3.hpp>

#include <cstddef>
#include <optional>

namespace Rigel::Voxel {

class World;
class BlockRegistry;

/** Exact declared block-model surface selected by a world-space ray. */
struct BlockTarget {
    glm::ivec3 block{};
    glm::ivec3 normal{};
    BlockState state{};
    float distance = 0.0f;
    glm::vec3 position{};
    Direction face = Direction::PosX;
    size_t cuboidIndex = 0;

    bool operator==(const BlockTarget&) const = default;
};

/**
 * Find the nearest declared oriented block-model surface along a gameplay DDA.
 * Empty models do not hit. The search accounts for registered model overhang,
 * and direction is normalized so returned distance is in world units.
 */
std::optional<BlockTarget> raycastBlock(
    const World& world,
    const glm::vec3& origin,
    const glm::vec3& direction,
    float maxDistance);

/** Resolve the selected cuboid's oriented world-space bounds. */
std::optional<BlockModelBounds> blockTargetBounds(
    const BlockRegistry& registry,
    const BlockTarget& target);

} // namespace Rigel::Voxel
