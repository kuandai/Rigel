#include "Rigel/Persistence/AsyncChunkLoader.h"

#include "Rigel/Persistence/Backends/CR/CRFormat.h"
#include "Rigel/Persistence/ChunkSpanMerge.h"
#include "Rigel/Persistence/Containers.h"
#include "Rigel/Persistence/RegionLayout.h"
#include "Rigel/Persistence/Storage.h"
#include "Rigel/Persistence/WorldPersistence.h"
#include "Rigel/Voxel/Chunk.h"
#include "Rigel/Voxel/World.h"
#include "Rigel/Core/Profiler.h"
#include "backends/cr/CRWorldMetadata.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <limits>
#include <sstream>
#include <system_error>

#include <spdlog/spdlog.h>

namespace Rigel::Persistence {

namespace {
constexpr size_t kMaxRegionLoadAttempts = 3;

void advanceFailureVersion(uint64_t& version) {
    ++version;
    if (version == 0) {
        ++version;
    }
}
constexpr auto kInitialRegionRetryDelay = std::chrono::milliseconds(100);
constexpr auto kMaxRegionRetryDelay = std::chrono::seconds(2);
constexpr size_t kZeroCapSpeculativeRegionQueueLimit = 64;

uint64_t nanosecondsBetween(std::chrono::steady_clock::time_point start,
                            std::chrono::steady_clock::time_point end) {
    if (end <= start) {
        return 0;
    }
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start)
            .count());
}

void recordMaximum(std::atomic<uint64_t>& maximum, uint64_t value) {
    uint64_t current = maximum.load(std::memory_order_relaxed);
    while (current < value &&
           !maximum.compare_exchange_weak(
               current,
               value,
               std::memory_order_relaxed,
               std::memory_order_relaxed)) {}
}

void eraseQueuedRegion(std::deque<RegionKey>& queue, const RegionKey& key) {
    auto it = std::find(queue.begin(), queue.end(), key);
    if (it != queue.end()) {
        queue.erase(it);
    }
}

std::string regionDecodeDiagnostic(const RegionKey& key,
                                   Voxel::ChunkCoord coord,
                                   const std::string& error) {
    std::ostringstream message;
    message << "Persisted region (" << key.x << ", " << key.y << ", " << key.z
            << ") for chunk (" << coord.x << ", " << coord.y << ", "
            << coord.z << ") could not be decoded: " << error
            << ". Restore or repair this region, then revisit the area to retry.";
    return message.str();
}

std::string chunkPayloadDiagnostic(Voxel::ChunkCoord coord,
                                   const std::string& error) {
    std::ostringstream message;
    message << "Persisted chunk payload at (" << coord.x << ", " << coord.y
            << ", " << coord.z << ") could not be decoded: " << error
            << ". Restore or repair this region, then revisit the area to retry.";
    return message.str();
}
}

size_t AsyncChunkLoader::RegionKeyHash::operator()(const RegionKey& key) const {
    size_t seed = std::hash<std::string>{}(key.zoneId);
    size_t hx = std::hash<int32_t>{}(key.x);
    size_t hy = std::hash<int32_t>{}(key.y);
    size_t hz = std::hash<int32_t>{}(key.z);
    seed ^= hx + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    seed ^= hy + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    seed ^= hz + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    return seed;
}

AsyncChunkLoader::RetryClock::time_point AsyncChunkLoader::retryNow() const {
    return m_retryClock ? m_retryClock() : RetryClock::now();
}

AsyncChunkLoader::RetryClock::duration
AsyncChunkLoader::retryDelay(size_t failureRounds) const {
    auto delay = kInitialRegionRetryDelay;
    for (size_t round = 1;
         round < failureRounds && delay < kMaxRegionRetryDelay;
         ++round) {
        delay *= 2;
    }
    return std::min<RetryClock::duration>(delay, kMaxRegionRetryDelay);
}

AsyncChunkLoader::RetryClock::time_point AsyncChunkLoader::metricNow() const {
    return m_metricClock ? m_metricClock() : RetryClock::now();
}

AsyncChunkLoader::AsyncChunkLoader(PersistenceService& service,
                                   PersistenceContext context,
                                   Voxel::World& world,
                                   uint32_t worldGenVersion,
                                   size_t ioThreads,
                                   size_t workerThreads,
                                   int viewDistanceChunks,
                                   std::shared_ptr<const Voxel::WorldGenerator> generator)
    : m_service(&service),
      m_context(std::move(context)),
      m_format(service.openFormat(m_context)),
      m_world(&world),
      m_worldGenVersion(worldGenVersion),
      m_generator(std::move(generator)),
      m_ioPool(ioThreads),
      m_workerPool(workerThreads) {
    if (m_format->descriptor().id == Backends::CR::descriptor().id) {
        Backends::CR::requireSupportedDefaultZone(m_context, m_zoneId);
    }

    int regionSpan = estimateRegionSpan();
    if (regionSpan < 1) {
        regionSpan = 1;
    }
    int radius = viewDistanceChunks / regionSpan;
    if (radius < 1) {
        radius = 1;
    }
    if (radius > 2) {
        radius = 2;
    }
    m_prefetchRadius = radius;
}

AsyncChunkLoader::~AsyncChunkLoader() {
    m_ioPool.stop(m_ioPoolStopStartCallback);
    m_submittedSpeculativeRegionJobs.clear();
    m_workerPool.stop(m_workerPoolStopStartCallback);
}

void AsyncChunkLoader::setMaxCachedRegions(size_t maxRegions) {
    m_maxCachedRegions = maxRegions;
}

void AsyncChunkLoader::setMaxInFlightRegions(size_t maxRegions) {
    m_maxInFlightRegions = maxRegions;
    startQueuedRegionLoads();
}

void AsyncChunkLoader::setPrefetchRadius(int radius) {
    m_prefetchRadius = radius;
}

void AsyncChunkLoader::setPrefetchPerRequest(size_t count) {
    m_prefetchPerRequest = count;
}

void AsyncChunkLoader::setRegionDrainBudget(size_t budget) {
    m_regionDrainBudget = budget;
}

void AsyncChunkLoader::setLoadQueueLimit(size_t maxPending) {
    m_loadQueueLimit = maxPending;
    startDeferredChunkLoads();
}

Voxel::ChunkLoadRequestResult AsyncChunkLoader::request(
    Voxel::ChunkLoadRequest request) {
    const Voxel::ChunkCoord coord = request.coord;
    if (!m_format || !m_world) {
        return Voxel::ChunkLoadRequestResult::Missing;
    }
    if (m_pendingChunks.find(coord) != m_pendingChunks.end()) {
        return Voxel::ChunkLoadRequestResult::Queued;
    }
    if (m_deferredChunkRequests.find(coord) != m_deferredChunkRequests.end()) {
        return Voxel::ChunkLoadRequestResult::Deferred;
    }
    if (m_retryChunks.find(coord) != m_retryChunks.end() ||
        m_terminalChunks.find(coord) != m_terminalChunks.end()) {
        return Voxel::ChunkLoadRequestResult::Deferred;
    }

    RegionKey key = m_format->regionLayout().regionForChunk(m_zoneId, coord);
    auto cacheIt = m_cache.find(key);
    if (cacheIt != m_cache.end() && cacheIt->second.prefetched) {
        cacheIt->second.prefetched = false;
        m_usefulPrefetchCacheHits.fetch_add(1, std::memory_order_relaxed);
    }
    promoteRegionDemand(key);
    if (cacheIt != m_cache.end() &&
        cacheIt->second.present.find(coord) == cacheIt->second.present.end()) {
        return Voxel::ChunkLoadRequestResult::Missing;
    }
    if (cacheIt == m_cache.end() &&
        m_regionJobs.find(key) == m_regionJobs.end() &&
        !regionMayExist(key)) {
        return Voxel::ChunkLoadRequestResult::Missing;
    }

    const ChunkRequestIdentity identity{
        request.requestId,
        m_nextRequestIncarnation++};
    if (m_nextRequestIncarnation == 0) {
        m_nextRequestIncarnation = 1;
    }
    if (m_loadQueueLimit > 0 && m_pendingChunks.size() >= m_loadQueueLimit) {
        deferChunkLoad(coord, identity);
        ++m_requestsStarted;
        return Voxel::ChunkLoadRequestResult::Deferred;
    }

    Voxel::ChunkLoadRequestResult result =
        queueChunkLoad(coord, identity);
    if (result == Voxel::ChunkLoadRequestResult::Queued) {
        ++m_requestsStarted;
    }
    return result;
}

