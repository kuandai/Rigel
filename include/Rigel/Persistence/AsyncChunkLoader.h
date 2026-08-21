#pragma once

#include "Rigel/Persistence/PersistenceService.h"
#include "Rigel/Persistence/Types.h"
#include "Rigel/Voxel/ChunkCoord.h"
#include "Rigel/Voxel/ChunkLoadRequest.h"
#include "Rigel/Voxel/ChunkTasks.h"
#include "Rigel/Voxel/StreamingDiagnostics.h"
#include "Rigel/Voxel/WorldGenerator.h"

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Rigel::Voxel { class World; class Chunk; }

namespace Rigel::Persistence {

namespace detail {
struct AsyncChunkLoaderTestAccess;
}

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
    ~AsyncChunkLoader();

    Voxel::ChunkLoadRequestResult request(Voxel::ChunkCoord coord);
    bool isPending(Voxel::ChunkCoord coord) const;
    void cancel(Voxel::ChunkCoord coord);
    bool persistChunk(Voxel::ChunkCoord coord);

    Voxel::StreamingWorkCount workCount() const;

    std::vector<Voxel::ChunkLoadCompletion> drainCompletions(size_t budget);

    void setMaxCachedRegions(size_t maxRegions);
    void setMaxInFlightRegions(size_t maxRegions);
    void setPrefetchRadius(int radius);
    void setPrefetchPerRequest(size_t count);
    void setRegionDrainBudget(size_t budget);
    void setLoadQueueLimit(size_t maxPending);

private:
    friend struct detail::AsyncChunkLoaderTestAccess;

    struct RegionKeyHash {
        size_t operator()(const RegionKey& key) const;
    };

    using ChunkLoadRequestId = uint64_t;
    using ChunkRequestMap =
        std::unordered_map<Voxel::ChunkCoord,
                           ChunkLoadRequestId,
                           Voxel::ChunkCoordHash>;

    struct RegionEntry {
        std::shared_ptr<ChunkRegionSnapshot> region;
        std::unordered_set<Voxel::ChunkCoord, Voxel::ChunkCoordHash> present;
        std::unordered_map<Voxel::ChunkCoord,
                           std::vector<const ChunkSnapshot*>,
                           Voxel::ChunkCoordHash> spansByCoord;
    };

    struct RegionResult {
        RegionKey key;
        uint64_t revision = 0;
        RegionEntry entry;
        std::string error;
        bool ok = false;
        bool exists = false;
    };

    struct ChunkPayload {
        Voxel::ChunkCoord coord;
        ChunkLoadRequestId requestId = 0;
        RegionKey regionKey;
        uint64_t regionRevision = 0;
        Voxel::ChunkBuffer blocks;
        uint32_t worldGenVersion = 0;
        std::string error;
        bool empty = false;
        bool cancelled = false;
        bool failed = false;
        bool loadedFromDisk = false;
    };

    void drainRegionCompletions(size_t budget,
                                std::vector<Voxel::ChunkLoadCompletion>& resolved);
    void drainPayloadCompletions(size_t budget,
                                 std::vector<Voxel::ChunkLoadCompletion>& resolved);
    Voxel::ChunkLoadRequestResult queueChunkLoad(
        Voxel::ChunkCoord coord,
        ChunkLoadRequestId requestId);
    void deferChunkLoad(Voxel::ChunkCoord coord,
                        ChunkLoadRequestId requestId);
    void startDeferredChunkLoads(
        std::vector<Voxel::ChunkLoadCompletion>* resolved = nullptr);
    void completeChunkLoad(Voxel::ChunkCoord coord,
                           ChunkLoadRequestId requestId,
                           Voxel::ChunkLoadOutcome outcome,
                           std::vector<Voxel::ChunkLoadCompletion>& resolved);
    void restartChunkLoad(Voxel::ChunkCoord coord,
                          ChunkLoadRequestId requestId,
                          std::vector<Voxel::ChunkLoadCompletion>& resolved);
    void deferRegionLoad(const RegionKey& key);
    void startDeferredRegionLoads();
    bool queueRegionLoad(const RegionKey& key);
    void queuePayloadBuild(const RegionEntry& entry,
                           Voxel::ChunkCoord coord,
                           ChunkLoadRequestId requestId);
    void prefetchNeighbors(const RegionKey& center);
    void touch(const RegionKey& key);
    void evictIfNeeded();
    int estimateRegionSpan() const;
    bool regionMayExist(const RegionKey& key);

    bool applyPayload(const ChunkPayload& payload);
    void invalidateRegion(const RegionKey& key);
    ChunkLoadRequestId nextChunkLoadRequestId();

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

    std::function<void()> m_regionLoadStartCallback;
    std::function<void()> m_payloadBuildStartCallback;
    std::function<void()> m_ioPoolStopStartCallback;
    std::function<void()> m_workerPoolStopStartCallback;

    Voxel::detail::ThreadPool m_ioPool;
    Voxel::detail::ThreadPool m_workerPool;
    Voxel::detail::ConcurrentQueue<RegionResult> m_regionComplete;
    Voxel::detail::ConcurrentQueue<ChunkPayload> m_chunkComplete;

    std::unordered_map<RegionKey, RegionEntry, RegionKeyHash> m_cache;
    std::unordered_set<RegionKey, RegionKeyHash> m_inFlight;
    std::unordered_map<RegionKey,
                       ChunkRequestMap,
                       RegionKeyHash> m_regionPending;
    std::deque<RegionKey> m_deferredRegionLoads;
    std::unordered_set<RegionKey, RegionKeyHash> m_deferredRegionLoadSet;
    ChunkRequestMap m_pendingChunks;
    std::deque<Voxel::ChunkCoord> m_deferredChunkLoads;
    ChunkRequestMap m_deferredChunkRequests;
    std::deque<Voxel::ChunkLoadCompletion> m_resolvedChunks;
    ChunkRequestMap m_payloadInFlight;
    ChunkLoadRequestId m_nextChunkLoadRequestId = 1;
    uint64_t m_requestsStarted = 0;
    std::deque<RegionKey> m_lru;

    struct RegionPresence {
        bool exists = false;
        std::chrono::steady_clock::time_point nextCheck{};
    };
    std::unordered_map<RegionKey, RegionPresence, RegionKeyHash> m_regionPresence;
    std::unordered_map<RegionKey, size_t, RegionKeyHash> m_regionLoadAttempts;
    std::unordered_map<RegionKey, uint64_t, RegionKeyHash> m_regionRevisions;
};

} // namespace Rigel::Persistence
