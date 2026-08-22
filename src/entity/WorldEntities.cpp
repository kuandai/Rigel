#include "Rigel/Entity/WorldEntities.h"

#include "Rigel/Entity/EntityUtils.h"
#include "Rigel/Voxel/World.h"

#include <cmath>
#include <vector>

namespace Rigel::Entity {

WorldEntities::~WorldEntities() {
    clear();
}

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
    entity->setCurrentChunk(nullptr);
    auto [it, inserted] = m_entities.emplace(id, std::move(entity));
    if (!inserted) {
        return EntityId::Null();
    }
    updateEntityChunk(*it->second);
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
    removeFromChunk(*it->second);
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
        if (m_entities.find(id) != m_entities.end()) {
            updateEntityChunk(*entity);
        }
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

void WorldEntities::clear() {
    for (auto& [_, entity] : m_entities) {
        removeFromChunk(*entity);
    }
    m_entities.clear();
    m_regions.clear();
    m_chunkIndex.clear();
    m_pendingDespawns.clear();
}

void WorldEntities::updateEntityChunk(Entity& entity) {
    if (!m_world) {
        return;
    }
    const glm::vec3& pos = entity.position();
    Voxel::ChunkCoord coord = Voxel::worldToChunk(
        static_cast<int>(std::floor(pos.x)),
        static_cast<int>(std::floor(pos.y)),
        static_cast<int>(std::floor(pos.z))
    );

    EntityChunk* current = entity.currentChunk();
    if (current && current->coord() == coord) {
        return;
    }

    if (current) {
        removeFromChunk(entity);
    }
    EntityChunk& target = getOrCreateChunk(coord);
    target.addEntity(&entity);
}

EntityRegion& WorldEntities::getOrCreateRegion(Voxel::ChunkCoord coord) {
    EntityRegionCoord regionCoord = chunkToRegion(coord);
    auto it = m_regions.find(regionCoord);
    if (it != m_regions.end()) {
        return *it->second;
    }
    auto region = std::make_unique<EntityRegion>(regionCoord);
    auto [inserted, _] = m_regions.emplace(regionCoord, std::move(region));
    return *inserted->second;
}

EntityChunk& WorldEntities::getOrCreateChunk(Voxel::ChunkCoord coord) {
    auto it = m_chunkIndex.find(coord);
    if (it != m_chunkIndex.end()) {
        return *it->second;
    }
    EntityRegion& region = getOrCreateRegion(coord);
    EntityChunk& chunk = region.getOrActivateChunk(coord);
    m_chunkIndex[coord] = &chunk;
    return chunk;
}

EntityChunk* WorldEntities::findChunk(Voxel::ChunkCoord coord) const {
    auto it = m_chunkIndex.find(coord);
    if (it == m_chunkIndex.end()) {
        return nullptr;
    }
    return it->second;
}

void WorldEntities::removeFromChunk(Entity& entity) {
    EntityChunk* chunk = entity.currentChunk();
    if (!chunk) {
        return;
    }

    const Voxel::ChunkCoord chunkCoord = chunk->coord();
    EntityRegion* region = chunk->region();
    chunk->removeEntity(&entity);
    if (chunk->hasEntities()) {
        return;
    }

    auto chunkIt = m_chunkIndex.find(chunkCoord);
    if (chunkIt != m_chunkIndex.end() && chunkIt->second == chunk) {
        m_chunkIndex.erase(chunkIt);
    }

    if (!region) {
        return;
    }
    const EntityRegionCoord regionCoord = region->coord();
    if (!region->removeChunkIfEmpty(chunkCoord) || !region->isEmpty()) {
        return;
    }

    auto regionIt = m_regions.find(regionCoord);
    if (regionIt != m_regions.end() && regionIt->second.get() == region) {
        m_regions.erase(regionIt);
    }
}

} // namespace Rigel::Entity