Voxel::ChunkLoadRequestResult AsyncChunkLoader::queueChunkLoad(
    Voxel::ChunkCoord coord,
    ChunkRequestIdentity request,
    RegionAdmissionKind admission) {
    RegionKey key = m_format->regionLayout().regionForChunk(m_zoneId, coord);
    auto cacheIt = m_cache.find(key);
    if (cacheIt != m_cache.end()) {
        if (cacheIt->second.present.find(coord) == cacheIt->second.present.end()) {
            return Voxel::ChunkLoadRequestResult::Missing;
        }
        m_pendingChunks[coord] = request;
        queuePayloadBuild(cacheIt->second, coord, request);
        touch(key);
        return Voxel::ChunkLoadRequestResult::Queued;
    }
    if (m_regionJobs.find(key) == m_regionJobs.end()) {
        if (!regionMayExist(key)) {
            return Voxel::ChunkLoadRequestResult::Missing;
        }
    }

    m_pendingChunks[coord] = request;
    m_regionPending[key][coord] = request;
    if (queueRegionLoad(key, RegionJobOrigin::Direct, admission)) {
        prefetchNeighbors(key);
    }
    return Voxel::ChunkLoadRequestResult::Queued;
}

bool AsyncChunkLoader::isPending(Voxel::ChunkCoord coord) const {
    return m_pendingChunks.find(coord) != m_pendingChunks.end() ||
        m_deferredChunkRequests.find(coord) != m_deferredChunkRequests.end() ||
        m_retryChunks.find(coord) != m_retryChunks.end() ||
        m_terminalChunks.find(coord) != m_terminalChunks.end();
}

std::optional<Voxel::ChunkLoadExecutionState>
AsyncChunkLoader::executionState(Voxel::ChunkCoord coord) const {
    if (m_retryChunks.find(coord) != m_retryChunks.end()) {
        return Voxel::ChunkLoadExecutionState{
            Voxel::ChunkLoadExecutionOwner::Region,
            Voxel::ChunkLoadExecutionPhase::RetryWaiting};
    }
    const auto terminal = m_terminalChunks.find(coord);
    if (terminal != m_terminalChunks.end()) {
        return Voxel::ChunkLoadExecutionState{
            terminal->second.owner,
            Voxel::ChunkLoadExecutionPhase::TerminalFailed};
    }
    if (m_deferredChunkRequests.find(coord) !=
        m_deferredChunkRequests.end()) {
        return Voxel::ChunkLoadExecutionState{
            Voxel::ChunkLoadExecutionOwner::Region,
            Voxel::ChunkLoadExecutionPhase::SchedulerPending};
    }
    const auto pending = m_pendingChunks.find(coord);
    if (pending == m_pendingChunks.end()) {
        return std::nullopt;
    }
    const auto payload = m_payloadInFlight.find(coord);
    if (payload != m_payloadInFlight.end()) {
        return Voxel::ChunkLoadExecutionState{
            Voxel::ChunkLoadExecutionOwner::Payload,
            payload->second->request == pending->second
                ? payload->second->phase.load(std::memory_order_acquire)
                : Voxel::ChunkLoadExecutionPhase::SchedulerPending};
    }
    if (!m_format) {
        return Voxel::ChunkLoadExecutionState{
            Voxel::ChunkLoadExecutionOwner::Region,
            Voxel::ChunkLoadExecutionPhase::SchedulerPending};
    }
    const RegionKey key =
        m_format->regionLayout().regionForChunk(m_zoneId, coord);
    const auto regionJob = m_regionJobs.find(key);
    return Voxel::ChunkLoadExecutionState{
        Voxel::ChunkLoadExecutionOwner::Region,
        regionJob == m_regionJobs.end()
            ? Voxel::ChunkLoadExecutionPhase::SchedulerPending
            : regionJob->second->phase.load(std::memory_order_acquire)};
}

Voxel::StreamingWorkCount AsyncChunkLoader::workCount() const {
    return Voxel::StreamingWorkCount{
        .pending = m_pendingChunks.size() + m_deferredChunkRequests.size() +
            m_retryChunks.size() + m_terminalChunks.size(),
        .inFlight = m_inFlight.size() + m_payloadInFlight.size(),
        .started = m_requestsStarted,
        .terminalErrors = m_terminalChunks.size(),
        .lastError = m_lastTerminalError,
        .failureVersion = m_terminalFailureVersion
    };
}

AsyncChunkLoader::Metrics AsyncChunkLoader::metrics() const {
    Metrics snapshot;
    snapshot.directOrigin = regionJobMetrics(m_directRegionMetrics);
    snapshot.speculativeOrigin = regionJobMetrics(m_speculativeRegionMetrics);
    snapshot.demandPromotions =
        m_demandPromotions.load(std::memory_order_relaxed);
    snapshot.usefulPrefetchCacheHits =
        m_usefulPrefetchCacheHits.load(std::memory_order_relaxed);
    snapshot.speculativeEvictionsBeforeDemand =
        m_speculativeEvictionsBeforeDemand.load(std::memory_order_relaxed);
    snapshot.demandOwnedQueued = m_demandOwnedQueuedRegionJobCount;
    snapshot.speculativeOwnedQueued = m_queuedSpeculativeRegionJobCount;
    snapshot.demandOwnedDispatchedUndrained =
        m_demandOwnedDispatchedRegionJobCount;
    snapshot.speculativeOwnedDispatchedUndrained =
        m_speculativeOwnedDispatchedRegionJobCount;
    snapshot.speculativePoolJobsPending =
        m_speculativePoolJobsPending.load(std::memory_order_relaxed);
    snapshot.maxSpeculativePoolJobsPending =
        m_maxSpeculativePoolJobsPending;
    snapshot.speculativePoolYieldCalls = m_speculativeYieldCalls;
    snapshot.speculativePoolYieldCandidateVisits =
        m_speculativeYieldCandidateVisits;
    snapshot.maxSpeculativePoolYieldCandidateVisits =
        m_maxSpeculativeYieldCandidateVisits;
    return snapshot;
}

Voxel::ChunkLoadDiagnosticSnapshot AsyncChunkLoader::diagnostics() const {
    return Voxel::ChunkLoadDiagnosticSnapshot{
        .work = workCount(),
        .regionScheduler = metrics()
    };
}

void AsyncChunkLoader::cancel(Voxel::ChunkCoord coord) {
    bool releasedCapacity = m_pendingChunks.erase(coord) > 0;
    m_deferredChunkRequests.erase(coord);
    m_retryChunks.erase(coord);
    m_chunkRetryRounds.erase(coord);
    clearTerminalChunkLoad(coord);
    if (!m_format) {
        return;
    }
    RegionKey key = m_format->regionLayout().regionForChunk(m_zoneId, coord);
    auto it = m_regionPending.find(key);
    if (it != m_regionPending.end()) {
        it->second.erase(coord);
        if (it->second.empty()) {
            m_regionPending.erase(it);
        }
    }
    if (!hasDirectRegionDemand(key)) {
        cancelQueuedDirectRegionLoad(key);
    }
    if (releasedCapacity) {
        startDeferredChunkLoads();
    }
    startQueuedRegionLoads();
}

