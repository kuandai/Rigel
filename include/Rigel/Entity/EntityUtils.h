#pragma once

#include "Entity.h"
namespace Rigel::Entity {
class WorldEntities;
}

namespace Rigel::Entity {

void applyFriction(float friction, glm::vec3& velocity);
void updateEntityChunk(WorldEntities& entities, Entity& entity);

} // namespace Rigel::Entity
