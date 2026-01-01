#pragma once

#include "Entity.h"

#include <Rigel/Voxel/ChunkCoord.h>

#include <functional>
#include <vector>

namespace Rigel::Entity {

class EntityRegion;

class EntityChunk {
public:
    explicit EntityChunk(Voxel::ChunkCoord coord);

    Voxel::ChunkCoord coord() const { return m_coord; }
    EntityRegion* region() const { return m_region; }
    void setRegion(EntityRegion* region) { m_region = region; }

    void addEntity(Entity* entity);
    void removeEntity(Entity* entity);
    bool contains(Entity* entity) const;
    bool hasEntities() const { return !m_entities.empty(); }

    void forEach(const std::function<void(Entity*)>& fn) const;

private:
    Voxel::ChunkCoord m_coord{};
    EntityRegion* m_region = nullptr;
    std::vector<Entity*> m_entities;
};

} // namespace Rigel::Entity
