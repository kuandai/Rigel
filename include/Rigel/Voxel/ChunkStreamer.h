#pragma once

#include "ChunkTasks.h"
#include "ChunkCache.h"
#include "ChunkBenchmark.h"
#include "ChunkLoadRequest.h"
#include "ChunkManager.h"
#include "ChunkMesh.h"
#include "StreamingDiagnostics.h"
#include "StreamingConfig.h"
#include "TextureAtlas.h"
#include "WorldMeshStore.h"
#include "WorldGenerator.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <queue>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <glm/vec3.hpp>
#include <memory>

namespace Rigel::Voxel {

namespace detail {
struct ChunkStreamerTestAccess;
}

class ChunkStreamer {
public:
    // Cumulative counters are retained for the lifetime of the streamer.
    // The last-update fields are replaced by each call to update().
    struct WorkMetrics {
        uint64_t generationJobsStarted = 0;
        uint64_t generationJobsFailed = 0;
        // Distinct coordinates whose load request transitioned to pending.
        uint64_t chunkLoadRequestsStarted = 0;
        uint64_t meshJobsStarted = 0;
        // Results observed by the completion drain.
        uint64_t meshJobsCompleted = 0;
        uint64_t meshJobsAccepted = 0;
        uint64_t meshJobsRejectedStale = 0;
        uint64_t meshJobsFailed = 0;
        // Dirty revisions requiring new mesh work, including in-flight replacements.
        uint64_t meshInvalidations = 0;
        uint64_t meshRequestsCoalesced = 0;
        // Candidate coordinates tested while rebuilding the desired set.
        uint64_t desiredBuildCoordinatesInspected = 0;
        // Pending load/generation and dirty-mesh entries visited by the scheduler.
        uint64_t schedulerCoordinatesInspected = 0;
        // Resident cache entries considered for capacity eviction.
        uint64_t cacheEvictionCoordinatesInspected = 0;
        uint64_t lastUpdateDesiredBuildCoordinatesInspected = 0;
        uint64_t lastUpdateSchedulerCoordinatesInspected = 0;
        uint64_t lastUpdateCacheEvictionCoordinatesInspected = 0;
    };

    enum class DebugState : uint8_t {
        QueuedGen,
        LoadedFromDisk,
        ReadyData,
        QueuedMesh,
        ReadyMesh,
        GenerationFailed,
        MeshFailed
    };

    struct DebugChunkState {
        ChunkCoord coord;
        DebugState state;
    };

    using ChunkLoadCallback = std::function<ChunkLoadRequestResult(ChunkCoord)>;
    using ChunkPendingCallback = std::function<bool(ChunkCoord)>;
    using ChunkLoadDrainCallback =
        std::function<std::vector<ChunkLoadCompletion>(size_t)>;
    using ChunkLoadCancelCallback = std::function<void(ChunkCoord)>;
    using ChunkLoadWorkCallback = std::function<StreamingWorkCount()>;
    using ChunkEvictionCallback = std::function<bool(ChunkCoord)>;

    ChunkStreamer() = default;
    ~ChunkStreamer();

    void setConfig(const StreamingConfig& config);
    void bind(ChunkManager* manager,
              WorldMeshStore* meshStore,
              BlockRegistry* registry,
              TextureAtlas* atlas,
              std::shared_ptr<WorldGenerator> generator);
    void setBenchmark(ChunkBenchmarkStats* stats);
    void setChunkLoader(ChunkLoadCallback loader);
    void setChunkPendingCallback(ChunkPendingCallback pending);
    void setChunkLoadDrain(ChunkLoadDrainCallback drain);
    void setChunkLoadCancel(ChunkLoadCancelCallback cancel);
    void setChunkLoadWorkCallback(ChunkLoadWorkCallback work);
    void setChunkEvictionCallback(ChunkEvictionCallback evict);
    void markSpawnDiscoveryComplete();
    void prioritizeMesh(ChunkCoord coord);