bool AsyncChunkLoader::persistChunk(Voxel::ChunkCoord coord) {
    if (!m_format || !m_service || !m_world) {
        return false;
    }

    Voxel::Chunk* chunk = m_world->chunkManager().getChunk(coord);
    if (!chunk || !chunk->isPersistDirty()) {
        return true;
    }

    try {
        saveChunkToDisk(*m_world, *m_service, m_context, coord);
        RegionKey key = m_format->regionLayout().regionForChunk(m_zoneId, coord);
        invalidateRegion(key);
    } catch (const std::exception& e) {
        spdlog::error("Chunk persistence failed at ({}, {}, {}): {}",
                      coord.x,
                      coord.y,
                      coord.z,
                      e.what());
        return false;
    } catch (...) {
        spdlog::error("Chunk persistence failed at ({}, {}, {})",
                      coord.x,
                      coord.y,
                      coord.z);
        return false;
    }

    Voxel::Chunk* savedChunk = m_world->chunkManager().getChunk(coord);
    if (savedChunk != chunk) {
        return false;
    }
    savedChunk->clearPersistDirty();
    return true;
}

std::vector<Voxel::ChunkLoadCompletion> AsyncChunkLoader::drainCompletions(
    size_t budget) {
    std::vector<Voxel::ChunkLoadCompletion> resolved;
    while (!m_resolvedChunks.empty()) {
        resolved.push_back(m_resolvedChunks.front());
        m_resolvedChunks.pop_front();
    }
    startRetryChunkLoads(&resolved);
    {
        PROFILE_SCOPE("Streaming/LoadRegionDrain");
        size_t regionBudget = m_regionDrainBudget;
        if (regionBudget == 0) {
            regionBudget = std::numeric_limits<size_t>::max();
        }
        if (budget != std::numeric_limits<size_t>::max()) {
            regionBudget = std::min(regionBudget, budget);
        }
        drainRegionCompletions(regionBudget, resolved);
    }
    {
        PROFILE_SCOPE("Streaming/LoadPayloadDrain");
        drainPayloadCompletions(budget, resolved);
    }
    return resolved;
}

void AsyncChunkLoader::drainRegionCompletions(
    size_t budget,
    std::vector<Voxel::ChunkLoadCompletion>& resolved) {
    size_t drained = 0;
    RegionResult result;
    while (drained < budget && m_regionComplete.tryPop(result)) {
        ++drained;
        m_inFlight.erase(result.key);
        retireSubmittedSpeculativeRegionJob(result.job);
        if (result.job) {
            if (result.job->demanded) {
                --m_demandOwnedDispatchedRegionJobCount;
            } else {
                --m_speculativeOwnedDispatchedRegionJobCount;
            }
            regionMetricCounters(result.job->origin)
                .resultsDrained.fetch_add(1, std::memory_order_seq_cst);
        }
        auto jobIt = m_regionJobs.find(result.key);
        if (jobIt != m_regionJobs.end() && jobIt->second == result.job) {
            m_regionJobs.erase(jobIt);
        }
        auto pendingIt = m_regionPending.find(result.key);
        if (result.revision != m_regionRevisions[result.key]) {
            m_regionLoadAttempts.erase(result.key);
            if (pendingIt != m_regionPending.end() && !pendingIt->second.empty() &&
                m_regionJobs.find(result.key) == m_regionJobs.end()) {
                queueRegionLoad(result.key);
            }
            continue;
        }
        if (!result.ok) {
            size_t attempts = m_regionLoadAttempts[result.key];
            bool hasPendingRequests =
                pendingIt != m_regionPending.end() && !pendingIt->second.empty();
            if (result.retryable && hasPendingRequests &&
                attempts < kMaxRegionLoadAttempts) {
                spdlog::warn(
                    "Region load failed ({} {} {}) on attempt {} of {}: {}; retrying",
                    result.key.x,
                    result.key.y,
                    result.key.z,
                    attempts,
                    kMaxRegionLoadAttempts,
                    result.error);
                queueRegionLoad(
                    result.key,
                    RegionJobOrigin::Direct,
                    RegionAdmissionKind::Retry);
                continue;
            }

            m_regionLoadAttempts.erase(result.key);
            if (!hasPendingRequests) {
                spdlog::warn("Region prefetch failed ({} {} {}): {}",
                             result.key.x,
                             result.key.y,
                             result.key.z,
                             result.error);
                continue;
            }

            auto pending = std::move(pendingIt->second);
            m_regionPending.erase(pendingIt);
            if (result.retryable) {
                scheduleRegionRetry(result.key, std::move(pending), result.error);
                continue;
            }

            for (const auto& [coord, request] : pending) {
                std::string diagnostic =
                    regionDecodeDiagnostic(result.key, coord, result.error);
                spdlog::error("{}", diagnostic);
                markTerminalChunkLoad(
                    coord,
                    request,
                    std::move(diagnostic),
                    Voxel::ChunkLoadExecutionOwner::Region);
            }
            startDeferredChunkLoads(&resolved);
            continue;
        }

        m_regionLoadAttempts.erase(result.key);
        auto now = std::chrono::steady_clock::now();
        RegionPresence& presence = m_regionPresence[result.key];
        if (result.exists) {
            presence.exists = true;
            presence.nextCheck = std::chrono::steady_clock::time_point{};
        } else {
            presence.exists = false;
            presence.nextCheck = now + std::chrono::seconds(2);
        }
        result.entry.prefetched =
            result.job && !result.job->demanded;
        m_cache[result.key] = std::move(result.entry);
        touch(result.key);
        evictIfNeeded();

        if (pendingIt != m_regionPending.end()) {
            auto pending = std::move(pendingIt->second);
            m_regionPending.erase(pendingIt);
            auto cacheIt = m_cache.find(result.key);
            if (cacheIt != m_cache.end()) {
                for (const auto& [coord, request] : pending) {
                    if (cacheIt->second.present.find(coord) == cacheIt->second.present.end()) {
                        completeChunkLoad(
                            coord,
                            request,
                            Voxel::ChunkLoadOutcome::Missing,
                            resolved);
                        continue;
                    }
                    queuePayloadBuild(cacheIt->second, coord, request);
                }
            }
        }
    }
    startQueuedRegionLoads();
}

void AsyncChunkLoader::drainPayloadCompletions(
    size_t budget,
    std::vector<Voxel::ChunkLoadCompletion>& resolved) {
    size_t applied = 0;
    ChunkPayload payload;
    while (applied < budget && m_chunkComplete.tryPop(payload)) {
        auto flightIt = m_payloadInFlight.find(payload.coord);
        if (flightIt != m_payloadInFlight.end() &&
            flightIt->second == payload.job) {
            m_payloadInFlight.erase(flightIt);
        }

        auto pendingIt = m_pendingChunks.find(payload.coord);
        if (payload.cancelled || pendingIt == m_pendingChunks.end()) {
            continue;
        }
        if (pendingIt->second != payload.request) {
            if (m_payloadInFlight.find(payload.coord) == m_payloadInFlight.end()) {
                restartChunkLoad(payload.coord, pendingIt->second, resolved);
            }
            continue;
        }
        if (payload.regionRevision != m_regionRevisions[payload.regionKey]) {
            restartChunkLoad(payload.coord, payload.request, resolved);
            continue;
        }
        if (payload.failed) {
            std::string diagnostic =
                chunkPayloadDiagnostic(payload.coord, payload.error);
            spdlog::error("{}", diagnostic);
            markTerminalChunkLoad(
                payload.coord,
                payload.request,
                std::move(diagnostic),
                Voxel::ChunkLoadExecutionOwner::Payload);
            startDeferredChunkLoads(&resolved);
            ++applied;
            continue;
        }
        applyPayload(payload);
        completeChunkLoad(
            payload.coord,
            payload.request,
            Voxel::ChunkLoadOutcome::Loaded,
            resolved);
        ++applied;
    }
}

