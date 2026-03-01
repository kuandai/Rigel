#pragma once

#include "Rigel/Entity/EntityId.h"
#include "Rigel/Voxel/Block.h"
#include "Rigel/Voxel/ChunkCoord.h"

#include <glm/vec3.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Rigel::Persistence {

class StorageBackend;
class ProviderRegistry;

struct WorldMetadata {
    std::string worldId;
    std::string displayName;
    std::string defaultZoneId;

    bool operator==(const WorldMetadata& other) const {
        return worldId == other.worldId &&
            displayName == other.displayName &&
            defaultZoneId == other.defaultZoneId;
    }
};

struct ZoneMetadata {
    std::string zoneId;
    std::string displayName;

    bool operator==(const ZoneMetadata& other) const {
        return zoneId == other.zoneId && displayName == other.displayName;
    }
};

struct ZoneKey {
    std::string zoneId;

    bool operator==(const ZoneKey& other) const {
        return zoneId == other.zoneId;
    }
};

struct RegionKey {
    std::string zoneId;
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;

    bool operator==(const RegionKey& other) const {
        return zoneId == other.zoneId && x == other.x && y == other.y && z == other.z;
    }
};

struct EntityRegionKey {
    std::string zoneId;
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;

    bool operator==(const EntityRegionKey& other) const {
        return zoneId == other.zoneId && x == other.x && y == other.y && z == other.z;
    }
};

struct ChunkKey {
    std::string zoneId;
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;

    bool operator==(const ChunkKey& other) const {
        return zoneId == other.zoneId && x == other.x && y == other.y && z == other.z;
    }
};

struct ChunkSpan {
    int32_t chunkX = 0;
    int32_t chunkY = 0;
    int32_t chunkZ = 0;
    int32_t offsetX = 0;
    int32_t offsetY = 0;
    int32_t offsetZ = 0;
    int32_t sizeX = 0;
    int32_t sizeY = 0;
    int32_t sizeZ = 0;

    bool operator==(const ChunkSpan& other) const {
        return chunkX == other.chunkX &&
            chunkY == other.chunkY &&
            chunkZ == other.chunkZ &&
            offsetX == other.offsetX &&
            offsetY == other.offsetY &&
            offsetZ == other.offsetZ &&
            sizeX == other.sizeX &&
            sizeY == other.sizeY &&
            sizeZ == other.sizeZ;
    }
};

struct ChunkData {
    ChunkSpan span;
    std::vector<Voxel::BlockState> blocks;

    bool operator==(const ChunkData& other) const {
        return span == other.span && blocks == other.blocks;
    }
};

struct ChunkSnapshot {
    ChunkKey key;
    ChunkData data;

    bool operator==(const ChunkSnapshot& other) const {
        return key == other.key && data == other.data;
    }
};

struct ChunkRegionSnapshot {
    RegionKey key;
    std::vector<ChunkSnapshot> chunks;

    bool operator==(const ChunkRegionSnapshot& other) const {
        return key == other.key && chunks == other.chunks;
    }
};

struct EntityPersistedEntity {
    std::string typeId;
    Entity::EntityId id;
    glm::vec3 position{0.0f};
    glm::vec3 velocity{0.0f};
    glm::vec3 viewDirection{0.0f, 0.0f, -1.0f};
    std::string modelId;

    bool operator==(const EntityPersistedEntity& other) const {
        return typeId == other.typeId &&
            id == other.id &&
            position == other.position &&
            velocity == other.velocity &&
            viewDirection == other.viewDirection &&
            modelId == other.modelId;
    }
};

struct EntityPersistedChunk {
    Voxel::ChunkCoord coord;
    std::vector<EntityPersistedEntity> entities;

    bool operator==(const EntityPersistedChunk& other) const {
        return coord == other.coord && entities == other.entities;
    }
};

struct EntityRegionSnapshot {
    EntityRegionKey key;
    std::vector<EntityPersistedChunk> chunks;

    bool operator==(const EntityRegionSnapshot& other) const {
        return key == other.key && chunks == other.chunks;
    }
};

struct WorldSnapshot {
    WorldMetadata metadata;
    std::vector<ZoneMetadata> zones;
};

struct ZoneSnapshot {
    ZoneMetadata metadata;
    std::vector<RegionKey> regions;
    std::vector<EntityRegionKey> entityRegions;
};

enum class SaveScope {
    MetadataOnly,
    ChunksOnly,
    EntitiesOnly,
    All
};

inline bool includesMetadata(SaveScope scope) {
    return scope == SaveScope::MetadataOnly || scope == SaveScope::All;
}

inline bool includesChunks(SaveScope scope) {
    return scope == SaveScope::ChunksOnly || scope == SaveScope::All;
}

inline bool includesEntities(SaveScope scope) {
    return scope == SaveScope::EntitiesOnly || scope == SaveScope::All;
}

enum class UnknownIdPolicy {
    Fail,
    Placeholder,
    Skip
};

enum class UnsupportedFeaturePolicy {
    Fail,
    NoOp,
    Warn
};

struct PersistencePolicies {
    UnknownIdPolicy unknownBlockPolicy = UnknownIdPolicy::Fail;
    UnknownIdPolicy unknownEntityPolicy = UnknownIdPolicy::Fail;
    UnsupportedFeaturePolicy unsupportedFeaturePolicy = UnsupportedFeaturePolicy::Fail;
};

struct PersistenceContext {
    std::string rootPath;
    std::string preferredFormat;
    std::string manifestPath;
    std::string zoneId;
    PersistencePolicies policies{};
    std::shared_ptr<StorageBackend> storage;
    std::shared_ptr<ProviderRegistry> providers;
};

} // namespace Rigel::Persistence
