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
#include "ViewDistancePolicy.h"
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
#include <set>
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
        // Submitted jobs whose result was observed by the completion drain.
        uint64_t generationJobsCompleted = 0;
        // Submitted jobs removed from the executor before worker start.
        uint64_t generationJobsCancelled = 0;
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
        // Candidate coordinates not visited because their chunk Y cannot
        // intersect the generator's inclusive finite-world bounds.
        uint64_t desiredBuildCoordinatesSkippedByWorldBounds = 0;
        // Pending work and event-driven reconciliation candidates visited by
        // the scheduler.
        uint64_t schedulerCoordinatesInspected = 0;
        // Resident cache entries considered for capacity eviction.
        uint64_t cacheEvictionCoordinatesInspected = 0;
        // Resident chunks considered for world-bounds or distance eviction.
        uint64_t residentEvictionCoordinatesInspected = 0;
        // Deferred evictions reconsidered after camera or configuration changes.
        uint64_t deferredEvictionCoordinatesInspected = 0;
        uint64_t lastUpdateDesiredBuildCoordinatesInspected = 0;
        uint64_t lastUpdateDesiredBuildCoordinatesSkippedByWorldBounds = 0;
        uint64_t lastUpdateSchedulerCoordinatesInspected = 0;
        uint64_t lastUpdateCacheEvictionCoordinatesInspected = 0;
        uint64_t lastUpdateResidentEvictionCoordinatesInspected = 0;
        uint64_t lastUpdateDeferredEvictionCoordinatesInspected = 0;
    };

    enum class DebugState : uint8_t {
        WaitingForData,
        WaitingForNeighbors,
        MeshSchedulerWait,
        MeshSubmittedOrBuilding,
        VoxelEmpty,
        AcceptedEmptyGeometry,
        AcceptedNonemptyGeometry,
        DirtyRemeshPending,
        SuppressedByWorldBounds,
        TerminalFailure,
        Count
    };

    enum class DebugPipelineOwner : uint8_t {
        WaitingForData,
        WaitingForNeighbors,
        MeshScheduler,
        MeshWork,
        DirtyRemesh,
        Complete,
        TerminalFailure
    };

    enum class DebugVoxelOccupancy : uint8_t {
        Unknown,
        Empty,
        Nonempty
    };

    enum class DebugInstalledGeometry : uint8_t {
        None,
        Empty,
        Nonempty
    };

    enum class DebugRemeshIntent : uint8_t {
        None,
        Pending
    };

    enum class DebugFailure : uint8_t {
        None,
        Load,
        Generation,
        Mesh,
        Eviction
    };

    enum class DebugDrawEvidence : uint8_t {
        NotApplicable,
        NotDrawn,
        Drawn
    };

    struct DebugChunkState {
        ChunkCoord coord{};
        DebugState state = DebugState::WaitingForData;
        DebugPipelineOwner pipelineOwner =
            DebugPipelineOwner::WaitingForData;
        DebugVoxelOccupancy voxelOccupancy =
            DebugVoxelOccupancy::Unknown;
        DebugInstalledGeometry installedGeometry =
            DebugInstalledGeometry::None;
        DebugRemeshIntent remeshIntent = DebugRemeshIntent::None;
        DebugFailure failure = DebugFailure::None;
        std::optional<ChunkVisibilityLifecycleKey> historicalTraceKey;
        std::optional<ChunkVisibilityLifecycleKind> historicalTraceKind;
        std::optional<ChunkVisibilityOutcome> historicalTraceOutcome;
        std::optional<ChunkVisibilityDrawOutcome>
            historicalTraceDrawOutcome;
        DebugDrawEvidence drawEvidence = DebugDrawEvidence::NotApplicable;
        uint64_t installedGeometryRevision = 0;
    };

    using ChunkLoadCallback =
        std::function<ChunkLoadRequestResult(ChunkLoadRequest)>;
    using ChunkPendingCallback = std::function<bool(ChunkCoord)>;
    using ChunkLoadDrainCallback =
        std::function<std::vector<ChunkLoadCompletion>(size_t)>;
    using ChunkLoadCancelCallback = std::function<void(ChunkCoord)>;
    using ChunkLoadDiagnosticsCallback =
        std::function<ChunkLoadDiagnosticSnapshot()>;
    using ChunkLoadExecutionStateCallback =
        std::function<std::optional<ChunkLoadExecutionState>(ChunkCoord)>;
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
    void setChunkLoadDiagnosticsCallback(ChunkLoadDiagnosticsCallback diagnostics);
    void setChunkLoadExecutionStateCallback(
        ChunkLoadExecutionStateCallback executionState);
    void setChunkEvictionCallback(ChunkEvictionCallback evict);
    void markSpawnDiscoveryComplete();
    void prioritizeMesh(ChunkCoord coord);

    void update(const glm::vec3& cameraPos);
    void processCompletions();
    void getDebugStates(std::vector<DebugChunkState>& out,
                        ChunkCoord center,
                        int radius) const;
    int viewDistanceChunks() const {
        return m_viewDistancePolicy
            ? m_viewDistancePolicy->viewDistanceChunks()
            : m_config.viewDistanceChunks;
    }
    const WorkMetrics& workMetrics() const { return m_workMetrics; }
    const StreamingDiagnosticSnapshot& diagnostics() const { return m_diagnostics; }