    void update(const glm::vec3& cameraPos);
    void processCompletions();
    void reset();
    void getDebugStates(std::vector<DebugChunkState>& out) const;
    int viewDistanceChunks() const { return m_config.viewDistanceChunks; }
    const WorkMetrics& workMetrics() const { return m_workMetrics; }
    const StreamingDiagnosticSnapshot& diagnostics() const { return m_diagnostics; }

private:
    friend struct detail::ChunkStreamerTestAccess;

    static constexpr int kPaddedSize = Chunk::SIZE + 2;
    static constexpr int kPaddedVolume = kPaddedSize * kPaddedSize * kPaddedSize;

    enum class ChunkState : uint8_t {
        Missing,
        QueuedGen,
        ReadyData,
        QueuedMesh,
        ReadyMesh,
        GenerationFailed,
        MeshFailed
    };

    struct GenResult {
        ChunkCoord coord;
        uint64_t lifecycle = 0;
        std::array<BlockState, Chunk::VOLUME> blocks{};
        uint32_t worldGenVersion = 0;
        double seconds = 0.0;
        std::string error;
        bool cancelled = false;
        bool failed = false;
        std::shared_ptr<std::atomic_bool> cancelToken;
    };

    struct MeshTask {
        ChunkCoord coord;
        uint64_t requestId = 0;
        uint64_t chunkInstanceId = 0;
        uint32_t revision = 0;
        std::array<BlockState, Chunk::VOLUME> blocks{};
        std::array<BlockState, kPaddedVolume> paddedBlocks{};
    };

    struct MeshResult {
        ChunkCoord coord;
        uint64_t requestId = 0;
        uint64_t chunkInstanceId = 0;
        uint32_t revision = 0;
        ChunkMesh mesh;
        double seconds = 0.0;
        std::string error;
        bool empty = false;
        bool failed = false;
    };

    enum class MeshRequestKind : uint8_t {
        Missing,
        Dirty
    };

    struct MeshInFlight {
        MeshRequestKind kind = MeshRequestKind::Missing;
        uint64_t requestId = 0;
        uint32_t observedRevision = 0;
        bool prioritized = false;
        bool obsolete = false;
    };

    struct PendingDirtyMesh {
        size_t priority = 0;
        ChunkCoord coord;
        bool prioritized = false;
    };

    struct PendingDirtyMeshGreater {
        bool operator()(const PendingDirtyMesh& lhs, const PendingDirtyMesh& rhs) const {
            if (lhs.prioritized != rhs.prioritized) {
                return lhs.prioritized < rhs.prioritized;
            }
            return lhs.priority > rhs.priority;
        }
    };

    StreamingConfig m_config;
    ChunkManager* m_chunkManager = nullptr;
    WorldMeshStore* m_meshStore = nullptr;
    BlockRegistry* m_registry = nullptr;
    TextureAtlas* m_atlas = nullptr;
    std::shared_ptr<WorldGenerator> m_generator;
    ChunkCache m_cache;
    ChunkBenchmarkStats* m_benchmark = nullptr;
    ChunkLoadCallback m_chunkLoader;
    ChunkPendingCallback m_chunkPending;
    ChunkLoadDrainCallback m_chunkLoadDrain;
    ChunkLoadCancelCallback m_chunkLoadCancel;
    ChunkLoadWorkCallback m_chunkLoadWork;
    ChunkEvictionCallback m_chunkEviction;

