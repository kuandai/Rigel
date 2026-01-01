#pragma once

#include <glm/vec3.hpp>

namespace Rigel::Entity {

struct Aabb {
    glm::vec3 min{0.0f};
    glm::vec3 max{0.0f};

    glm::vec3 size() const { return max - min; }

    bool intersects(const Aabb& other) const {
        return min.x <= other.max.x && max.x >= other.min.x &&
               min.y <= other.max.y && max.y >= other.min.y &&
               min.z <= other.max.z && max.z >= other.min.z;
    }

    Aabb translated(const glm::vec3& offset) const {
        return Aabb{min + offset, max + offset};
    }
};

} // namespace Rigel::Entity
