#pragma once

#include "Entity.h"

#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

namespace Rigel::Voxel {
class World;
}

namespace Rigel::Entity {

class WorldEntities {
public:
    void bind(Voxel::World* world);

    EntityId spawn(std::unique_ptr<Entity> entity);
    bool despawn(const EntityId& id);
    Entity* get(const EntityId& id);
    const Entity* get(const EntityId& id) const;

    void forEach(const std::function<void(Entity&)>& fn);
    void forEach(const std::function<void(const Entity&)>& fn) const;
    void tick(float dt);

    size_t size() const { return m_entities.size(); }

private:
    Voxel::World* m_world = nullptr;
    std::unordered_map<EntityId, std::unique_ptr<Entity>, EntityIdHash> m_entities;
    std::vector<EntityId> m_pendingDespawns;
    bool m_isTicking = false;
};

} // namespace Rigel::Entity
