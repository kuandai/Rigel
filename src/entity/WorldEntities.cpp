#include "Rigel/Entity/WorldEntities.h"

#include "Rigel/Voxel/World.h"

#include <vector>

namespace Rigel::Entity {

void WorldEntities::bind(Voxel::World* world) {
    m_world = world;
}

EntityId WorldEntities::spawn(std::unique_ptr<Entity> entity) {
    if (!entity) {
        return EntityId::Null();
    }
    EntityId id = entity->id();
    if (id.isNull()) {
        id = EntityId::New();
        entity->setId(id);
    }
    auto [it, inserted] = m_entities.emplace(id, std::move(entity));
    if (!inserted) {
        return EntityId::Null();
    }
    return id;
}

bool WorldEntities::despawn(const EntityId& id) {
    if (m_isTicking) {
        if (m_entities.find(id) == m_entities.end()) {
            return false;
        }
        m_pendingDespawns.push_back(id);
        return true;
    }
    auto it = m_entities.find(id);
    if (it == m_entities.end()) {
        return false;
    }
    m_entities.erase(it);
    return true;
}

Entity* WorldEntities::get(const EntityId& id) {
    auto it = m_entities.find(id);
    if (it == m_entities.end()) {
        return nullptr;
    }
    return it->second.get();
}

const Entity* WorldEntities::get(const EntityId& id) const {
    auto it = m_entities.find(id);
    if (it == m_entities.end()) {
        return nullptr;
    }
    return it->second.get();
}

void WorldEntities::forEach(const std::function<void(Entity&)>& fn) {
    for (auto& [_, entity] : m_entities) {
        fn(*entity);
    }
}

void WorldEntities::forEach(const std::function<void(const Entity&)>& fn) const {
    for (const auto& [_, entity] : m_entities) {
        fn(*entity);
    }
}

void WorldEntities::tick(float dt) {
    if (!m_world) {
        return;
    }
    m_isTicking = true;
    std::vector<EntityId> ids;
    ids.reserve(m_entities.size());
    for (auto& [id, _] : m_entities) {
        ids.push_back(id);
    }
    for (const EntityId& id : ids) {
        auto it = m_entities.find(id);
        if (it == m_entities.end()) {
            continue;
        }
        Entity* entity = it->second.get();
        entity->update(*m_world, dt);
    }
    m_isTicking = false;

    if (!m_pendingDespawns.empty()) {
        std::vector<EntityId> pending = std::move(m_pendingDespawns);
        m_pendingDespawns.clear();
        for (const EntityId& id : pending) {
            despawn(id);
        }
    }
}

} // namespace Rigel::Entity