void AsyncChunkLoader::deferChunkLoad(
    Voxel::ChunkCoord coord,
    ChunkRequestIdentity request) {
    if (m_deferredChunkRequests.emplace(coord, request).second) {
        m_deferredChunkLoads.push_back(coord);
    }
}

void AsyncChunkLoader::startDeferredChunkLoads(
    std::vector<Voxel::ChunkLoadCompletion>* resolved) {
    startRetryChunkLoads(resolved);
    while (!m_deferredChunkLoads.empty() &&
           (m_loadQueueLimit == 0 || m_pendingChunks.size() < m_loadQueueLimit)) {
        Voxel::ChunkCoord coord = m_deferredChunkLoads.front();
        m_deferredChunkLoads.pop_front();
        auto deferredIt = m_deferredChunkRequests.find(coord);
        if (deferredIt == m_deferredChunkRequests.end()) {
            continue;
        }
        ChunkRequestIdentity request = deferredIt->second;
        m_deferredChunkRequests.erase(deferredIt);

        Voxel::ChunkLoadRequestResult result = queueChunkLoad(coord, request);
        if (result != Voxel::ChunkLoadRequestResult::Missing) {
            continue;
        }
        if (resolved) {
            resolved->push_back(
                {coord, request.requestId, Voxel::ChunkLoadOutcome::Missing});
        } else {
            m_resolvedChunks.push_back(
                {coord, request.requestId, Voxel::ChunkLoadOutcome::Missing});
        }
    }
}

void AsyncChunkLoader::startRetryChunkLoads(
    std::vector<Voxel::ChunkLoadCompletion>* resolved) {
    const auto now = retryNow();
    while (!m_retrySchedule.empty()) {
        const ChunkRetrySchedule schedule = m_retrySchedule.top();
        auto retryIt = m_retryChunks.find(schedule.coord);
        if (retryIt == m_retryChunks.end() ||
            retryIt->second.request != schedule.request ||
            retryIt->second.retryAfter != schedule.retryAfter) {
            m_retrySchedule.pop();
            continue;
        }
        if (schedule.retryAfter > now) {
            return;
        }
        if (m_loadQueueLimit > 0 &&
            m_pendingChunks.size() >= m_loadQueueLimit) {
            return;
        }

        m_retrySchedule.pop();
        m_retryChunks.erase(retryIt);
        Voxel::ChunkLoadRequestResult result =
            queueChunkLoad(
                schedule.coord,
                schedule.request,
                RegionAdmissionKind::Retry);
        if (result != Voxel::ChunkLoadRequestResult::Missing) {
            continue;
        }

        m_chunkRetryRounds.erase(schedule.coord);
        Voxel::ChunkLoadCompletion completion{
            schedule.coord,
            schedule.request.requestId,
            Voxel::ChunkLoadOutcome::Missing};
        if (resolved) {
            resolved->push_back(completion);
        } else {
            m_resolvedChunks.push_back(completion);
        }
    }
}

void AsyncChunkLoader::completeChunkLoad(
    Voxel::ChunkCoord coord,
    ChunkRequestIdentity request,
    Voxel::ChunkLoadOutcome outcome,
    std::vector<Voxel::ChunkLoadCompletion>& resolved) {
    auto pendingIt = m_pendingChunks.find(coord);
    if (pendingIt == m_pendingChunks.end() ||
        pendingIt->second != request) {
        return;
    }
    m_pendingChunks.erase(pendingIt);
    m_chunkRetryRounds.erase(coord);
    clearTerminalChunkLoad(coord);
    resolved.push_back({coord, request.requestId, outcome});
    startDeferredChunkLoads(&resolved);
}

void AsyncChunkLoader::scheduleRegionRetry(
    const RegionKey& key,
    ChunkRequestMap pending,
    const std::string& error) {
    const auto now = retryNow();
    for (const auto& [coord, request] : pending) {
        auto activeIt = m_pendingChunks.find(coord);
        if (activeIt == m_pendingChunks.end() ||
            activeIt->second != request) {
            continue;
        }

        m_pendingChunks.erase(activeIt);
        size_t failureRounds = ++m_chunkRetryRounds[coord];
        const auto delay = retryDelay(failureRounds);
        const auto retryAfter = now + delay;
        m_retryChunks[coord] = ChunkRetryState{request, retryAfter};
        m_retrySchedule.push({coord, request, retryAfter});

        spdlog::warn(
            "Region load ({} {} {}) exhausted {} attempts for chunk ({}, {}, {}): {}; retrying in {} ms",
            key.x,
            key.y,
            key.z,
            kMaxRegionLoadAttempts,
            coord.x,
            coord.y,
            coord.z,
            error,
            std::chrono::duration_cast<std::chrono::milliseconds>(delay).count());
    }
    startDeferredChunkLoads();
}

void AsyncChunkLoader::markTerminalChunkLoad(
    Voxel::ChunkCoord coord,
    ChunkRequestIdentity request,
    std::string diagnostic,
    Voxel::ChunkLoadExecutionOwner owner) {
    auto pendingIt = m_pendingChunks.find(coord);
    if (pendingIt == m_pendingChunks.end() ||
        pendingIt->second != request) {
        return;
    }

    m_pendingChunks.erase(pendingIt);
    m_retryChunks.erase(coord);
    m_chunkRetryRounds.erase(coord);
    auto terminalIt = m_terminalChunks.find(coord);
    if (terminalIt == m_terminalChunks.end()) {
        m_terminalChunks.emplace(
            coord,
            TerminalChunkState{std::move(diagnostic), owner});
        advanceFailureVersion(m_terminalFailureVersion);
    } else if (terminalIt->second.diagnostic != diagnostic ||
               terminalIt->second.owner != owner) {
        terminalIt->second = TerminalChunkState{
            std::move(diagnostic), owner};
        advanceFailureVersion(m_terminalFailureVersion);
    }
    refreshLastTerminalError();
}

void AsyncChunkLoader::clearTerminalChunkLoad(Voxel::ChunkCoord coord) {
    if (m_terminalChunks.erase(coord) == 0) {
        return;
    }
    advanceFailureVersion(m_terminalFailureVersion);
    refreshLastTerminalError();
}

void AsyncChunkLoader::refreshLastTerminalError() {
    if (m_terminalChunks.empty()) {
        m_lastTerminalError.clear();
        return;
    }
    m_lastTerminalError = m_terminalChunks.begin()->second.diagnostic;
}

void AsyncChunkLoader::restartChunkLoad(
    Voxel::ChunkCoord coord,
    ChunkRequestIdentity request,
    std::vector<Voxel::ChunkLoadCompletion>& resolved) {
    auto pendingIt = m_pendingChunks.find(coord);
    if (pendingIt == m_pendingChunks.end() ||
        pendingIt->second != request || !m_format) {
        return;
    }

    RegionKey key = m_format->regionLayout().regionForChunk(m_zoneId, coord);
    auto cacheIt = m_cache.find(key);
    if (cacheIt != m_cache.end()) {
        if (cacheIt->second.present.find(coord) == cacheIt->second.present.end()) {
            completeChunkLoad(
                coord,
                request,
                Voxel::ChunkLoadOutcome::Missing,
                resolved);
            return;
        }
        queuePayloadBuild(cacheIt->second, coord, request);
        touch(key);
        return;
    }

    m_regionPending[key][coord] = request;
    queueRegionLoad(key);
}

bool AsyncChunkLoader::applyPayload(const ChunkPayload& payload) {
    if (!m_world) {
        return false;
    }

    if (m_world->chunkManager().getChunk(payload.coord)) {
        return false;
    }

    Voxel::Chunk& chunk = m_world->chunkManager().getOrCreateChunk(payload.coord);

    chunk.copyFrom(payload.blocks.blocks, m_world->blockRegistry());
    chunk.setWorldGenVersion(payload.worldGenVersion);
    chunk.clearPersistDirty();
    chunk.clearDirty();
    chunk.setLoadedFromDisk(payload.loadedFromDisk);

    if (!chunk.isEmpty()) {
        m_world->chunkManager().invalidateFaceNeighbors(payload.coord);
    }

    return true;
}

