#pragma once

#include "Rigel/Persistence/FormatRegistry.h"

namespace Rigel::Persistence {

class PersistenceService {
public:
    explicit PersistenceService(FormatRegistry& registry);

    void saveWorld(const WorldSnapshot& snapshot, SaveScope scope, const PersistenceContext& context);
    WorldMetadata loadWorldMetadata(const PersistenceContext& context);

    void saveZone(const ZoneSnapshot& snapshot, SaveScope scope, const PersistenceContext& context);
    ZoneMetadata loadZoneMetadata(const ZoneKey& key, const PersistenceContext& context);

    void saveRegion(const ChunkRegionSnapshot& region, const PersistenceContext& context);
    ChunkRegionSnapshot loadRegion(const RegionKey& key, const PersistenceContext& context);

    void saveEntities(const EntityRegionSnapshot& region, const PersistenceContext& context);
    EntityRegionSnapshot loadEntities(const EntityRegionKey& key, const PersistenceContext& context);

private:
    FormatRegistry& m_registry;

    std::unique_ptr<PersistenceFormat> resolve(const PersistenceContext& context) const;
    void handleUnsupportedFeature(const PersistenceContext& context, const std::string& message) const;
};

} // namespace Rigel::Persistence