private:
    friend class WorldView;
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

    enum class GenerationExecutorPhase : uint8_t {
        Submitting,
        ExecutorQueued,
        WorkerRunning,
        ResultPublished
    };

    struct GenerationFlight {
        std::atomic_bool cancelled{false};
        std::atomic<GenerationExecutorPhase> phase{
            GenerationExecutorPhase::Submitting};
        detail::ThreadPool::JobHandle executorJob;
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
        std::shared_ptr<GenerationFlight> flight;
        std::optional<ChunkVisibilityLifecycleKey> visibilityTrace;
        std::shared_ptr<ChunkVisibilityTracer> visibilityTracer;
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

    enum class ConfigRetiredWorkKind : uint8_t {
        LoadGen,
        MissingMesh,
        DirtyMesh
    };

    struct PendingWorldBoundsReconciliation {
        GeneratorDefinitionData::Bounds replacement;
        uint32_t replacementSemanticsVersion = 0;
        std::optional<GeneratorDefinitionData::Bounds> previous;
        std::optional<ChunkCoord> deferredCursor;
        std::optional<ChunkCoord> retentionCenter;
        int retentionRadiusSquared = 0;
        bool remeshIntersectingRows = false;
        bool forceRemeshIntersecting = false;
        bool revisitFromStart = false;
    };

    struct ViewDistancePolicyState {
        std::shared_ptr<const ViewDistancePolicy> policy;
        int viewDistanceChunks = 0;
        int unloadDistanceChunks = 0;
        int lastViewDistance = -1;
        int lastUnloadDistance = -1;
        bool desiredSetRebuildPending = false;
        std::optional<PendingWorldBoundsReconciliation>
            worldBoundsReconciliation;
        StreamingDiagnosticSnapshot diagnostics;
        uint64_t observedLoaderFailureVersion = 0;
        uint64_t observedLoadFailureVersion = 0;
        uint64_t chunkLoadFailureVersion = 0;
    };

    void reset();
    ViewDistancePolicyState applyViewDistancePolicy(
        std::shared_ptr<const ViewDistancePolicy> policy);
    void restoreViewDistancePolicy(ViewDistancePolicyState state) noexcept;

    enum class PendingWorkKind : uint8_t {
        None,
        Generation,
        Mesh
    };

    struct ChunkImportance {
        bool cameraContaining = false;
        uint64_t distanceSquared = 0;
        ChunkCoord coord{};

        bool operator==(const ChunkImportance&) const = default;
    };

    struct ChunkImportancePrecedes {
        bool operator()(const ChunkImportance& lhs,
                        const ChunkImportance& rhs) const {
            if (lhs.cameraContaining != rhs.cameraContaining) {
                return lhs.cameraContaining;
            }
            if (lhs.distanceSquared != rhs.distanceSquared) {
                return lhs.distanceSquared < rhs.distanceSquared;
            }
            return lhs.coord < rhs.coord;
        }
    };

    struct MeshInFlight {
        MeshRequestKind kind = MeshRequestKind::Missing;
        uint64_t requestId = 0;
        uint64_t workEpoch = 0;
        uint32_t observedRevision = 0;
        bool prioritized = false;
        bool obsolete = false;
        bool replacementPending = false;
        std::optional<ChunkVisibilityLifecycleKey> visibilityTrace;
        std::shared_ptr<ChunkVisibilityTracer> visibilityTracer;
        ChunkVisibilityLifecycleKind visibilityKind =
            ChunkVisibilityLifecycleKind::CameraDemand;
    };

    struct PendingMeshRequest {
        size_t priority = 0;
        ChunkCoord coord;
        MeshRequestKind kind = MeshRequestKind::Missing;
        bool prioritized = false;
        uint64_t sequence = 0;
    };

    struct PendingVisibilityTrace {
        ChunkVisibilityLifecycleKey key{};
        ChunkVisibilityLifecycleKind kind =
            ChunkVisibilityLifecycleKind::CameraDemand;
        std::shared_ptr<ChunkVisibilityTracer> tracer;
    };

    struct PendingMeshRequestGreater {
        bool operator()(const PendingMeshRequest& lhs,
                        const PendingMeshRequest& rhs) const {
            if (lhs.prioritized != rhs.prioritized) {
                return lhs.prioritized < rhs.prioritized;
            }
            if (lhs.priority != rhs.priority) {
                return lhs.priority > rhs.priority;
            }
            return lhs.sequence > rhs.sequence;
        }
    };

    using PendingMeshQueue = std::priority_queue<
        PendingMeshRequest,
        std::vector<PendingMeshRequest>,
        PendingMeshRequestGreater>;

    StreamingConfig m_config;
    std::shared_ptr<const ViewDistancePolicy> m_viewDistancePolicy;
    ChunkManager* const m_chunkManager;
    WorldMeshStore* const m_meshStore;
    BlockRegistry* const m_registry;
    TextureAtlas* const m_atlas;
    std::shared_ptr<const WorldGenerator> m_generator;
    ChunkCache m_cache;
    ChunkBenchmarkStats* m_benchmark = nullptr;
    std::shared_ptr<ChunkVisibilityTracer> m_visibilityTracer;
    std::array<std::optional<PendingVisibilityTrace>, 2>
        m_pendingVisibilityTraces;
    ChunkLoadCallback m_chunkLoader;
    ChunkPendingCallback m_chunkPending;
    ChunkLoadDrainCallback m_chunkLoadDrain;
    ChunkLoadCancelCallback m_chunkLoadCancel;
    ChunkLoadDiagnosticsCallback m_chunkLoadDiagnostics;
    ChunkLoadExecutionStateCallback m_chunkLoadExecutionState;
    ChunkEvictionCallback m_chunkEviction;

    std::unique_ptr<detail::ThreadPool> m_genPool;
    std::unique_ptr<detail::ThreadPool> m_meshPool;
    detail::ConcurrentQueue<GenResult> m_genComplete;
    detail::ConcurrentQueue<MeshResult> m_meshComplete;
    std::unordered_map<ChunkCoord, ChunkState, ChunkCoordHash> m_states;
    // Resident nonempty chunks whose installed meshes were removed when the
    // chunks left the generator bounds and can be rebuilt if they re-enter.
    std::unordered_set<ChunkCoord, ChunkCoordHash>
        m_worldBoundsSuppressedMeshes;
    std::unordered_map<ChunkCoord, ChunkLoadRequestId, ChunkCoordHash> m_loadPending;
    std::unordered_map<
        ChunkCoord,
        std::shared_ptr<GenerationFlight>,
        ChunkCoordHash> m_generationFlights;
    std::unordered_map<ChunkCoord, MeshInFlight, ChunkCoordHash> m_meshInFlight;
    std::unordered_map<ChunkCoord, uint32_t, ChunkCoordHash> m_countedMeshRetryRevisions;
    std::deque<ChunkCoord> m_loadGenQueue;
    std::unordered_set<ChunkCoord, ChunkCoordHash> m_loadGenQueued;
    // The map owns each logical request. The ordered set is its current-camera
    // dispatch index and is rebuilt only when camera relevance changes.
    std::unordered_map<ChunkCoord, ChunkImportance, ChunkCoordHash>
        m_pendingGenerations;
    std::set<ChunkImportance, ChunkImportancePrecedes>
        m_pendingGenerationQueue;
    std::unordered_set<ChunkCoord, ChunkCoordHash> m_meshDependencyWaiting;
    // Retain the kind, but not explicit priority, until a desired-set rebuild
    // either transfers the work to its canonical owner or confirms departure.
    std::unordered_map<ChunkCoord,
                       ConfigRetiredWorkKind,
                       ChunkCoordHash> m_configRetiredWork;
    std::vector<ChunkCoord> m_desired;
    std::unordered_set<ChunkCoord, ChunkCoordHash> m_desiredSet;
    std::unordered_map<ChunkCoord, size_t, ChunkCoordHash> m_desiredPriority;
    std::array<PendingMeshQueue, 2> m_pendingMeshQueues;
    std::unordered_map<ChunkCoord,
                       PendingMeshRequest,
                       ChunkCoordHash> m_pendingMeshes;
    std::unordered_set<ChunkCoord, ChunkCoordHash> m_priorityMeshRequests;
    std::map<ChunkCoord, uint64_t> m_evictionRetryAfter;
    std::unordered_set<ChunkCoord, ChunkCoordHash> m_versionReplacementRetries;
    std::unordered_set<ChunkCoord, ChunkCoordHash> m_versionReplacementWaiting;
    // Deterministic index of residents observed by the streaming pipeline.
    // ChunkManager remains the lifecycle owner.
    std::set<ChunkCoord> m_streamedResidents;
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
    size_t m_replacementPendingMeshCount = 0;
    ChunkLoadRequestId m_nextLoadRequestId = 1;
    uint64_t m_nextMeshRequestId = 1;
    uint64_t m_nextPendingMeshSequence = 1;
    std::atomic<uint64_t> m_workEpoch{1};
    MeshRequestKind m_nextSingleSlotMeshKind = MeshRequestKind::Missing;
    std::optional<ChunkCoord> m_lastCenter;
    int m_lastViewDistance = -1;
    int m_lastUnloadDistance = -1;
    bool m_desiredSetRebuildPending = false;
    std::optional<PendingWorldBoundsReconciliation>
        m_worldBoundsReconciliation;
    bool m_spawnDiscoveryComplete = false;
    bool m_initialStreamingBegun = false;
    bool m_workObservedThisUpdate = false;
    bool m_workStartedThisUpdate = false;
    uint64_t m_streamingUpdateSequence = 0;
    uint64_t m_lifecycleUpdateSequence = 0;
    uint64_t m_nextEvictionRetrySequence = 0;
    std::optional<ChunkCoord> m_evictionRetryScanCursor;
    bool m_evictionRetryScanActive = false;
    std::function<void()> m_generationStartCallback;
    std::function<void(ChunkCoord)> m_generationStartObserver;
    std::function<void()> m_generationResultReadyToPublishCallback;
    std::function<void(ChunkCoord)> m_generationResultPublishedObserver;
    std::function<void()> m_meshBuildStartCallback;
    WorkMetrics m_workMetrics;
    StreamingDiagnosticSnapshot m_diagnostics;

    void applyGenCompletions(size_t budget);
    void applyMeshCompletions(size_t budget);
    uint64_t reconcileDeferredWorldBounds();
    ChunkLoadRequestId nextLoadRequestId();
    void cancelPendingLoad(ChunkCoord coord);
    void queueLoadGen(ChunkCoord coord);
    void queuePendingGeneration(ChunkCoord coord);
    void erasePendingGeneration(ChunkCoord coord);
    void retireGeneration(ChunkCoord coord);
    void retireAllGenerations();
    bool settleGenerationOwner(
        ChunkCoord coord,
        const std::shared_ptr<GenerationFlight>& flight,
        bool cancelledBeforeStart);
    void activatePendingGeneration(ChunkCoord coord);
    void waitForMeshDependencies(ChunkCoord coord);
    void queueLoadedNeighbors(ChunkCoord coord);
    bool hasDirectStreamingDemand(ChunkCoord coord) const;
    bool chunkIntersectsWorldBounds(ChunkCoord coord) const;
    std::optional<size_t> dirtyMeshPriority(ChunkCoord coord) const;
    bool queuePendingMesh(ChunkCoord coord,
                          MeshRequestKind kind,
                          bool prioritize = false);
    void compactPendingMeshQueuesIfNeeded();
    void erasePendingMesh(ChunkCoord coord);
    void retirePendingMesh(ChunkCoord coord);
    void rememberConfigRetiredWork(ChunkCoord coord,
                                   ConfigRetiredWorkKind kind);
    bool isConfigRetiredMeshEligible(
        ChunkCoord coord,
        ConfigRetiredWorkKind kind) const;
    bool hasMeshReconciliationWork(ChunkCoord coord) const;
    PendingWorkKind classifyPendingWork(ChunkCoord coord) const;
    bool hasCanonicalWorkOwner(
        ChunkCoord coord,
        bool includeLoadGenQueue = true) const;
    void reconcileConfigRetiredWork(
        uint64_t& schedulerCoordinatesInspected);
    bool hasEligibleMeshWork(ChunkCoord coord) const;
    void setReplacementPending(ChunkCoord coord,
                               MeshInFlight& flight,
                               bool pending);
    void retireReplacementPending(ChunkCoord coord);
    void queueDirtyMesh(ChunkCoord coord, bool prioritize = false);
    void ensureVisibilityTrace(
        ChunkCoord coord,
        ChunkVisibilityLifecycleKind kind,
        ChunkVisibilityOrigin origin = ChunkVisibilityOrigin::Unresolved);
    void beginCameraVisibilityTrace(ChunkCoord coord);
    void observeVisibilityDataReady(
        ChunkCoord coord,
        ChunkVisibilityOrigin origin);
    static bool areFaceNeighbors(ChunkCoord lhs, ChunkCoord rhs);
    void observeVisibilityNeighborReadiness(ChunkCoord coord);
    void refreshVisibilityDependencySnapshot();
    ChunkVisibilityBlockingNeighborSnapshot visibilityDependencySnapshot(
        ChunkCoord coord) const;
    void markVisibilityMeshEligible(ChunkCoord coord,
                                    bool neighborBecameReady);
    void markVisibilityStage(ChunkCoord coord, ChunkVisibilityStage stage);
    std::optional<ChunkVisibilityTraceLink> currentVisibilityTrace(
        ChunkCoord coord,
        ChunkVisibilityLifecycleKind kind) const;
    ChunkVisibilityBlockerState classifyVisibilityBlocker(
        ChunkCoord coord) const;
    std::optional<ChunkVisibilityTraceLink> bindVisibilityTrace(
        ChunkCoord coord,
        const ChunkVisibilityMeshTaskIdentity& meshTask,
        ChunkVisibilityLifecycleKind kind);
    void completePendingVisibilityTrace(
        ChunkCoord coord,
        ChunkVisibilityOutcome outcome);
    void completePendingVisibilityTrace(
        ChunkCoord coord,
        ChunkVisibilityLifecycleKind kind,
        ChunkVisibilityOutcome outcome);
    void completeInFlightVisibilityTrace(
        MeshInFlight& flight,
        ChunkVisibilityOutcome outcome);
    void abandonVisibilityTraces(ChunkVisibilityOutcome outcome);
    void reprioritizePendingGenerations(
        uint64_t& schedulerCoordinatesInspected);
    void dispatchPendingGenerations(
        uint64_t& schedulerCoordinatesInspected);
    static ChunkImportance chunkImportance(ChunkCoord camera,
                                           ChunkCoord coord);
    size_t generationDispatchLimit() const;
    void reprioritizePendingMeshes(uint64_t& schedulerCoordinatesInspected);
    void dispatchPendingMeshes(uint64_t& schedulerCoordinatesInspected);
    size_t meshDispatchLimit() const;
    void enqueueGeneration(ChunkCoord coord);
    void enqueueMesh(ChunkCoord coord,
                     Chunk& chunk,
                     MeshRequestKind kind,
                     bool prioritized = false);
    void ensureThreadPool();
    uint8_t countMissingDesiredCardinalNeighbors(
        ChunkCoord coord,
        uint8_t limit = static_cast<uint8_t>(DirectionCount)) const;
    bool hasAllNeighborsLoaded(ChunkCoord coord) const;
    StreamingDiagnosticSnapshot collectDiagnostics();
    void refreshDiagnostics(bool advanceWindow);
    bool evictChunk(ChunkCoord coord, bool versionReplacement = false);
    void deferEviction(ChunkCoord coord, bool versionReplacement);
    uint64_t retireIneligibleEvictions(ChunkCoord center,
                                       int viewRadiusSq,
                                       int unloadRadiusSq);
    uint64_t retryDeferredEvictions(ChunkCoord center, int unloadRadiusSq);

    ChunkCoord cameraToChunk(const glm::vec3& cameraPos) const;
};

} // namespace Rigel::Voxel