bool AsyncChunkLoader::queueRegionLoad(const RegionKey& key,
                                       RegionJobOrigin origin,
                                       RegionAdmissionKind admission) {
    if (m_cache.find(key) != m_cache.end()) {
        return false;
    }
    if (m_regionJobs.find(key) != m_regionJobs.end()) {
        if (origin == RegionJobOrigin::Direct) {
            promoteRegionDemand(key);
        }
        return false;
    }

    if (origin == RegionJobOrigin::Speculative) {
        const size_t speculativeLimit = m_maxInFlightRegions > 0
            ? m_maxInFlightRegions
            : kZeroCapSpeculativeRegionQueueLimit;
        if (m_queuedSpeculativeRegionJobCount >= speculativeLimit ||
            (m_maxInFlightRegions > 0 &&
             m_regionJobs.size() >= m_maxInFlightRegions)) {
            return false;
        }
    } else if (m_maxInFlightRegions > 0 &&
               m_regionJobs.size() >= m_maxInFlightRegions) {
        cancelQueuedSpeculativeRegionLoad();
    }

    auto jobState = std::make_shared<RegionJobState>();
    jobState->key = key;
    jobState->origin = origin;
    jobState->admittedAt = metricNow();
    jobState->demanded = origin == RegionJobOrigin::Direct;
    m_regionJobs[key] = jobState;
    if (origin == RegionJobOrigin::Direct) {
        m_directRegionLoads.push_back(key);
        ++m_demandOwnedQueuedRegionJobCount;
    } else {
        m_speculativeRegionLoads.push_back(key);
        ++m_queuedSpeculativeRegionJobCount;
    }
    RegionMetricCounters& counters = regionMetricCounters(origin);
    counters.logicalAdmissions.fetch_add(
        1, std::memory_order_relaxed);
    if (admission == RegionAdmissionKind::Retry) {
        counters.retryAdmissions.fetch_add(1, std::memory_order_relaxed);
    }
    startQueuedRegionLoads();
    return true;
}

void AsyncChunkLoader::startQueuedRegionLoads() {
    size_t dispatchLimit = std::max<size_t>(m_ioPool.threadCount(), 1);
    if (m_maxInFlightRegions > 0) {
        dispatchLimit = std::min(dispatchLimit, m_maxInFlightRegions);
    }

    while (!m_directRegionLoads.empty() &&
           m_inFlight.size() >= dispatchLimit &&
           yieldSubmittedSpeculativeRegionLoad()) {}

    while (m_inFlight.size() < dispatchLimit) {
        std::shared_ptr<RegionJobState> jobState =
            takeQueuedRegionLoad(true);
        if (!jobState) {
            jobState = takeQueuedRegionLoad(false);
        }
        if (!jobState) {
            return;
        }
        startRegionLoad(jobState->key, jobState);
    }
}

std::shared_ptr<AsyncChunkLoader::RegionJobState>
AsyncChunkLoader::takeQueuedRegionLoad(bool direct) {
    std::deque<RegionKey>& queue =
        direct ? m_directRegionLoads : m_speculativeRegionLoads;
    while (!queue.empty()) {
        RegionKey key = queue.front();
        queue.pop_front();
        auto jobIt = m_regionJobs.find(key);
        if (jobIt == m_regionJobs.end() || jobIt->second->started ||
            jobIt->second->demanded != direct) {
            continue;
        }
        jobIt->second->started = true;
        if (direct) {
            --m_demandOwnedQueuedRegionJobCount;
            ++m_demandOwnedDispatchedRegionJobCount;
        } else {
            --m_queuedSpeculativeRegionJobCount;
            ++m_speculativeOwnedDispatchedRegionJobCount;
        }
        return jobIt->second;
    }
    return nullptr;
}

bool AsyncChunkLoader::cancelQueuedSpeculativeRegionLoad() {
    while (!m_speculativeRegionLoads.empty()) {
        RegionKey key = m_speculativeRegionLoads.back();
        m_speculativeRegionLoads.pop_back();
        auto jobIt = m_regionJobs.find(key);
        if (jobIt == m_regionJobs.end() || jobIt->second->started ||
            jobIt->second->demanded) {
            continue;
        }
        m_regionJobs.erase(jobIt);
        --m_queuedSpeculativeRegionJobCount;
        m_speculativeRegionMetrics.logicalPreStartCancellations.fetch_add(
            1, std::memory_order_relaxed);
        return true;
    }
    return false;
}

bool AsyncChunkLoader::reserveQueuedSpeculativeRegionSlot() {
    if (m_maxInFlightRegions != 0 ||
        m_queuedSpeculativeRegionJobCount <
            kZeroCapSpeculativeRegionQueueLimit) {
        return true;
    }
    return cancelQueuedSpeculativeRegionLoad();
}

bool AsyncChunkLoader::yieldSubmittedSpeculativeRegionLoad() {
    ++m_speculativeYieldCalls;
    size_t visits = 0;
    for (auto it = m_submittedSpeculativeRegionJobs.begin();
         it != m_submittedSpeculativeRegionJobs.end();) {
        ++visits;
        const std::shared_ptr<RegionJobState> job = *it;
        auto ownerIt = m_regionJobs.find(job->key);
        if (ownerIt == m_regionJobs.end() || ownerIt->second != job ||
            !job->started || job->demanded ||
            job->origin != RegionJobOrigin::Speculative ||
            job->poolJobId == 0 ||
            !job->speculativePoolPending.load(std::memory_order_relaxed)) {
            it = m_submittedSpeculativeRegionJobs.erase(it);
            continue;
        }
        if (!m_ioPool.cancel(job->poolJobId)) {
            ++it;
            continue;
        }
        it = m_submittedSpeculativeRegionJobs.erase(it);
        retireSpeculativeRegionPoolPending(job);
        m_inFlight.erase(job->key);
        undoRegionLoadAttempt(job->key);
        --m_speculativeOwnedDispatchedRegionJobCount;
        job->poolJobId = 0;
        job->started = false;
        job->phase.store(
            Voxel::ChunkLoadExecutionPhase::SchedulerPending,
            std::memory_order_release);
        if (reserveQueuedSpeculativeRegionSlot()) {
            ++m_queuedSpeculativeRegionJobCount;
            m_speculativeRegionLoads.push_front(job->key);
        } else {
            m_regionJobs.erase(job->key);
            m_speculativeRegionMetrics.logicalPreStartCancellations.fetch_add(
                1, std::memory_order_relaxed);
        }
        regionMetricCounters(job->origin).successfulPoolYields.fetch_add(
            1, std::memory_order_relaxed);
        m_speculativeYieldCandidateVisits += visits;
        m_maxSpeculativeYieldCandidateVisits =
            std::max(m_maxSpeculativeYieldCandidateVisits, visits);
        return true;
    }
    m_speculativeYieldCandidateVisits += visits;
    m_maxSpeculativeYieldCandidateVisits =
        std::max(m_maxSpeculativeYieldCandidateVisits, visits);
    return false;
}

void AsyncChunkLoader::trackSubmittedSpeculativeRegionJob(
    const std::shared_ptr<RegionJobState>& job) {
    if (!job->speculativePoolPending.load(std::memory_order_relaxed)) {
        return;
    }
    auto tracked = std::find(
        m_submittedSpeculativeRegionJobs.begin(),
        m_submittedSpeculativeRegionJobs.end(),
        job);
    if (tracked != m_submittedSpeculativeRegionJobs.end()) {
        return;
    }
    m_submittedSpeculativeRegionJobs.push_back(job);
}

