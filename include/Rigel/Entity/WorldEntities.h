#pragma once

#include "Entity.h"
#include "EntityRegion.h"

#include <unordered_map>
#include <memory>

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
    void tick(float dt);
    void clear();

    size_t size() const { return m_entities.size(); }

    void updateEntityChunk(Entity& entity);

private:
    EntityRegion& getOrCreateRegion(Voxel::ChunkCoord coord);
    EntityChunk& getOrCreateChunk(Voxel::ChunkCoord coord);
    EntityChunk* findChunk(Voxel::ChunkCoord coord) const;
    void removeFromChunk(Entity& entity);

    Voxel::World* m_world = nullptr;
    std::unordered_map<EntityId, std::unique_ptr<Entity>, EntityIdHash> m_entities;
    std::unordered_map<EntityRegionCoord, std::unique_ptr<EntityRegion>, EntityRegionCoordHash> m_regions;
    std::unordered_map<Voxel::ChunkCoord, EntityChunk*, Voxel::ChunkCoordHash> m_chunkIndex;
};

} // namespace Rigel::Entity
