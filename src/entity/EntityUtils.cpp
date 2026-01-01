#include "Rigel/Entity/EntityUtils.h"

#include "Rigel/Entity/WorldEntities.h"

#include <algorithm>
#include <cmath>

namespace Rigel::Entity {

void applyFriction(float friction, glm::vec3& velocity) {
    friction = std::clamp(friction, 0.0f, 1.0f);
    float factor = 1.0f - friction;
    velocity.x *= factor;
    velocity.z *= factor;
    if (std::abs(velocity.x) < 1.0e-4f) {
        velocity.x = 0.0f;
    }
    if (std::abs(velocity.z) < 1.0e-4f) {
        velocity.z = 0.0f;
    }
}

void updateEntityChunk(WorldEntities& entities, Entity& entity) {
    entities.updateEntityChunk(entity);
}

} // namespace Rigel::Entity
