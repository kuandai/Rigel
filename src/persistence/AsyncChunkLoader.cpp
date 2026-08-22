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
constexpr auto kInitialRegionRetryDelay = std::chrono::milliseconds(100);
constexpr auto kMaxRegionRetryDelay = std::chrono::seconds(2);

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

AsyncChunkLoader::ChunkLoadRequestId
AsyncChunkLoader::nextChunkLoadRequestId() {
    ChunkLoadRequestId requestId = m_nextChunkLoadRequestId++;
    if (m_nextChunkLoadRequestId == 0) {
        m_nextChunkLoadRequestId = 1;
    }
    return requestId;
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
    m_workerPool.stop(m_workerPoolStopStartCallback);
}

void AsyncChunkLoader::setMaxCachedRegions(size_t maxRegions) {
    m_maxCachedRegions = maxRegions;
}

void AsyncChunkLoader::setMaxInFlightRegions(size_t maxRegions) {
    m_maxInFlightRegions = maxRegions;
    startDeferredRegionLoads();
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

Voxel::ChunkLoadRequestResult AsyncChunkLoader::request(Voxel::ChunkCoord coord) {
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
    if (cacheIt != m_cache.end() &&
        cacheIt->second.present.find(coord) == cacheIt->second.present.end()) {
        return Voxel::ChunkLoadRequestResult::Missing;
    }
    if (cacheIt == m_cache.end() &&
        m_inFlight.find(key) == m_inFlight.end() &&
        !regionMayExist(key)) {
        return Voxel::ChunkLoadRequestResult::Missing;
    }

    if (m_loadQueueLimit > 0 && m_pendingChunks.size() >= m_loadQueueLimit) {
        deferChunkLoad(coord, nextChunkLoadRequestId());
        ++m_requestsStarted;
        return Voxel::ChunkLoadRequestResult::Deferred;
    }

    Voxel::ChunkLoadRequestResult result =
        queueChunkLoad(coord, nextChunkLoadRequestId());
    if (result == Voxel::ChunkLoadRequestResult::Queued) {
        ++m_requestsStarted;
    }
    return result;
}

Voxel::ChunkLoadRequestResult AsyncChunkLoader::queueChunkLoad(
    Voxel::ChunkCoord coord,
    ChunkLoadRequestId requestId) {
    RegionKey key = m_format->regionLayout().regionForChunk(m_zoneId, coord);
    auto cacheIt = m_cache.find(key);
    if (cacheIt != m_cache.end()) {
        if (cacheIt->second.present.find(coord) == cacheIt->second.present.end()) {
            return Voxel::ChunkLoadRequestResult::Missing;
        }
        m_pendingChunks[coord] = requestId;
        queuePayloadBuild(cacheIt->second, coord, requestId);
        touch(key);
        return Voxel::ChunkLoadRequestResult::Queued;
    }
    if (m_inFlight.find(key) == m_inFlight.end()) {
        if (!regionMayExist(key)) {
            return Voxel::ChunkLoadRequestResult::Missing;
        }
    }

    m_pendingChunks[coord] = requestId;
    m_regionPending[key][coord] = requestId;
    if (queueRegionLoad(key)) {
        prefetchNeighbors(key);
    } else if (m_cache.find(key) == m_cache.end() &&
               m_inFlight.find(key) == m_inFlight.end()) {
        deferRegionLoad(key);
    }
    return Voxel::ChunkLoadRequestResult::Queued;
}

bool AsyncChunkLoader::isPending(Voxel::ChunkCoord coord) const {
    return m_pendingChunks.find(coord) != m_pendingChunks.end() ||
        m_deferredChunkRequests.find(coord) != m_deferredChunkRequests.end() ||
        m_retryChunks.find(coord) != m_retryChunks.end() ||
        m_terminalChunks.find(coord) != m_terminalChunks.end();
}

Voxel::StreamingWorkCount AsyncChunkLoader::workCount() const {
    return Voxel::StreamingWorkCount{
        .pending = m_pendingChunks.size() + m_deferredChunkRequests.size() +
            m_retryChunks.size() + m_terminalChunks.size(),
        .inFlight = m_inFlight.size() + m_payloadInFlight.size(),
        .started = m_requestsStarted,
        .terminalErrors = m_terminalChunks.size(),
        .lastError = m_lastTerminalError
    };
}

void AsyncChunkLoader::cancel(Voxel::ChunkCoord coord) {
    bool releasedCapacity = m_pendingChunks.erase(coord) > 0;
    m_deferredChunkRequests.erase(coord);
    m_retryChunks.erase(coord);
    m_chunkRetryRounds.erase(coord);
    if (m_terminalChunks.erase(coord) > 0) {
        refreshLastTerminalError();
    }
    if (!m_format) {
        return;
    }
    RegionKey key = m_format->regionLayout().regionForChunk(m_zoneId, coord);
    auto it = m_regionPending.find(key);
    if (it != m_regionPending.end()) {
        it->second.erase(coord);
        if (it->second.empty()) {
            m_regionPending.erase(it);
            m_deferredRegionLoadSet.erase(key);
        }
    }
    if (releasedCapacity) {
        startDeferredChunkLoads();
    }
    startDeferredRegionLoads();
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
        auto pendingIt = m_regionPending.find(result.key);
        if (result.revision != m_regionRevisions[result.key]) {
            m_regionLoadAttempts.erase(result.key);
            if (pendingIt != m_regionPending.end() && !pendingIt->second.empty() &&
                !queueRegionLoad(result.key)) {
                deferRegionLoad(result.key);
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
                if (!queueRegionLoad(result.key)) {
                    deferRegionLoad(result.key);
                }
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

            for (const auto& [coord, requestId] : pending) {
                std::string diagnostic =
                    regionDecodeDiagnostic(result.key, coord, result.error);
                spdlog::error("{}", diagnostic);
                markTerminalChunkLoad(
                    coord,
                    requestId,
                    std::move(diagnostic));
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
        m_cache[result.key] = std::move(result.entry);
        touch(result.key);
        evictIfNeeded();

        if (pendingIt != m_regionPending.end()) {
            auto pending = std::move(pendingIt->second);
            m_regionPending.erase(pendingIt);
            auto cacheIt = m_cache.find(result.key);
            if (cacheIt != m_cache.end()) {
                for (const auto& [coord, requestId] : pending) {
                    if (cacheIt->second.present.find(coord) == cacheIt->second.present.end()) {
                        completeChunkLoad(
                            coord,
                            requestId,
                            Voxel::ChunkLoadOutcome::Missing,
                            resolved);
                        continue;
                    }
                    queuePayloadBuild(cacheIt->second, coord, requestId);
                }
            }
        }
    }
    startDeferredRegionLoads();
}

void AsyncChunkLoader::drainPayloadCompletions(
    size_t budget,
    std::vector<Voxel::ChunkLoadCompletion>& resolved) {
    size_t applied = 0;
    ChunkPayload payload;
    while (applied < budget && m_chunkComplete.tryPop(payload)) {
        auto flightIt = m_payloadInFlight.find(payload.coord);
        if (flightIt != m_payloadInFlight.end() &&
            flightIt->second == payload.requestId) {
            m_payloadInFlight.erase(flightIt);
        }

        auto pendingIt = m_pendingChunks.find(payload.coord);
        if (payload.cancelled || pendingIt == m_pendingChunks.end()) {
            continue;
        }
        if (pendingIt->second != payload.requestId) {
            if (m_payloadInFlight.find(payload.coord) == m_payloadInFlight.end()) {
                restartChunkLoad(payload.coord, pendingIt->second, resolved);
            }
            continue;
        }
        if (payload.regionRevision != m_regionRevisions[payload.regionKey]) {
            restartChunkLoad(payload.coord, payload.requestId, resolved);
            continue;
        }
        if (payload.failed) {
            std::string diagnostic =
                chunkPayloadDiagnostic(payload.coord, payload.error);
            spdlog::error("{}", diagnostic);
            markTerminalChunkLoad(
                payload.coord,
                payload.requestId,
                std::move(diagnostic));
            startDeferredChunkLoads(&resolved);
            ++applied;
            continue;
        }
        applyPayload(payload);
        completeChunkLoad(
            payload.coord,
            payload.requestId,
            Voxel::ChunkLoadOutcome::Loaded,
            resolved);
        ++applied;
    }
}

void AsyncChunkLoader::deferChunkLoad(
    Voxel::ChunkCoord coord,
    ChunkLoadRequestId requestId) {
    if (m_deferredChunkRequests.emplace(coord, requestId).second) {
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
        ChunkLoadRequestId requestId = deferredIt->second;
        m_deferredChunkRequests.erase(deferredIt);

        Voxel::ChunkLoadRequestResult result = queueChunkLoad(coord, requestId);
        if (result != Voxel::ChunkLoadRequestResult::Missing) {
            continue;
        }
        if (resolved) {
            resolved->push_back(
                {coord, Voxel::ChunkLoadOutcome::Missing});
        } else {
            m_resolvedChunks.push_back(
                {coord, Voxel::ChunkLoadOutcome::Missing});
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
            retryIt->second.requestId != schedule.requestId ||
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
            queueChunkLoad(schedule.coord, schedule.requestId);
        if (result != Voxel::ChunkLoadRequestResult::Missing) {
            continue;
        }

        m_chunkRetryRounds.erase(schedule.coord);
        Voxel::ChunkLoadCompletion completion{
            schedule.coord,
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
    ChunkLoadRequestId requestId,
    Voxel::ChunkLoadOutcome outcome,
    std::vector<Voxel::ChunkLoadCompletion>& resolved) {
    auto pendingIt = m_pendingChunks.find(coord);
    if (pendingIt == m_pendingChunks.end() ||
        pendingIt->second != requestId) {
        return;
    }
    m_pendingChunks.erase(pendingIt);
    m_chunkRetryRounds.erase(coord);
    resolved.push_back({coord, outcome});
    startDeferredChunkLoads(&resolved);
}

void AsyncChunkLoader::scheduleRegionRetry(
    const RegionKey& key,
    ChunkRequestMap pending,
    const std::string& error) {
    const auto now = retryNow();
    for (const auto& [coord, requestId] : pending) {
        auto activeIt = m_pendingChunks.find(coord);
        if (activeIt == m_pendingChunks.end() ||
            activeIt->second != requestId) {
            continue;
        }

        m_pendingChunks.erase(activeIt);
        size_t failureRounds = ++m_chunkRetryRounds[coord];
        const auto delay = retryDelay(failureRounds);
        const auto retryAfter = now + delay;
        m_retryChunks[coord] = ChunkRetryState{requestId, retryAfter};
        m_retrySchedule.push({coord, requestId, retryAfter});

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
    ChunkLoadRequestId requestId,
    std::string diagnostic) {
    auto pendingIt = m_pendingChunks.find(coord);
    if (pendingIt == m_pendingChunks.end() ||
        pendingIt->second != requestId) {
        return;
    }

    m_pendingChunks.erase(pendingIt);
    m_retryChunks.erase(coord);
    m_chunkRetryRounds.erase(coord);
    m_lastTerminalError = diagnostic;
    m_terminalChunks[coord] = std::move(diagnostic);
}

void AsyncChunkLoader::refreshLastTerminalError() {
    if (m_terminalChunks.empty()) {
        m_lastTerminalError.clear();
        return;
    }
    m_lastTerminalError = m_terminalChunks.begin()->second;
}

void AsyncChunkLoader::restartChunkLoad(
    Voxel::ChunkCoord coord,
    ChunkLoadRequestId requestId,
    std::vector<Voxel::ChunkLoadCompletion>& resolved) {
    auto pendingIt = m_pendingChunks.find(coord);
    if (pendingIt == m_pendingChunks.end() ||
        pendingIt->second != requestId || !m_format) {
        return;
    }

    RegionKey key = m_format->regionLayout().regionForChunk(m_zoneId, coord);
    auto cacheIt = m_cache.find(key);
    if (cacheIt != m_cache.end()) {
        if (cacheIt->second.present.find(coord) == cacheIt->second.present.end()) {
            completeChunkLoad(
                coord,
                requestId,
                Voxel::ChunkLoadOutcome::Missing,
                resolved);
            return;
        }
        queuePayloadBuild(cacheIt->second, coord, requestId);
        touch(key);
        return;
    }

    m_regionPending[key][coord] = requestId;
    if (!queueRegionLoad(key) && m_inFlight.find(key) == m_inFlight.end()) {
        deferRegionLoad(key);
    }
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

    for (int i = 0; i < Voxel::DirectionCount; ++i) {
        Voxel::Direction dir = static_cast<Voxel::Direction>(i);
        int dx = 0;
        int dy = 0;
        int dz = 0;
        Voxel::directionOffset(dir, dx, dy, dz);
        Voxel::ChunkCoord neighborCoord = payload.coord.offset(dx, dy, dz);
        Voxel::Chunk* neighbor = m_world->chunkManager().getChunk(neighborCoord);
        if (neighbor) {
            neighbor->invalidateMesh();
        }
    }

    return true;
}

void AsyncChunkLoader::deferRegionLoad(const RegionKey& key) {
    if (m_deferredRegionLoadSet.insert(key).second) {
        m_deferredRegionLoads.push_back(key);
    }
}

void AsyncChunkLoader::startDeferredRegionLoads() {
    while (!m_deferredRegionLoads.empty()) {
        if (m_maxInFlightRegions > 0 &&
            m_inFlight.size() >= m_maxInFlightRegions) {
            return;
        }

        RegionKey key = m_deferredRegionLoads.front();
        m_deferredRegionLoads.pop_front();
        if (m_deferredRegionLoadSet.erase(key) == 0) {
            continue;
        }

        auto pendingIt = m_regionPending.find(key);
        if (pendingIt == m_regionPending.end() || pendingIt->second.empty()) {
            continue;
        }
        queueRegionLoad(key);
    }
}

bool AsyncChunkLoader::queueRegionLoad(const RegionKey& key) {
    if (m_cache.find(key) != m_cache.end()) {
        return false;
    }
    if (m_inFlight.find(key) != m_inFlight.end()) {
        return false;
    }
    if (m_maxInFlightRegions > 0 && m_inFlight.size() >= m_maxInFlightRegions) {
        return false;
    }

    m_inFlight.insert(key);
    ++m_regionLoadAttempts[key];
    uint64_t revision = m_regionRevisions[key];
    PersistenceService* servicePtr = m_service;
    PersistenceContext contextCopy = m_context;
    auto regionLoadStartCallback = m_regionLoadStartCallback;

    auto job = [this,
                servicePtr,
                contextCopy,
                key,
                revision,
                regionLoadStartCallback = std::move(regionLoadStartCallback)]() mutable {
        if (regionLoadStartCallback) {
            regionLoadStartCallback();
        }
        RegionResult result;
        result.key = key;
        result.revision = revision;
        try {
            auto jobFormat = servicePtr->openFormat(contextCopy);
            result.exists = jobFormat->chunkContainer().regionExists(key);
            if (!result.exists) {
                RegionEntry entry;
                entry.region = std::make_shared<ChunkRegionSnapshot>();
                entry.region->key = key;
                result.entry = std::move(entry);
                result.ok = true;
                m_regionComplete.push(std::move(result));
                return;
            }
            ChunkRegionSnapshot region = jobFormat->chunkContainer().loadRegion(key);
            RegionEntry entry;
            entry.region = std::make_shared<ChunkRegionSnapshot>(std::move(region));
            entry.present.reserve(entry.region->chunks.size());
            entry.spansByCoord.reserve(entry.region->chunks.size());
            for (const auto& snapshot : entry.region->chunks) {
                const ChunkSpan& span = snapshot.data.span;
                Voxel::ChunkCoord coord{span.chunkX, span.chunkY, span.chunkZ};
                entry.present.insert(coord);
                entry.spansByCoord[coord].push_back(&snapshot);
            }
            result.entry = std::move(entry);
            result.ok = true;
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
        m_regionComplete.push(std::move(result));
    };

    if (m_ioPool.threadCount() > 0) {
        m_ioPool.enqueue(std::move(job));
    } else {
        job();
    }
    return true;
}

void AsyncChunkLoader::queuePayloadBuild(
    const RegionEntry& entry,
    Voxel::ChunkCoord coord,
    ChunkLoadRequestId requestId) {
    if (!m_generator || !m_world) {
        return;
    }
    auto pendingIt = m_pendingChunks.find(coord);
    if (pendingIt == m_pendingChunks.end() ||
        pendingIt->second != requestId) {
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

    m_payloadInFlight[coord] = requestId;
    auto generator = m_generator;
    auto registry = &m_world->blockRegistry();
    std::vector<const ChunkSnapshot*> spans = spanIt->second;
    std::shared_ptr<ChunkRegionSnapshot> region = entry.region;
    RegionKey regionKey = region->key;
    uint64_t regionRevision = m_regionRevisions[regionKey];
    auto payloadBuildStartCallback = m_payloadBuildStartCallback;

    auto job = [this,
                coord,
                requestId,
                spans = std::move(spans),
                generator,
                registry,
                region,
                regionKey,
                regionRevision,
                payloadBuildStartCallback = std::move(payloadBuildStartCallback)]() mutable {
        ChunkPayload payload;
        payload.coord = coord;
        payload.requestId = requestId;
        payload.regionKey = regionKey;
        payload.regionRevision = regionRevision;
        payload.worldGenVersion = generator ? generator->config().world.version : 0;
        payload.loadedFromDisk = true;
        try {
            if (payloadBuildStartCallback) {
                payloadBuildStartCallback();
            }
            if (!region) {
                payload.cancelled = true;
                m_chunkComplete.push(std::move(payload));
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
        m_chunkComplete.push(std::move(payload));
    };

    if (m_workerPool.threadCount() > 0) {
        m_workerPool.enqueue(std::move(job));
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
        if (queueRegionLoad(neighbor)) {
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
        m_cache.erase(key);
    }
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