void AsyncChunkLoader::markSpeculativeRegionPoolPending(
    const std::shared_ptr<RegionJobState>& job) {
    if (job->speculativePoolPending.exchange(
            true, std::memory_order_relaxed)) {
        return;
    }
    const size_t pending = m_speculativePoolJobsPending.fetch_add(
        1, std::memory_order_relaxed) + 1;
    m_maxSpeculativePoolJobsPending = std::max(
        m_maxSpeculativePoolJobsPending, pending);
}

void AsyncChunkLoader::retireSpeculativeRegionPoolPending(
    const std::shared_ptr<RegionJobState>& job) {
    if (job && job->speculativePoolPending.exchange(
                   false, std::memory_order_relaxed)) {
        m_speculativePoolJobsPending.fetch_sub(1, std::memory_order_relaxed);
    }
}

bool AsyncChunkLoader::retireSubmittedSpeculativeRegionJob(
    const std::shared_ptr<RegionJobState>& job) {
    retireSpeculativeRegionPoolPending(job);
    auto tracked = std::find(
        m_submittedSpeculativeRegionJobs.begin(),
        m_submittedSpeculativeRegionJobs.end(),
        job);
    if (tracked == m_submittedSpeculativeRegionJobs.end()) {
        return false;
    }
    m_submittedSpeculativeRegionJobs.erase(tracked);
    return true;
}

void AsyncChunkLoader::cancelQueuedDirectRegionLoad(const RegionKey& key) {
    auto jobIt = m_regionJobs.find(key);
    if (jobIt == m_regionJobs.end() || !jobIt->second->demanded) {
        return;
    }
    if (jobIt->second->origin == RegionJobOrigin::Speculative) {
        if (jobIt->second->started) {
            --m_demandOwnedDispatchedRegionJobCount;
            ++m_speculativeOwnedDispatchedRegionJobCount;
        } else {
            --m_demandOwnedQueuedRegionJobCount;
        }
        jobIt->second->demanded = false;
        if (jobIt->second->started && jobIt->second->poolJobId != 0 &&
            m_ioPool.cancel(jobIt->second->poolJobId)) {
            retireSubmittedSpeculativeRegionJob(jobIt->second);
            m_inFlight.erase(key);
            undoRegionLoadAttempt(key);
            --m_speculativeOwnedDispatchedRegionJobCount;
            m_speculativeRegionMetrics.successfulPoolYields.fetch_add(
                1, std::memory_order_relaxed);
            jobIt->second->poolJobId = 0;
            jobIt->second->started = false;
            jobIt->second->phase.store(
                Voxel::ChunkLoadExecutionPhase::SchedulerPending,
                std::memory_order_release);
        }
        if (!jobIt->second->started) {
            eraseQueuedRegion(m_directRegionLoads, key);
            if (reserveQueuedSpeculativeRegionSlot()) {
                ++m_queuedSpeculativeRegionJobCount;
                m_speculativeRegionLoads.push_back(key);
            } else {
                m_regionJobs.erase(jobIt);
                m_speculativeRegionMetrics.logicalPreStartCancellations.fetch_add(
                    1, std::memory_order_relaxed);
            }
        }
        return;
    }
    if (jobIt->second->started) {
        if (jobIt->second->poolJobId == 0 ||
            !m_ioPool.cancel(jobIt->second->poolJobId)) {
            --m_demandOwnedDispatchedRegionJobCount;
            ++m_speculativeOwnedDispatchedRegionJobCount;
            jobIt->second->demanded = false;
            return;
        }
        m_inFlight.erase(key);
        undoRegionLoadAttempt(key);
        --m_demandOwnedDispatchedRegionJobCount;
        m_directRegionMetrics.terminalPoolCancellations.fetch_add(
            1, std::memory_order_relaxed);
    } else {
        --m_demandOwnedQueuedRegionJobCount;
    }
    eraseQueuedRegion(m_directRegionLoads, key);
    m_regionJobs.erase(jobIt);
    m_directRegionMetrics.logicalPreStartCancellations.fetch_add(
        1, std::memory_order_relaxed);
}

bool AsyncChunkLoader::hasDirectRegionDemand(const RegionKey& key) const {
    auto pendingIt = m_regionPending.find(key);
    if (pendingIt != m_regionPending.end() && !pendingIt->second.empty()) {
        return true;
    }
    for (const auto& [coord, request] : m_deferredChunkRequests) {
        (void)request;
        if (m_format->regionLayout().regionForChunk(m_zoneId, coord) == key) {
            return true;
        }
    }
    return false;
}

void AsyncChunkLoader::undoRegionLoadAttempt(const RegionKey& key) {
    auto attemptsIt = m_regionLoadAttempts.find(key);
    if (attemptsIt == m_regionLoadAttempts.end()) {
        return;
    }
    if (attemptsIt->second > 1) {
        --attemptsIt->second;
    } else {
        m_regionLoadAttempts.erase(attemptsIt);
    }
}

void AsyncChunkLoader::startRegionLoad(
    const RegionKey& key,
    const std::shared_ptr<RegionJobState>& jobState) {
    m_inFlight.insert(key);
    ++m_regionLoadAttempts[key];
    uint64_t revision = m_regionRevisions[key];
    PersistenceService* servicePtr = m_service;
    PersistenceContext contextCopy = m_context;
    auto regionLoadStartCallback = m_regionLoadStartCallback;
    auto regionLoadStartObserver = m_regionLoadStartObserver;
    auto regionResultReadyToPublishCallback =
        m_regionResultReadyToPublishCallback;
    const bool poolExecution = m_ioPool.threadCount() > 0;

    auto job = [this,
                servicePtr,
                contextCopy,
                key,
                revision,
                jobState,
                poolExecution,
                regionLoadStartCallback = std::move(regionLoadStartCallback),
                regionLoadStartObserver = std::move(regionLoadStartObserver),
                regionResultReadyToPublishCallback =
                    std::move(regionResultReadyToPublishCallback)]() mutable {
        jobState->phase.store(
            Voxel::ChunkLoadExecutionPhase::WorkerRunning,
            std::memory_order_release);
        if (poolExecution) {
            retireSpeculativeRegionPoolPending(jobState);
        }
        const auto workerStart = metricNow();
        RegionMetricCounters& counters =
            regionMetricCounters(jobState->origin);
        (poolExecution ? counters.poolWorkerStarts : counters.inlineExecutions)
            .fetch_add(1, std::memory_order_seq_cst);
        const uint64_t admissionToStart =
            nanosecondsBetween(jobState->admittedAt, workerStart);
        counters.admissionToWorkerStartNanoseconds.fetch_add(
            admissionToStart, std::memory_order_relaxed);
        recordMaximum(
            counters.maxAdmissionToWorkerStartNanoseconds,
            admissionToStart);
        if (regionLoadStartCallback) {
            regionLoadStartCallback();
        }
        if (regionLoadStartObserver) {
            regionLoadStartObserver(key, jobState->origin);
        }
        RegionResult result;
        result.key = key;
        result.revision = revision;
        result.job = jobState;
        auto recordExecutionMetrics = [&]() {
            const uint64_t workerExecution = nanosecondsBetween(
                workerStart, metricNow());
            counters.workerExecutionNanoseconds.fetch_add(
                workerExecution, std::memory_order_relaxed);
            recordMaximum(
                counters.maxWorkerExecutionNanoseconds, workerExecution);
        };
        try {
            auto jobFormat = servicePtr->openFormat(contextCopy);
            result.exists = jobFormat->chunkContainer().regionExists(key);
            if (!result.exists) {
                counters.missingProbes.fetch_add(1, std::memory_order_relaxed);
                RegionEntry entry;
                entry.region = std::make_shared<ChunkRegionSnapshot>();
                entry.region->key = key;
                result.entry = std::move(entry);
                result.ok = true;
            } else {
                ChunkRegionSnapshot region =
                    jobFormat->chunkContainer().loadRegion(key);
                RegionEntry entry;
                entry.region =
                    std::make_shared<ChunkRegionSnapshot>(std::move(region));
                entry.present.reserve(entry.region->chunks.size());
                entry.spansByCoord.reserve(entry.region->chunks.size());
                for (const auto& snapshot : entry.region->chunks) {
                    const ChunkSpan& span = snapshot.data.span;
                    Voxel::ChunkCoord coord{
                        span.chunkX, span.chunkY, span.chunkZ};
                    entry.present.insert(coord);
                    entry.spansByCoord[coord].push_back(&snapshot);
                }
                result.entry = std::move(entry);
                result.ok = true;
            }
        } catch (const StorageReadError& e) {
            result.ok = false;
            result.exists = false;
            result.retryable = true;
            result.error = e.what();
        } catch (const std::system_error& e) {
            result.ok = false;
            result.exists = false;
            result.retryable = true;
            result.error = e.what();
        } catch (const std::exception& e) {
            result.ok = false;
            result.exists = false;
            result.retryable = false;
            result.error = e.what();
        } catch (...) {
            result.ok = false;
            result.exists = false;
            result.retryable = false;
            result.error = "unknown error";
        }
        recordExecutionMetrics();
        if (regionResultReadyToPublishCallback) {
            regionResultReadyToPublishCallback();
        }
        m_regionComplete.push(
            std::move(result),
            [&counters, jobState]() noexcept {
                jobState->phase.store(
                    Voxel::ChunkLoadExecutionPhase::ResultPublished,
                    std::memory_order_release);
                counters.resultsPublished.fetch_add(
                    1, std::memory_order_seq_cst);
            });
    };

    if (poolExecution) {
        RegionMetricCounters& counters =
            regionMetricCounters(jobState->origin);
        const bool resubmission = jobState->poolSubmissionCount > 0;
        const bool speculativePoolPending =
            jobState->origin == RegionJobOrigin::Speculative &&
            !jobState->demanded;
        if (speculativePoolPending) {
            markSpeculativeRegionPoolPending(jobState);
        }
        jobState->poolJobId = m_ioPool.enqueue(
            std::move(job),
            jobState->demanded
                ? Voxel::detail::ThreadPool::Priority::High
                : Voxel::detail::ThreadPool::Priority::Normal,
            Voxel::detail::ThreadPool::SubmissionCommitAccounting{
                counters.poolSubmissions,
                resubmission ? &counters.poolResubmissions : nullptr});
        if (jobState->poolJobId != 0) {
            ++jobState->poolSubmissionCount;
            auto expected =
                Voxel::ChunkLoadExecutionPhase::SchedulerPending;
            jobState->phase.compare_exchange_strong(
                expected,
                Voxel::ChunkLoadExecutionPhase::PoolQueued,
                std::memory_order_release,
                std::memory_order_relaxed);
        } else if (speculativePoolPending) {
            retireSpeculativeRegionPoolPending(jobState);
        }
        if (jobState->poolJobId != 0 && speculativePoolPending) {
            trackSubmittedSpeculativeRegionJob(jobState);
        }
    } else {
        job();
    }
}

