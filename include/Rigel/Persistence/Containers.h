#pragma once

#include "Rigel/Persistence/Types.h"

#include <string>
#include <vector>

namespace Rigel::Persistence {

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
    virtual std::vector<EntityRegionKey> listRegions(const std::string& zoneId) = 0;
};

} // namespace Rigel::Persistence
