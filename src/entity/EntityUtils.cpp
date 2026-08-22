#include "Rigel/Entity/EntityUtils.h"

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

} // namespace Rigel::Entity
