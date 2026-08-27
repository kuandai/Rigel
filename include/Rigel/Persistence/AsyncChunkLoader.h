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
#include <map>
#include <memory>
#include <optional>
#include <queue>
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
    using Metrics = Voxel::RegionSchedulerDiagnosticSnapshot;

    AsyncChunkLoader(PersistenceService& service,
                     PersistenceContext context,
                     Voxel::World& world,
                     uint32_t worldGenVersion,
                     size_t ioThreads,
                     size_t workerThreads,
                     int viewDistanceChunks,
                     std::shared_ptr<const Voxel::WorldGenerator> generator);
    ~AsyncChunkLoader();

    Voxel::ChunkLoadRequestResult request(Voxel::ChunkLoadRequest request);
    bool isPending(Voxel::ChunkCoord coord) const;
    std::optional<Voxel::ChunkLoadExecutionState> executionState(
        Voxel::ChunkCoord coord) const;
    void cancel(Voxel::ChunkCoord coord);
    bool persistChunk(Voxel::ChunkCoord coord);

    Voxel::StreamingWorkCount workCount() const;
    Metrics metrics() const;
    Voxel::ChunkLoadDiagnosticSnapshot diagnostics() const;

    std::vector<Voxel::ChunkLoadCompletion> drainCompletions(size_t budget);

    void setMaxCachedRegions(size_t maxRegions);
    void setMaxInFlightRegions(size_t maxRegions);
    void setPrefetchRadius(int radius);
    void setPrefetchPerRequest(size_t count);
    void setRegionDrainBudget(size_t budget);
    void setLoadQueueLimit(size_t maxPending);
    void applyViewDistanceChunks(int chunks);

