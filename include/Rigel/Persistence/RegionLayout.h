#pragma once

#include "Rigel/Persistence/Types.h"
#include "Rigel/Voxel/ChunkCoord.h"

#include <string>
#include <vector>

namespace Rigel::Persistence {

class RegionLayout {
public:
    virtual ~RegionLayout() = default;

    virtual RegionKey regionForChunk(const std::string& zoneId, Voxel::ChunkCoord coord) const = 0;
    virtual std::vector<ChunkKey> storageKeysForChunk(const std::string& zoneId,
                                                      Voxel::ChunkCoord coord) const = 0;
    virtual ChunkSpan spanForStorageKey(const ChunkKey& key) const = 0;
    virtual std::vector<Voxel::ChunkCoord> chunksForRegion(const RegionKey& key) const = 0;
};

} // namespace Rigel::Persistence
