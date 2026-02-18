#pragma once

#include "Rigel/Persistence/PersistenceService.h"
#include "Rigel/Persistence/Types.h"
#include "Rigel/Voxel/Chunk.h"
#include "Rigel/Voxel/ChunkCoord.h"
#include "Rigel/Voxel/VoxelLod/VoxelSource.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace Rigel::Voxel {

// Worker-safe voxel source backed by persistence region reads.
//
// This source is read-only and does not mutate World/ChunkManager. Missing
// regions/chunks report BrickSampleStatus::Miss so the caller can fall back to
// generator sampling.
class PersistenceSource final : public IVoxelSource {
public:
    PersistenceSource(Persistence::PersistenceService* service,
                      Persistence::PersistenceContext context,
                      std::string zoneId = "rigel:default");

    void setCacheLimits(size_t maxCachedRegions, size_t maxCachedChunks);

    BrickSampleStatus sampleBrick(const BrickSampleDesc& desc,
                                  std::span<VoxelId> out,
                                  const std::atomic_bool* cancel = nullptr) const override;

private:
    struct CachedRegion {
        std::shared_ptr<Persistence::ChunkRegionSnapshot> region;
        uint64_t lastAccess = 0;
    };

    struct CachedChunk {
        bool hit = false;
        std::shared_ptr<std::array<BlockState, Chunk::VOLUME>> blocks;
        uint64_t lastAccess = 0;
    };

    static std::string regionCacheKey(const Persistence::RegionKey& key);
    bool tryLoadChunk(ChunkCoord coord,
                      std::array<BlockState, Chunk::VOLUME>& out,
                      const std::atomic_bool* cancel) const;
    bool decodeChunkFromRegion(const Persistence::ChunkRegionSnapshot& region,
                               const std::vector<Persistence::ChunkKey>& storageKeys,
                               ChunkCoord coord,
                               std::array<BlockState, Chunk::VOLUME>& out) const;
    static bool applySpanToChunkArray(const Persistence::ChunkData& data,
                                      std::array<BlockState, Chunk::VOLUME>& out);
    void evictCachesLocked() const;

    Persistence::PersistenceService* m_service = nullptr;
    Persistence::PersistenceContext m_context;
    std::string m_zoneId;

    mutable std::mutex m_cacheMutex;
    mutable std::unordered_map<std::string, CachedRegion> m_regionCache;
    mutable std::unordered_map<ChunkCoord, CachedChunk, ChunkCoordHash> m_chunkCache;
    mutable uint64_t m_accessClock = 0;

    size_t m_maxCachedRegions = 64;
    size_t m_maxCachedChunks = 512;
};

} // namespace Rigel::Voxel
