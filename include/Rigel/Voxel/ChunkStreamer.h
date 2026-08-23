#pragma once

#include "ChunkTasks.h"
#include "ChunkVisibilityTrace.h"
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
#include <map>
#include <optional>
#include <queue>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <glm/vec3.hpp>
#include <memory>

namespace Rigel::Voxel {

class WorldView;

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
        // Resident chunks considered for distance eviction.
        uint64_t residentEvictionCoordinatesInspected = 0;
        // Deferred evictions reconsidered after camera or configuration changes.
        uint64_t deferredEvictionCoordinatesInspected = 0;
        uint64_t lastUpdateDesiredBuildCoordinatesInspected = 0;
        uint64_t lastUpdateSchedulerCoordinatesInspected = 0;
        uint64_t lastUpdateCacheEvictionCoordinatesInspected = 0;
        uint64_t lastUpdateResidentEvictionCoordinatesInspected = 0;
        uint64_t lastUpdateDeferredEvictionCoordinatesInspected = 0;
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

    using ChunkLoadCallback =
        std::function<ChunkLoadRequestResult(ChunkLoadRequest)>;
    using ChunkPendingCallback = std::function<bool(ChunkCoord)>;
    using ChunkLoadDrainCallback =
        std::function<std::vector<ChunkLoadCompletion>(size_t)>;
    using ChunkLoadCancelCallback = std::function<void(ChunkCoord)>;
    using ChunkLoadWorkCallback = std::function<StreamingWorkCount()>;
    using ChunkEvictionCallback = std::function<bool(ChunkCoord)>;

    ChunkStreamer(ChunkManager& manager,
                  WorldMeshStore& meshStore,
                  BlockRegistry& registry,
                  TextureAtlas* atlas,
                  std::shared_ptr<const WorldGenerator> generator);
    ~ChunkStreamer();

    void setConfig(const StreamingConfig& config);
    void setGenerator(std::shared_ptr<const WorldGenerator> generator);
    void setBenchmark(ChunkBenchmarkStats* stats);
    void setVisibilityTracer(std::shared_ptr<ChunkVisibilityTracer> tracer);
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
    void getDebugStates(std::vector<DebugChunkState>& out) const;
    int viewDistanceChunks() const { return m_config.viewDistanceChunks; }
    const WorkMetrics& workMetrics() const { return m_workMetrics; }
    const StreamingDiagnosticSnapshot& diagnostics() const { return m_diagnostics; }

private:
    friend class WorldView;
    friend struct detail::ChunkStreamerTestAccess;

