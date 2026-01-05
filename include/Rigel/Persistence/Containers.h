#pragma once

#include "Rigel/Persistence/Types.h"

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace Rigel::Persistence {

class ChunkContainer {
public:
    virtual ~ChunkContainer() = default;

    virtual void saveRegion(const ChunkRegionSnapshot& region) = 0;
    virtual ChunkRegionSnapshot loadRegion(const RegionKey& key) = 0;
    virtual std::vector<RegionKey> listRegions(const std::string& zoneId) = 0;

    virtual bool supportsChunkIO() const { return false; }
    virtual void saveChunk(const ChunkSnapshot&) {
        throw std::runtime_error("Chunk-level IO not supported by this container");
    }
    virtual ChunkSnapshot loadChunk(const ChunkKey&) {
        throw std::runtime_error("Chunk-level IO not supported by this container");
    }
};

class EntityContainer {
public:
    virtual ~EntityContainer() = default;

    virtual void saveRegion(const EntityRegionSnapshot& region) = 0;
    virtual EntityRegionSnapshot loadRegion(const EntityRegionKey& key) = 0;
    virtual std::vector<EntityRegionKey> listRegions(const std::string& zoneId) = 0;
};

} // namespace Rigel::Persistence
