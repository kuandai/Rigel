#pragma once

#include "EntityChunk.h"

#include <Rigel/Voxel/ChunkCoord.h>

#include <unordered_map>
#include <memory>
#include <Rigel/Util/SpatialHash.h>

namespace Rigel::Entity {

inline constexpr int EntityRegionChunkSpan = 16;

struct EntityRegionCoord {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;

    bool operator==(const EntityRegionCoord&) const = default;
};

struct EntityRegionCoordHash {
    size_t operator()(const EntityRegionCoord& coord) const noexcept {
        return Rigel::Util::spatialHash3D(coord.x, coord.y, coord.z);
    }
};

EntityRegionCoord chunkToRegion(Voxel::ChunkCoord coord);

class EntityRegion {
public:
    explicit EntityRegion(EntityRegionCoord coord);

    EntityRegionCoord coord() const { return m_coord; }

    EntityChunk* findActiveChunk(Voxel::ChunkCoord coord);
    EntityChunk& getOrActivateChunk(Voxel::ChunkCoord coord);
    void deactivateChunk(Voxel::ChunkCoord coord);
    bool isEmpty() const;

private:
    EntityRegionCoord m_coord{};
    std::unordered_map<Voxel::ChunkCoord, std::unique_ptr<EntityChunk>, Voxel::ChunkCoordHash>
        m_activeChunks;
    std::unordered_map<Voxel::ChunkCoord, std::unique_ptr<EntityChunk>, Voxel::ChunkCoordHash>
        m_inactiveChunks;
};

} // namespace Rigel::Entity