void AsyncChunkLoader::queuePayloadBuild(
    const RegionEntry& entry,
    Voxel::ChunkCoord coord,
    ChunkRequestIdentity request) {
    if (!m_generator || !m_world) {
        return;
    }
    auto pendingIt = m_pendingChunks.find(coord);
    if (pendingIt == m_pendingChunks.end() ||
        pendingIt->second != request) {
        return;
    }
    if (m_payloadInFlight.find(coord) != m_payloadInFlight.end()) {
        return;
    }
    auto spanIt = entry.spansByCoord.find(coord);
    if (spanIt == entry.spansByCoord.end()) {
        return;
    }
    if (!entry.region) {
        return;
    }

    auto payloadJob = std::make_shared<PayloadJobState>();
    payloadJob->request = request;
    m_payloadInFlight[coord] = payloadJob;
    auto generator = m_generator;
    auto registry = &m_world->blockRegistry();
    std::vector<const ChunkSnapshot*> spans = spanIt->second;
    std::shared_ptr<ChunkRegionSnapshot> region = entry.region;
    RegionKey regionKey = region->key;
    uint64_t regionRevision = m_regionRevisions[regionKey];
    auto payloadBuildStartCallback = m_payloadBuildStartCallback;
    auto payloadResultPublishedObserver =
        m_payloadResultPublishedObserver;

    auto job = [this,
                coord,
                request,
                spans = std::move(spans),
                generator,
                registry,
                region,
                regionKey,
                regionRevision,
                payloadJob,
                payloadBuildStartCallback = std::move(payloadBuildStartCallback),
                payloadResultPublishedObserver =
                    std::move(payloadResultPublishedObserver)]() mutable {
        payloadJob->phase.store(
            Voxel::ChunkLoadExecutionPhase::WorkerRunning,
            std::memory_order_release);
        ChunkPayload payload;
        payload.coord = coord;
        payload.request = request;
        payload.regionKey = regionKey;
        payload.regionRevision = regionRevision;
        payload.job = payloadJob;
        payload.worldGenVersion = generator ? generator->config().world.version : 0;
        payload.loadedFromDisk = true;
        try {
            if (payloadBuildStartCallback) {
                payloadBuildStartCallback();
            }
            if (!region) {
                payload.cancelled = true;
                m_chunkComplete.push(
                    std::move(payload),
                    [payloadJob,
                     coord,
                     payloadResultPublishedObserver]() noexcept {
                        payloadJob->phase.store(
                            Voxel::ChunkLoadExecutionPhase::ResultPublished,
                            std::memory_order_release);
                        if (payloadResultPublishedObserver) {
                            try {
                                payloadResultPublishedObserver(coord);
                            } catch (...) {
                            }
                        }
                    });
                return;
            }

            Voxel::Chunk temp(coord);
            ChunkBaseFillFn baseFill;
            bool allowBaseFill = false;
            if (m_format) {
                allowBaseFill =
                    m_format->descriptor().capabilities.fillMissingChunkSpans;
            }
            if (allowBaseFill && generator) {
                baseFill = [generator, coord](
                    Voxel::Chunk& target,
                    const Voxel::BlockRegistry& reg) {
                    Voxel::ChunkBuffer buffer;
                    generator->generate(coord, buffer, nullptr);
                    target.copyFrom(buffer.blocks, reg);
                    target.clearPersistDirty();
                };
            }
            auto mergeResult = mergeChunkSpans(temp, *registry, spans, baseFill);
            temp.copyBlocks(payload.blocks.blocks);
            payload.empty = temp.isEmpty();
            payload.cancelled = false;
            payload.loadedFromDisk = mergeResult.loadedFromDisk;
        } catch (const std::exception& e) {
            payload.failed = true;
            payload.error = e.what();
        } catch (...) {
            payload.failed = true;
            payload.error = "unknown error";
        }
        m_chunkComplete.push(
            std::move(payload),
            [payloadJob,
             coord,
             payloadResultPublishedObserver]() noexcept {
                payloadJob->phase.store(
                    Voxel::ChunkLoadExecutionPhase::ResultPublished,
                    std::memory_order_release);
                if (payloadResultPublishedObserver) {
                    try {
                        payloadResultPublishedObserver(coord);
                    } catch (...) {
                    }
                }
            });
    };

    if (m_workerPool.threadCount() > 0) {
        const auto jobId = m_workerPool.enqueue(std::move(job));
        if (jobId != 0) {
            auto expected =
                Voxel::ChunkLoadExecutionPhase::SchedulerPending;
            payloadJob->phase.compare_exchange_strong(
                expected,
                Voxel::ChunkLoadExecutionPhase::PoolQueued,
                std::memory_order_release,
                std::memory_order_relaxed);
        }
    } else {
        job();
    }
}

void AsyncChunkLoader::invalidateRegion(const RegionKey& key) {
    uint64_t next = m_regionRevisions[key] + 1;
    m_regionRevisions[key] = next == 0 ? 1 : next;
    m_cache.erase(key);
    auto lruIt = std::find(m_lru.begin(), m_lru.end(), key);
    if (lruIt != m_lru.end()) {
        m_lru.erase(lruIt);
    }
    RegionPresence& presence = m_regionPresence[key];
    presence.exists = true;
    presence.nextCheck = std::chrono::steady_clock::time_point{};
}

