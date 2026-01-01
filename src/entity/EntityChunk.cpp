#include "Rigel/Entity/EntityChunk.h"

#include <algorithm>

namespace Rigel::Entity {

EntityChunk::EntityChunk(Voxel::ChunkCoord coord)
    : m_coord(coord)
{}

void EntityChunk::addEntity(Entity* entity) {
    if (!entity) {
        return;
    }
    if (std::find(m_entities.begin(), m_entities.end(), entity) == m_entities.end()) {
        m_entities.push_back(entity);
        entity->setCurrentChunk(this);
    }
}

void EntityChunk::removeEntity(Entity* entity) {
    if (!entity) {
        return;
    }
    auto it = std::remove(m_entities.begin(), m_entities.end(), entity);
    if (it != m_entities.end()) {
        m_entities.erase(it, m_entities.end());
    }
}

bool EntityChunk::contains(Entity* entity) const {
    return std::find(m_entities.begin(), m_entities.end(), entity) != m_entities.end();
}

void EntityChunk::forEach(const std::function<void(Entity*)>& fn) const {
    for (Entity* entity : m_entities) {
        fn(entity);
    }
}

} // namespace Rigel::Entity
