#include "Rigel/Entity/WorldEntities.h"

#include "Rigel/Voxel/World.h"

#include <algorithm>
#include <stdexcept>
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
    m_pendingDespawns.reserve(m_entities.size() + 1);
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

std::vector<EntityId> WorldEntities::sortedIds() const {
    std::vector<EntityId> result;
    result.reserve(m_entities.size());
    for (const auto& [id, _] : m_entities) result.push_back(id);
    std::sort(result.begin(), result.end());
    return result;
}

void WorldEntities::prepareTick() {
    if (m_isTicking) {
        throw std::logic_error("cannot prepare entity tick during update");
    }
    m_tickIds.clear();
    m_tickIds.reserve(m_entities.size());
    m_pendingDespawns.reserve(m_entities.size());
    for (const auto& [id, _] : m_entities) {
        m_tickIds.push_back(id);
    }
    std::sort(m_tickIds.begin(), m_tickIds.end());
    m_tickPrepared = true;
}

void WorldEntities::tickPrepared(float dt) {
    if (!m_world || !m_tickPrepared) {
        return;
    }
    m_tickPrepared = false;

    auto finishTick = [&] {
        m_isTicking = false;
        if (!m_pendingDespawns.empty()) {
            std::vector<EntityId> pending = std::move(m_pendingDespawns);
            m_pendingDespawns.clear();
            for (const EntityId& id : pending) {
                despawn(id);
            }
        }
    };

    m_isTicking = true;
    try {
        for (const EntityId& id : m_tickIds) {
            auto it = m_entities.find(id);
            if (it != m_entities.end()) {
                it->second->update(*m_world, dt);
            }
        }
    } catch (...) {
        finishTick();
        throw;
    }
    finishTick();
}

void WorldEntities::tick(float dt) {
    if (!m_world) return;
    prepareTick();
    tickPrepared(dt);
}

} // namespace Rigel::Entity