    std::unique_ptr<detail::ThreadPool> m_genPool;
    std::unique_ptr<detail::ThreadPool> m_meshPool;
    detail::ConcurrentQueue<GenResult> m_genComplete;
    detail::ConcurrentQueue<MeshResult> m_meshComplete;
    std::unordered_map<ChunkCoord, ChunkState, ChunkCoordHash> m_states;
    std::unordered_set<ChunkCoord, ChunkCoordHash> m_loadPending;
    std::unordered_map<ChunkCoord, std::shared_ptr<std::atomic_bool>, ChunkCoordHash> m_genCancel;
    std::unordered_map<ChunkCoord, MeshInFlight, ChunkCoordHash> m_meshInFlight;
    std::unordered_map<ChunkCoord, uint32_t, ChunkCoordHash> m_countedMeshRetryRevisions;
    std::deque<ChunkCoord> m_loadGenQueue;
    std::unordered_set<ChunkCoord, ChunkCoordHash> m_loadGenQueued;
    std::deque<ChunkCoord> m_generationCapacityWait;
    std::unordered_set<ChunkCoord, ChunkCoordHash> m_generationCapacityWaiting;
    std::deque<ChunkCoord> m_missingMeshCapacityWait;
    std::unordered_set<ChunkCoord, ChunkCoordHash> m_missingMeshCapacityWaiting;
    std::unordered_set<ChunkCoord, ChunkCoordHash> m_meshDependencyWaiting;
    std::vector<ChunkCoord> m_desired;
    std::unordered_set<ChunkCoord, ChunkCoordHash> m_desiredSet;
    std::unordered_map<ChunkCoord, size_t, ChunkCoordHash> m_desiredPriority;
    std::priority_queue<PendingDirtyMesh,
                        std::vector<PendingDirtyMesh>,
                        PendingDirtyMeshGreater> m_dirtyMeshQueue;
    std::unordered_set<ChunkCoord, ChunkCoordHash> m_dirtyMeshQueued;
    std::unordered_set<ChunkCoord, ChunkCoordHash> m_priorityMeshRequests;
    std::unordered_map<ChunkCoord, uint64_t, ChunkCoordHash> m_evictionRetryAfter;
    std::unordered_set<ChunkCoord, ChunkCoordHash> m_versionReplacementWaiting;
    // Jobs whose completion has not yet been observed, including cancelled work
    // from an earlier generation lifecycle.
    size_t m_inFlightGen = 0;
    size_t m_inFlightMesh = 0;
    size_t m_inFlightMeshMissing = 0;
    size_t m_inFlightMeshDirty = 0;
    uint64_t m_nextMeshRequestId = 1;
    MeshRequestKind m_nextSingleSlotMeshKind = MeshRequestKind::Missing;
    std::optional<ChunkCoord> m_lastCenter;
    int m_lastViewDistance = -1;
    int m_lastUnloadDistance = -1;
    uint32_t m_lastWorldGenVersion = 0;
    bool m_spawnDiscoveryComplete = false;
    bool m_initialStreamingBegun = false;
    bool m_workObservedThisUpdate = false;
    bool m_workStartedThisUpdate = false;
    uint64_t m_streamingUpdateSequence = 0;
    uint64_t m_lifecycleUpdateSequence = 0;
    uint64_t m_generationLifecycle = 1;
    uint64_t m_nextEvictionRetrySequence = 0;
    std::function<void()> m_generationStartCallback;
    std::function<void()> m_meshBuildStartCallback;
    WorkMetrics m_workMetrics;
    StreamingDiagnosticSnapshot m_diagnostics;

    void applyGenCompletions(size_t budget);
    void applyMeshCompletions(size_t budget);
    void cancelPendingLoad(ChunkCoord coord);
    void queueLoadGen(ChunkCoord coord);
    void waitForGenerationCapacity(ChunkCoord coord);
    void waitForMissingMeshCapacity(ChunkCoord coord);
    void waitForMeshDependencies(ChunkCoord coord);
    void wakeGenerationCapacityWaiter();
    void wakeMissingMeshCapacityWaiter();
    void queueLoadedNeighbors(ChunkCoord coord);
    void queueDirtyMesh(ChunkCoord coord, bool prioritize = false);
    void reprioritizeDirtyMeshes();
    void enqueueGeneration(ChunkCoord coord);
    void enqueueMesh(ChunkCoord coord,
                     Chunk& chunk,
                     MeshRequestKind kind,
                     bool prioritized = false);
    void ensureThreadPool();
    bool hasAllNeighborsLoaded(ChunkCoord coord) const;
    StreamingDiagnosticSnapshot collectDiagnostics() const;
    void refreshDiagnostics(bool advanceWindow);
    bool evictChunk(ChunkCoord coord);
    void deferEviction(ChunkCoord coord);
    void retryDeferredEvictions(ChunkCoord center, int unloadRadiusSq);

    ChunkCoord cameraToChunk(const glm::vec3& cameraPos) const;
};

} // namespace Rigel::Voxel
