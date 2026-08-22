#pragma once

#include "Rigel/Persistence/Types.h"

#include <functional>
#include <string>
#include <vector>

namespace Rigel::Persistence {

using EntityRegionVisitor = std::function<bool(const EntityRegionKey&)>;

class ChunkContainer {
public:
    virtual ~ChunkContainer() = default;

    virtual void saveRegion(const ChunkRegionSnapshot& region) = 0;
    virtual ChunkRegionSnapshot loadRegion(const RegionKey& key) = 0;
    virtual std::vector<RegionKey> listRegions(const std::string& zoneId) = 0;
    virtual bool regionExists(const RegionKey& key) {
        for (const auto& existing : listRegions(key.zoneId)) {
            if (existing == key) {
                return true;
            }
        }
        return false;
    }
};

class EntityContainer {
public:
    virtual ~EntityContainer() = default;

    virtual void saveRegion(const EntityRegionSnapshot& region) = 0;
    virtual void removeRegion(const EntityRegionKey& key) = 0;
    virtual EntityRegionSnapshot loadRegion(const EntityRegionKey& key) = 0;
    virtual void forEachRegion(const std::string& zoneId,
                               const EntityRegionVisitor& visitor) = 0;
    virtual std::vector<EntityRegionKey> listRegions(
        const std::string& zoneId) {
        std::vector<EntityRegionKey> regions;
        forEachRegion(zoneId, [&](const EntityRegionKey& key) {
            regions.push_back(key);
            return true;
        });
        return regions;
    }
};

} // namespace Rigel::Persistence
