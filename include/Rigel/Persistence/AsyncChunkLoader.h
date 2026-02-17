#pragma once

#include "Rigel/Persistence/PersistenceService.h"
#include "Rigel/Persistence/Types.h"
#include "Rigel/Voxel/ChunkCoord.h"
#include "Rigel/Voxel/ChunkTasks.h"
#include "Rigel/Voxel/WorldGenerator.h"

#include <atomic>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace Rigel::Voxel { class World; class Chunk; }

namespace Rigel::Persistence {

class AsyncChunkLoader {
public:
    AsyncChunkLoader(PersistenceService& service,
                     PersistenceContext context,
                     Voxel::World& world,
                     uint32_t worldGenVersion,
                     size_t ioThreads,
                     size_t workerThreads,
                     int viewDistanceChunks,
                     std::shared_ptr<Voxel::WorldGenerator> generator);

    bool request(Voxel::ChunkCoord coord);
    bool isPending(Voxel::ChunkCoord coord) const;
    void cancel(Voxel::ChunkCoord coord);

    void drainCompletions(size_t budget);

    void setMaxCachedRegions(size_t maxRegions);
    void setMaxInFlightRegions(size_t maxRegions);
    void setPrefetchRadius(int radius);
    void setPrefetchPerRequest(size_t count);
    void setRegionDrainBudget(size_t budget);
    void setLoadQueueLimit(size_t maxPending);

private:
    struct RegionKeyHash {
        size_t operator()(const RegionKey& key) const;
    };

    struct RegionEntry {
        std::shared_ptr<ChunkRegionSnapshot> region;
        std::unordered_set<Voxel::ChunkCoord, Voxel::ChunkCoordHash> present;
        std::unordered_map<Voxel::ChunkCoord,
                           std::vector<const ChunkSnapshot*>,
                           Voxel::ChunkCoordHash> spansByCoord;
    };

    struct RegionResult {
        RegionKey key;
        RegionEntry entry;
        bool ok = false;
        bool exists = false;
    };

    struct ChunkPayload {
        Voxel::ChunkCoord coord;
        Voxel::ChunkBuffer blocks;
        uint32_t worldGenVersion = 0;
        bool empty = false;
        bool cancelled = false;
        bool loadedFromDisk = false;
    };

    void drainRegionCompletions(size_t budget);
    void drainPayloadCompletions(size_t budget);
    bool queueRegionLoad(const RegionKey& key);
    void queuePayloadBuild(const RegionEntry& entry, Voxel::ChunkCoord coord);
    void prefetchNeighbors(const RegionKey& center);
    void touch(const RegionKey& key);
    void evictIfNeeded();
    int estimateRegionSpan() const;
    bool regionMayExist(const RegionKey& key);

    bool applyPayload(const ChunkPayload& payload);

    PersistenceService* m_service = nullptr;
    PersistenceContext m_context;
    std::unique_ptr<PersistenceFormat> m_format;
    Voxel::World* m_world = nullptr;
    uint32_t m_worldGenVersion = 0;
    std::string m_zoneId = "rigel:default";
    size_t m_maxCachedRegions = 8;
    size_t m_maxInFlightRegions = 8;
    size_t m_loadQueueLimit = 0;
    int m_prefetchRadius = 1;
    size_t m_prefetchPerRequest = 12;
    size_t m_regionDrainBudget = 32;

    std::shared_ptr<Voxel::WorldGenerator> m_generator;

    Voxel::detail::ThreadPool m_ioPool;
    Voxel::detail::ThreadPool m_workerPool;
    Voxel::detail::ConcurrentQueue<RegionResult> m_regionComplete;
    Voxel::detail::ConcurrentQueue<ChunkPayload> m_chunkComplete;

    std::unordered_map<RegionKey, RegionEntry, RegionKeyHash> m_cache;
    std::unordered_set<RegionKey, RegionKeyHash> m_inFlight;
    std::unordered_map<RegionKey,
                       std::unordered_set<Voxel::ChunkCoord, Voxel::ChunkCoordHash>,
                       RegionKeyHash> m_regionPending;
    std::unordered_set<Voxel::ChunkCoord, Voxel::ChunkCoordHash> m_pendingChunks;
    std::unordered_set<Voxel::ChunkCoord, Voxel::ChunkCoordHash> m_payloadInFlight;
    std::deque<RegionKey> m_lru;

    struct RegionPresence {
        bool exists = false;
        std::chrono::steady_clock::time_point nextCheck{};
    };
    std::unordered_map<RegionKey, RegionPresence, RegionKeyHash> m_regionPresence;
};

} // namespace Rigel::Persistence