void AsyncChunkLoader::prefetchNeighbors(const RegionKey& center) {
    if (m_prefetchRadius <= 0) {
        return;
    }
    struct Candidate {
        int distSq;
        int dx;
        int dy;
        int dz;
    };
    std::vector<Candidate> candidates;
    int radius = std::max(1, m_prefetchRadius);
    for (int dz = -radius; dz <= radius; ++dz) {
        for (int dy = -radius; dy <= radius; ++dy) {
            for (int dx = -radius; dx <= radius; ++dx) {
                if (dx == 0 && dy == 0 && dz == 0) {
                    continue;
                }
                int distSq = dx * dx + dy * dy + dz * dz;
                candidates.push_back({distSq, dx, dy, dz});
            }
        }
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) {
                  return a.distSq < b.distSq;
              });

    size_t queued = 0;
    size_t limit = m_prefetchPerRequest;
    if (limit == 0) {
        limit = std::numeric_limits<size_t>::max();
    }
    for (const Candidate& candidate : candidates) {
        if (queued >= limit) {
            break;
        }
        RegionKey neighbor = center;
        neighbor.x += candidate.dx;
        neighbor.y += candidate.dy;
        neighbor.z += candidate.dz;
        if (queueRegionLoad(neighbor, RegionJobOrigin::Speculative)) {
            ++queued;
        }
    }
}

void AsyncChunkLoader::touch(const RegionKey& key) {
    auto it = std::find(m_lru.begin(), m_lru.end(), key);
    if (it != m_lru.end()) {
        m_lru.erase(it);
    }
    m_lru.push_back(key);
}

void AsyncChunkLoader::evictIfNeeded() {
    if (m_maxCachedRegions == 0) {
        return;
    }
    while (m_cache.size() > m_maxCachedRegions && !m_lru.empty()) {
        RegionKey key = m_lru.front();
        m_lru.pop_front();
        auto cacheIt = m_cache.find(key);
        if (cacheIt == m_cache.end()) {
            continue;
        }
        if (cacheIt->second.prefetched) {
            m_speculativeEvictionsBeforeDemand.fetch_add(
                1, std::memory_order_relaxed);
        }
        m_cache.erase(cacheIt);
    }
}

void AsyncChunkLoader::promoteRegionDemand(const RegionKey& key) {
    auto jobIt = m_regionJobs.find(key);
    if (jobIt == m_regionJobs.end() || jobIt->second->demanded) {
        return;
    }
    if (jobIt->second->started) {
        --m_speculativeOwnedDispatchedRegionJobCount;
        ++m_demandOwnedDispatchedRegionJobCount;
    } else {
        --m_queuedSpeculativeRegionJobCount;
        ++m_demandOwnedQueuedRegionJobCount;
    }
    jobIt->second->demanded = true;
    if (!jobIt->second->started) {
        eraseQueuedRegion(m_speculativeRegionLoads, key);
        m_directRegionLoads.push_back(key);
    } else if (jobIt->second->poolJobId != 0) {
        retireSubmittedSpeculativeRegionJob(jobIt->second);
        m_ioPool.promote(jobIt->second->poolJobId);
    }
    m_demandPromotions.fetch_add(1, std::memory_order_relaxed);
    startQueuedRegionLoads();
}

AsyncChunkLoader::RegionMetricCounters&
AsyncChunkLoader::regionMetricCounters(RegionJobOrigin origin) {
    return origin == RegionJobOrigin::Direct
        ? m_directRegionMetrics
        : m_speculativeRegionMetrics;
}

const AsyncChunkLoader::RegionMetricCounters&
AsyncChunkLoader::regionMetricCounters(RegionJobOrigin origin) const {
    return origin == RegionJobOrigin::Direct
        ? m_directRegionMetrics
        : m_speculativeRegionMetrics;
}

Voxel::RegionSchedulerOriginDiagnostics AsyncChunkLoader::regionJobMetrics(
    const RegionMetricCounters& counters,
    std::atomic<bool>* subsetReadEntered,
    std::atomic<bool>* subsetReadReleased) {
    const uint64_t resultsDrained =
        counters.resultsDrained.load(std::memory_order_seq_cst);
    const uint64_t resultsPublished =
        counters.resultsPublished.load(std::memory_order_seq_cst);
    const uint64_t inlineExecutions =
        counters.inlineExecutions.load(std::memory_order_seq_cst);
    const uint64_t poolWorkerStarts =
        counters.poolWorkerStarts.load(std::memory_order_seq_cst);
    const uint64_t poolResubmissions =
        counters.poolResubmissions.load(std::memory_order_seq_cst);
    if (subsetReadEntered && subsetReadReleased) {
        subsetReadEntered->store(true, std::memory_order_release);
        subsetReadEntered->notify_all();
        while (!subsetReadReleased->load(std::memory_order_acquire)) {
            subsetReadReleased->wait(false, std::memory_order_acquire);
        }
    }
    const uint64_t poolSubmissions =
        counters.poolSubmissions.load(std::memory_order_seq_cst);
    return Voxel::RegionSchedulerOriginDiagnostics{
        .logicalAdmissions =
            counters.logicalAdmissions.load(std::memory_order_relaxed),
        .retryAdmissions =
            counters.retryAdmissions.load(std::memory_order_relaxed),
        .logicalPreStartCancellations =
            counters.logicalPreStartCancellations.load(
                std::memory_order_relaxed),
        .poolSubmissions = poolSubmissions,
        .poolResubmissions = poolResubmissions,
        .successfulPoolYields =
            counters.successfulPoolYields.load(std::memory_order_relaxed),
        .terminalPoolCancellations =
            counters.terminalPoolCancellations.load(
                std::memory_order_relaxed),
        .poolWorkerStarts = poolWorkerStarts,
        .inlineExecutions = inlineExecutions,
        .resultsPublished = resultsPublished,
        .resultsDrained = resultsDrained,
        .missingProbes = counters.missingProbes.load(std::memory_order_relaxed),
        .admissionToWorkerStartNanoseconds =
            counters.admissionToWorkerStartNanoseconds.load(
                std::memory_order_relaxed),
        .maxAdmissionToWorkerStartNanoseconds =
            counters.maxAdmissionToWorkerStartNanoseconds.load(
                std::memory_order_relaxed),
        .workerExecutionNanoseconds =
            counters.workerExecutionNanoseconds.load(
                std::memory_order_relaxed),
        .maxWorkerExecutionNanoseconds =
            counters.maxWorkerExecutionNanoseconds.load(
                std::memory_order_relaxed)
    };
}

int AsyncChunkLoader::estimateRegionSpan() const {
    if (!m_format) {
        return 1;
    }
    Voxel::ChunkCoord origin{0, 0, 0};
    RegionKey base = m_format->regionLayout().regionForChunk(m_zoneId, origin);
    constexpr int kMaxSpan = 64;
    for (int offset = 1; offset <= kMaxSpan; ++offset) {
        Voxel::ChunkCoord probe{offset, 0, 0};
        RegionKey key = m_format->regionLayout().regionForChunk(m_zoneId, probe);
        if (!(key == base)) {
            return offset;
        }
    }
    return kMaxSpan;
}

bool AsyncChunkLoader::regionMayExist(const RegionKey& key) {
    if (!m_format) {
        return false;
    }
    auto now = std::chrono::steady_clock::now();
    auto presenceIt = m_regionPresence.find(key);
    if (presenceIt != m_regionPresence.end()) {
        if (presenceIt->second.exists) {
            return true;
        }
        if (now < presenceIt->second.nextCheck) {
            return false;
        }
    }
    return true;
}

} // namespace Rigel::Persistence