    void reset();

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
        uint64_t workEpoch = 0;
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
        uint64_t workEpoch = 0;
        uint64_t chunkInstanceId = 0;
        uint32_t revision = 0;
        std::optional<ChunkVisibilityLifecycleKey> visibilityTrace;
        std::shared_ptr<ChunkVisibilityTracer> visibilityTracer;
        ChunkVisibilityLifecycleKind visibilityKind =
            ChunkVisibilityLifecycleKind::CameraDemand;
        std::array<BlockState, Chunk::VOLUME> blocks{};
        std::array<BlockState, kPaddedVolume> paddedBlocks{};
    };

    struct MeshResult {
        ChunkCoord coord;
        uint64_t requestId = 0;
        uint64_t workEpoch = 0;
        uint64_t chunkInstanceId = 0;
        uint32_t revision = 0;
        std::optional<ChunkVisibilityLifecycleKey> visibilityTrace;
        std::shared_ptr<ChunkVisibilityTracer> visibilityTracer;
        ChunkVisibilityLifecycleKind visibilityKind =
            ChunkVisibilityLifecycleKind::CameraDemand;
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
        uint64_t workEpoch = 0;
        uint32_t observedRevision = 0;
        bool prioritized = false;
        bool obsolete = false;
        std::optional<ChunkVisibilityLifecycleKey> visibilityTrace;
        std::shared_ptr<ChunkVisibilityTracer> visibilityTracer;
        ChunkVisibilityLifecycleKind visibilityKind =
            ChunkVisibilityLifecycleKind::CameraDemand;
    };

    struct PendingDirtyMesh {
        size_t priority = 0;
        ChunkCoord coord;
        bool prioritized = false;
    };

    struct PendingVisibilityTrace {
        ChunkVisibilityLifecycleKey key{};
        ChunkVisibilityLifecycleKind kind =
            ChunkVisibilityLifecycleKind::CameraDemand;
        std::shared_ptr<ChunkVisibilityTracer> tracer;
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
    ChunkManager* const m_chunkManager;
    WorldMeshStore* const m_meshStore;
    BlockRegistry* const m_registry;
    TextureAtlas* const m_atlas;
    std::shared_ptr<const WorldGenerator> m_generator;
    ChunkCache m_cache;
    ChunkBenchmarkStats* m_benchmark = nullptr;
    std::shared_ptr<ChunkVisibilityTracer> m_visibilityTracer;
    std::optional<PendingVisibilityTrace> m_pendingVisibilityTrace;
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
    std::unordered_map<ChunkCoord, ChunkLoadRequestId, ChunkCoordHash> m_loadPending;
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
    std::unordered_set<ChunkCoord, ChunkCoordHash> m_versionReplacementRetries;
    std::unordered_set<ChunkCoord, ChunkCoordHash> m_versionReplacementWaiting;
    std::map<ChunkCoord, std::string> m_generationErrors;
    std::map<ChunkCoord, std::string> m_loadErrors;
    std::map<ChunkCoord, std::string> m_meshErrors;
    std::map<ChunkCoord, std::string> m_evictionErrors;
    uint64_t m_generationFailureVersion = 0;
    uint64_t m_loadFailureVersion = 0;
    uint64_t m_meshFailureVersion = 0;
    uint64_t m_evictionFailureVersion = 0;
    uint64_t m_observedLoaderFailureVersion = 0;
    uint64_t m_observedLoadFailureVersion = 0;
    uint64_t m_chunkLoadFailureVersion = 0;
    // Jobs whose completion has not yet been observed, including cancelled work
    // from an earlier generation lifecycle.
    size_t m_inFlightGen = 0;
    size_t m_inFlightMesh = 0;
    size_t m_inFlightMeshMissing = 0;
    size_t m_inFlightMeshDirty = 0;
    ChunkLoadRequestId m_nextLoadRequestId = 1;
    uint64_t m_nextMeshRequestId = 1;
    std::atomic<uint64_t> m_workEpoch{1};
    MeshRequestKind m_nextSingleSlotMeshKind = MeshRequestKind::Missing;
    std::optional<ChunkCoord> m_lastCenter;
    int m_lastViewDistance = -1;
    int m_lastUnloadDistance = -1;
    bool m_spawnDiscoveryComplete = false;
    bool m_initialStreamingBegun = false;
    bool m_workObservedThisUpdate = false;
    bool m_workStartedThisUpdate = false;
    uint64_t m_streamingUpdateSequence = 0;
    uint64_t m_lifecycleUpdateSequence = 0;
    uint64_t m_nextEvictionRetrySequence = 0;
    std::function<void()> m_generationStartCallback;
    std::function<void()> m_meshBuildStartCallback;
    WorkMetrics m_workMetrics;
    StreamingDiagnosticSnapshot m_diagnostics;

    void applyGenCompletions(size_t budget);
    void applyMeshCompletions(size_t budget);
    ChunkLoadRequestId nextLoadRequestId();
    void cancelPendingLoad(ChunkCoord coord);
    void queueLoadGen(ChunkCoord coord);
    void waitForGenerationCapacity(ChunkCoord coord);
    void waitForMissingMeshCapacity(ChunkCoord coord);
    void waitForMeshDependencies(ChunkCoord coord);
    void wakeGenerationCapacityWaiter();
    void wakeMissingMeshCapacityWaiter();
    void queueLoadedNeighbors(ChunkCoord coord, bool dataBecameReady = false);
    std::optional<size_t> dirtyMeshPriority(ChunkCoord coord) const;
    void queueDirtyMesh(ChunkCoord coord, bool prioritize = false);
    void ensureVisibilityTrace(
        ChunkCoord coord,
        ChunkVisibilityLifecycleKind kind);
    void beginCameraVisibilityTrace(ChunkCoord coord);
    void markVisibilityMeshEligible(ChunkCoord coord,
                                    bool neighborBecameReady);
    void markVisibilityStage(ChunkCoord coord, ChunkVisibilityStage stage);
    std::optional<ChunkVisibilityTraceLink> bindVisibilityTrace(
        ChunkCoord coord,
        const ChunkVisibilityMeshTaskIdentity& meshTask,
        ChunkVisibilityLifecycleKind kind);
    void completePendingVisibilityTrace(
        ChunkCoord coord,
        ChunkVisibilityOutcome outcome,
        const Chunk* chunk = nullptr);
    void completeInFlightVisibilityTrace(
        MeshInFlight& flight,
        ChunkVisibilityOutcome outcome);
    void abandonVisibilityTraces(ChunkVisibilityOutcome outcome);
    void reprioritizeDirtyMeshes();
    void enqueueGeneration(ChunkCoord coord);
    void enqueueMesh(ChunkCoord coord,
                     Chunk& chunk,
                     MeshRequestKind kind,
                     bool prioritized = false);
    void ensureThreadPool();
    bool hasAllNeighborsLoaded(ChunkCoord coord) const;
    StreamingDiagnosticSnapshot collectDiagnostics();
    void refreshDiagnostics(bool advanceWindow);
    bool evictChunk(ChunkCoord coord, bool versionReplacement = false);
    void deferEviction(ChunkCoord coord, bool versionReplacement);
    uint64_t retireIneligibleEvictions(ChunkCoord center,
                                       int viewRadiusSq,
                                       int unloadRadiusSq);
    void retryDeferredEvictions(ChunkCoord center, int unloadRadiusSq);

    ChunkCoord cameraToChunk(const glm::vec3& cameraPos) const;
};

} // namespace Rigel::Voxel
