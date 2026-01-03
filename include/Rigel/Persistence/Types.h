#pragma once

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

    bool operator==(const WorldMetadata& other) const {
        return worldId == other.worldId && displayName == other.displayName;
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

struct ChunkSnapshot {
    ChunkKey key;
    std::vector<uint8_t> payload;

    bool operator==(const ChunkSnapshot& other) const {
        return key == other.key && payload == other.payload;
    }
};

struct ChunkRegionSnapshot {
    RegionKey key;
    std::vector<ChunkSnapshot> chunks;

    bool operator==(const ChunkRegionSnapshot& other) const {
        return key == other.key && chunks == other.chunks;
    }
};

struct EntityRegionSnapshot {
    EntityRegionKey key;
    std::vector<uint8_t> payload;

    bool operator==(const EntityRegionSnapshot& other) const {
        return key == other.key && payload == other.payload;
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
    PersistencePolicies policies{};
    std::shared_ptr<StorageBackend> storage;
    std::shared_ptr<ProviderRegistry> providers;
};

} // namespace Rigel::Persistence