private:
    friend struct detail::AsyncChunkLoaderTestAccess;

    struct RegionKeyHash {
        size_t operator()(const RegionKey& key) const;
    };

    using ChunkLoadRequestId = Voxel::ChunkLoadRequestId;
    struct ChunkRequestIdentity {
        ChunkLoadRequestId requestId = 0;
        uint64_t incarnation = 0;

        bool operator==(const ChunkRequestIdentity&) const = default;
    };
    using ChunkRequestMap =
        std::unordered_map<Voxel::ChunkCoord,
                           ChunkRequestIdentity,
                           Voxel::ChunkCoordHash>;

    enum class RegionJobOrigin : uint8_t {
        Direct,
        Speculative
    };

    enum class RegionAdmissionKind : uint8_t {
        Initial,
        Retry
    };

    struct RegionJobState {
        RegionKey key;
        RegionJobOrigin origin = RegionJobOrigin::Direct;
        std::chrono::steady_clock::time_point admittedAt{};
        Voxel::detail::ThreadPool::JobId poolJobId = 0;
        uint64_t poolSubmissionCount = 0;
        std::atomic<bool> speculativePoolPending{false};
        std::atomic<Voxel::ChunkLoadExecutionPhase> phase{
            Voxel::ChunkLoadExecutionPhase::SchedulerPending};
        bool demanded = false;
        bool started = false;
    };

    struct RegionEntry {
        std::shared_ptr<ChunkRegionSnapshot> region;
        std::unordered_set<Voxel::ChunkCoord, Voxel::ChunkCoordHash> present;
        std::unordered_map<Voxel::ChunkCoord,
                           std::vector<const ChunkSnapshot*>,
                           Voxel::ChunkCoordHash> spansByCoord;
        bool prefetched = false;
    };

    struct RegionResult {
        RegionKey key;
        uint64_t revision = 0;
        std::shared_ptr<RegionJobState> job;
        RegionEntry entry;
        std::string error;
        bool ok = false;
        bool exists = false;
        bool retryable = false;
    };

    struct PayloadJobState {
        ChunkRequestIdentity request;
        std::atomic<Voxel::ChunkLoadExecutionPhase> phase{
            Voxel::ChunkLoadExecutionPhase::SchedulerPending};
    };

    struct ChunkPayload {
        Voxel::ChunkCoord coord;
        ChunkRequestIdentity request;
        RegionKey regionKey;
        uint64_t regionRevision = 0;
        Voxel::ChunkBuffer blocks;
        uint32_t worldGenVersion = 0;
        std::string error;
        bool empty = false;
        bool cancelled = false;
        bool failed = false;
        bool loadedFromDisk = false;
        std::shared_ptr<PayloadJobState> job;
    };

    void drainRegionCompletions(size_t budget,
                                std::vector<Voxel::ChunkLoadCompletion>& resolved);
    void drainPayloadCompletions(size_t budget,
                                 std::vector<Voxel::ChunkLoadCompletion>& resolved);
    Voxel::ChunkLoadRequestResult queueChunkLoad(
        Voxel::ChunkCoord coord,
        ChunkRequestIdentity request,
        RegionAdmissionKind admission = RegionAdmissionKind::Initial);
    void deferChunkLoad(Voxel::ChunkCoord coord,
                        ChunkRequestIdentity request);
    void startDeferredChunkLoads(
        std::vector<Voxel::ChunkLoadCompletion>* resolved = nullptr);
    void startRetryChunkLoads(
        std::vector<Voxel::ChunkLoadCompletion>* resolved = nullptr);
    void completeChunkLoad(Voxel::ChunkCoord coord,
                           ChunkRequestIdentity request,
                           Voxel::ChunkLoadOutcome outcome,
                           std::vector<Voxel::ChunkLoadCompletion>& resolved);
    void restartChunkLoad(Voxel::ChunkCoord coord,
                          ChunkRequestIdentity request,
                          std::vector<Voxel::ChunkLoadCompletion>& resolved);
    void scheduleRegionRetry(const RegionKey& key,
                             ChunkRequestMap pending,
                             const std::string& error);
    void markTerminalChunkLoad(Voxel::ChunkCoord coord,
                               ChunkRequestIdentity request,
                               std::string diagnostic,
                               Voxel::ChunkLoadExecutionOwner owner);
    void clearTerminalChunkLoad(Voxel::ChunkCoord coord);
    void refreshLastTerminalError();
    void startQueuedRegionLoads();
    std::shared_ptr<RegionJobState> takeQueuedRegionLoad(bool direct);
    void startRegionLoad(const RegionKey& key,
                         const std::shared_ptr<RegionJobState>& jobState);
    bool cancelQueuedSpeculativeRegionLoad();
    bool reserveQueuedSpeculativeRegionSlot();
    bool yieldSubmittedSpeculativeRegionLoad();
    void trackSubmittedSpeculativeRegionJob(
        const std::shared_ptr<RegionJobState>& job);
    void markSpeculativeRegionPoolPending(
        const std::shared_ptr<RegionJobState>& job);
    void retireSpeculativeRegionPoolPending(
        const std::shared_ptr<RegionJobState>& job);
    bool retireSubmittedSpeculativeRegionJob(
        const std::shared_ptr<RegionJobState>& job);
    void cancelQueuedDirectRegionLoad(const RegionKey& key);
    bool hasDirectRegionDemand(const RegionKey& key) const;
    void undoRegionLoadAttempt(const RegionKey& key);
    bool queueRegionLoad(
        const RegionKey& key,
        RegionJobOrigin origin = RegionJobOrigin::Direct,
        RegionAdmissionKind admission = RegionAdmissionKind::Initial);
    void queuePayloadBuild(const RegionEntry& entry,
                           Voxel::ChunkCoord coord,
                           ChunkRequestIdentity request);
    void prefetchNeighbors(const RegionKey& center);
    void touch(const RegionKey& key);
    void evictIfNeeded();
    void promoteRegionDemand(const RegionKey& key);
    int prefetchRadiusForViewDistance(int chunks) const;
    int estimateRegionSpan() const;
    bool regionMayExist(const RegionKey& key);

    bool applyPayload(const ChunkPayload& payload);
    void invalidateRegion(const RegionKey& key);

    using RetryClock = std::chrono::steady_clock;
    RetryClock::time_point retryNow() const;
    RetryClock::duration retryDelay(size_t failureRounds) const;
    RetryClock::time_point metricNow() const;

    struct RegionMetricCounters {
        std::atomic<uint64_t> logicalAdmissions{0};
        std::atomic<uint64_t> retryAdmissions{0};
        std::atomic<uint64_t> logicalPreStartCancellations{0};
        std::atomic<uint64_t> poolSubmissions{0};
        std::atomic<uint64_t> poolResubmissions{0};
        std::atomic<uint64_t> successfulPoolYields{0};
        std::atomic<uint64_t> terminalPoolCancellations{0};
        std::atomic<uint64_t> poolWorkerStarts{0};
        std::atomic<uint64_t> inlineExecutions{0};
        std::atomic<uint64_t> resultsPublished{0};
        std::atomic<uint64_t> resultsDrained{0};
        std::atomic<uint64_t> missingProbes{0};
        std::atomic<uint64_t> admissionToWorkerStartNanoseconds{0};
        std::atomic<uint64_t> maxAdmissionToWorkerStartNanoseconds{0};
        std::atomic<uint64_t> workerExecutionNanoseconds{0};
        std::atomic<uint64_t> maxWorkerExecutionNanoseconds{0};
    };

    RegionMetricCounters& regionMetricCounters(RegionJobOrigin origin);
    const RegionMetricCounters& regionMetricCounters(
        RegionJobOrigin origin) const;
    static Voxel::RegionSchedulerOriginDiagnostics regionJobMetrics(
        const RegionMetricCounters& counters,
        std::atomic<bool>* subsetReadEntered = nullptr,
        std::atomic<bool>* subsetReadReleased = nullptr);

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

    std::shared_ptr<const Voxel::WorldGenerator> m_generator;

    std::function<void()> m_regionLoadStartCallback;
    std::function<void(const RegionKey&, RegionJobOrigin)>
        m_regionLoadStartObserver;
    std::function<void()> m_regionResultReadyToPublishCallback;
    std::function<void()> m_payloadBuildStartCallback;
    std::function<void(Voxel::ChunkCoord)>
        m_payloadResultPublishedObserver;
    std::function<void()> m_ioPoolStopStartCallback;
    std::function<void()> m_workerPoolStopStartCallback;

    Voxel::detail::ConcurrentQueue<RegionResult> m_regionComplete;
    Voxel::detail::ConcurrentQueue<ChunkPayload> m_chunkComplete;

    std::unordered_map<RegionKey, RegionEntry, RegionKeyHash> m_cache;
    std::unordered_set<RegionKey, RegionKeyHash> m_inFlight;
    std::unordered_map<RegionKey,
                       std::shared_ptr<RegionJobState>,
                       RegionKeyHash> m_regionJobs;
    std::unordered_map<RegionKey,
                       ChunkRequestMap,
                       RegionKeyHash> m_regionPending;
    std::deque<RegionKey> m_directRegionLoads;
    std::deque<RegionKey> m_speculativeRegionLoads;
    std::deque<std::shared_ptr<RegionJobState>>
        m_submittedSpeculativeRegionJobs;
    size_t m_queuedSpeculativeRegionJobCount = 0;
    size_t m_demandOwnedQueuedRegionJobCount = 0;
    size_t m_demandOwnedDispatchedRegionJobCount = 0;
    size_t m_speculativeOwnedDispatchedRegionJobCount = 0;
    std::atomic<size_t> m_speculativePoolJobsPending{0};
    size_t m_maxSpeculativePoolJobsPending = 0;
    uint64_t m_speculativeYieldCalls = 0;
    uint64_t m_speculativeYieldCandidateVisits = 0;
    size_t m_maxSpeculativeYieldCandidateVisits = 0;
    ChunkRequestMap m_pendingChunks;
    std::deque<Voxel::ChunkCoord> m_deferredChunkLoads;
    ChunkRequestMap m_deferredChunkRequests;

    struct ChunkRetryState {
        ChunkRequestIdentity request;
        RetryClock::time_point retryAfter{};
    };

    struct TerminalChunkState {
        std::string diagnostic;
        Voxel::ChunkLoadExecutionOwner owner =
            Voxel::ChunkLoadExecutionOwner::Region;
    };

    struct ChunkRetrySchedule {
        Voxel::ChunkCoord coord;
        ChunkRequestIdentity request;
        RetryClock::time_point retryAfter{};
    };

    struct ChunkRetryScheduleGreater {
        bool operator()(const ChunkRetrySchedule& lhs,
                        const ChunkRetrySchedule& rhs) const {
            return lhs.retryAfter > rhs.retryAfter;
        }
    };

    std::unordered_map<Voxel::ChunkCoord,
                       ChunkRetryState,
                       Voxel::ChunkCoordHash> m_retryChunks;
    std::priority_queue<ChunkRetrySchedule,
                        std::vector<ChunkRetrySchedule>,
                        ChunkRetryScheduleGreater> m_retrySchedule;
    std::map<Voxel::ChunkCoord, TerminalChunkState> m_terminalChunks;
    std::unordered_map<Voxel::ChunkCoord,
                       size_t,
                       Voxel::ChunkCoordHash> m_chunkRetryRounds;
    std::string m_lastTerminalError;
    uint64_t m_terminalFailureVersion = 0;
    std::deque<Voxel::ChunkLoadCompletion> m_resolvedChunks;
    std::unordered_map<
        Voxel::ChunkCoord,
        std::shared_ptr<PayloadJobState>,
        Voxel::ChunkCoordHash> m_payloadInFlight;
    uint64_t m_nextRequestIncarnation = 1;
    uint64_t m_requestsStarted = 0;
    std::deque<RegionKey> m_lru;

    struct RegionPresence {
        bool exists = false;
        std::chrono::steady_clock::time_point nextCheck{};
    };
    std::unordered_map<RegionKey, RegionPresence, RegionKeyHash> m_regionPresence;
    std::unordered_map<RegionKey, size_t, RegionKeyHash> m_regionLoadAttempts;
    std::unordered_map<RegionKey, uint64_t, RegionKeyHash> m_regionRevisions;

    std::function<RetryClock::time_point()> m_retryClock;
    std::function<RetryClock::time_point()> m_metricClock;

    RegionMetricCounters m_directRegionMetrics;
    RegionMetricCounters m_speculativeRegionMetrics;
    std::atomic<uint64_t> m_demandPromotions{0};
    std::atomic<uint64_t> m_usefulPrefetchCacheHits{0};
    std::atomic<uint64_t> m_speculativeEvictionsBeforeDemand{0};

    Voxel::detail::ThreadPool m_ioPool;
    Voxel::detail::ThreadPool m_workerPool;
};

} // namespace Rigel::Persistence
