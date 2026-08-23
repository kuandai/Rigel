#include "Rigel/Voxel/ChunkStreamer.h"
#include "Rigel/Voxel/MeshBuilder.h"
#include "Rigel/Core/Profiler.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <limits>
#include <sstream>
#include <unordered_set>

#include <spdlog/spdlog.h>

namespace Rigel::Voxel {

namespace {
constexpr uint64_t kEvictionRetryDelayUpdates = 60;
using FailureMap = std::map<ChunkCoord, std::string>;

void advanceFailureVersion(uint64_t& version) {
    ++version;
    if (version == 0) {
        ++version;
    }
}

void setFailure(FailureMap& failures,
                uint64_t& version,
                ChunkCoord coord,
                std::string diagnostic) {
    auto failureIt = failures.find(coord);
    if (failureIt == failures.end()) {
        failures.emplace(coord, std::move(diagnostic));
        advanceFailureVersion(version);
    } else if (failureIt->second != diagnostic) {
        failureIt->second = std::move(diagnostic);
        advanceFailureVersion(version);
    }
}

void eraseFailure(FailureMap& failures,
                  uint64_t& version,
                  ChunkCoord coord) {
    if (failures.erase(coord) > 0) {
        advanceFailureVersion(version);
    }
}

void clearFailures(FailureMap& failures, uint64_t& version) {
    if (!failures.empty()) {
        failures.clear();
        advanceFailureVersion(version);
    }
}

int distanceSquared(const ChunkCoord& a, const ChunkCoord& b) {
    int dx = a.x - b.x;
    int dy = a.y - b.y;
    int dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

std::string failureDiagnostic(std::string_view operation,
                              ChunkCoord coord,
                              const std::string& error) {
    std::ostringstream message;
    message << "Chunk " << operation << " failed at (" << coord.x << ", "
            << coord.y << ", " << coord.z << ')';
    if (!error.empty()) {
        message << ": " << error;
    }
    return message.str();
}

std::string diagnosticForLowestCoordinate(
    const FailureMap& errors) {
    return errors.empty() ? std::string{} : errors.begin()->second;
}

size_t visibilityKindIndex(ChunkVisibilityLifecycleKind kind) {
    return kind == ChunkVisibilityLifecycleKind::CameraDemand ? 0 : 1;
}
} // namespace

ChunkStreamer::ChunkStreamer(ChunkManager& manager,
                             WorldMeshStore& meshStore,
                             BlockRegistry& registry,
                             TextureAtlas* atlas,
                             std::shared_ptr<const WorldGenerator> generator)
    : m_chunkManager(&manager),
      m_meshStore(&meshStore),
      m_registry(&registry),
      m_atlas(atlas),
      m_generator(std::move(generator)) {}

ChunkStreamer::~ChunkStreamer() {
    if (m_genPool) {
        m_genPool->stop();
        m_genPool.reset();
    }
    if (m_meshPool) {
        m_meshPool->stop();
        m_meshPool.reset();
    }
    abandonVisibilityTraces(ChunkVisibilityOutcome::StreamerDestroyed);
}

void ChunkStreamer::setConfig(const StreamingConfig& config) {
    bool activeStream = m_initialStreamingBegun && m_lastCenter.has_value();
    m_config = config;
    m_cache.setMaxChunks(m_config.maxResidentChunks);
    if (activeStream) {
        int viewDistance = std::max(0, m_config.viewDistanceChunks);
        int unloadDistance = std::max(
            viewDistance, m_config.unloadDistanceChunks);
        m_workMetrics.deferredEvictionCoordinatesInspected +=
            retireIneligibleEvictions(
                *m_lastCenter,
                viewDistance * viewDistance,
                unloadDistance * unloadDistance);
        m_lastViewDistance = -1;
        m_lastUnloadDistance = -1;
        ensureThreadPool();
        refreshDiagnostics(false);
        return;
    }

    for (const auto& pending : m_pendingVisibilityTraces) {
        if (pending) {
            completePendingVisibilityTrace(
                pending->key.coord,
                ChunkVisibilityOutcome::Reset);
            break;
        }
    }

    m_desired.clear();
    m_desiredSet.clear();
    m_desiredPriority.clear();
    m_dirtyMeshQueue = {};
    m_dirtyMeshQueued.clear();
    m_priorityMeshRequests.clear();
    m_evictionRetryAfter.clear();
    m_versionReplacementRetries.clear();
    m_versionReplacementWaiting.clear();
    clearFailures(m_generationErrors, m_generationFailureVersion);
    clearFailures(m_loadErrors, m_loadFailureVersion);
    clearFailures(m_meshErrors, m_meshFailureVersion);
    clearFailures(m_evictionErrors, m_evictionFailureVersion);
    m_nextEvictionRetrySequence = 0;
    m_loadGenQueue.clear();
    m_loadGenQueued.clear();
    m_generationCapacityWait.clear();
    m_generationCapacityWaiting.clear();
    m_missingMeshCapacityWait.clear();
    m_missingMeshCapacityWaiting.clear();
    m_meshDependencyWaiting.clear();
    m_lastCenter.reset();
    m_lastViewDistance = -1;
    m_lastUnloadDistance = -1;
    m_initialStreamingBegun = false;
    m_workObservedThisUpdate = false;
    m_workStartedThisUpdate = false;
    m_nextSingleSlotMeshKind = MeshRequestKind::Missing;
    ensureThreadPool();
    refreshDiagnostics(false);
}

void ChunkStreamer::setGenerator(std::shared_ptr<const WorldGenerator> generator) {
    if (m_generator == generator) {
        return;
    }

    for (const auto& pending : m_pendingVisibilityTraces) {
        if (pending) {
            completePendingVisibilityTrace(
                pending->key.coord,
                ChunkVisibilityOutcome::GeneratorReplaced);
            break;
        }
    }

    for (auto& [coord, cancelToken] : m_genCancel) {
        cancelToken->store(true, std::memory_order_relaxed);
        auto stateIt = m_states.find(coord);
        if (stateIt != m_states.end() &&
            stateIt->second == ChunkState::QueuedGen) {
            m_states.erase(stateIt);
        }
    }
    m_genCancel.clear();

    uint64_t nextEpoch = m_workEpoch.fetch_add(1, std::memory_order_relaxed) + 1;
    if (nextEpoch == 0) {
        m_workEpoch.store(1, std::memory_order_relaxed);
    }
    for (auto& [coord, flight] : m_meshInFlight) {
        flight.obsolete = true;
        completeInFlightVisibilityTrace(
            flight,
            ChunkVisibilityOutcome::GeneratorReplaced);
        Chunk* chunk = dirtyMeshPriority(coord)
            ? m_chunkManager->getChunk(coord)
            : nullptr;
        auto stateIt = m_states.find(coord);
        if (stateIt != m_states.end() &&
            stateIt->second == ChunkState::QueuedMesh) {
            if (chunk && !chunk->isEmpty()) {
                stateIt->second = ChunkState::ReadyData;
            } else {
                m_states.erase(stateIt);
            }
        }
        if (chunk && !chunk->isEmpty()) {
            chunk->markDirty();
        }
    }

    m_generator = std::move(generator);
    for (const ChunkCoord& coord : m_desired) {
        queueLoadGen(coord);
    }
    refreshDiagnostics(false);
}

void ChunkStreamer::setBenchmark(ChunkBenchmarkStats* stats) {
    m_benchmark = stats;
}

void ChunkStreamer::setVisibilityTracer(
    std::shared_ptr<ChunkVisibilityTracer> tracer) {
    tracer = tracer && tracer->enabled() ? std::move(tracer) : nullptr;
    if (tracer == m_visibilityTracer) {
        return;
    }
    const auto previousTracer = m_visibilityTracer;
    for (const auto& pending : m_pendingVisibilityTraces) {
        if (pending) {
            completePendingVisibilityTrace(
                pending->key.coord,
                ChunkVisibilityOutcome::TracerReplaced);
            break;
        }
    }
    for (auto& [coord, flight] : m_meshInFlight) {
        if (flight.visibilityTracer == previousTracer) {
            completeInFlightVisibilityTrace(
                flight,
                ChunkVisibilityOutcome::TracerReplaced);
        }
    }
    if (m_meshStore && previousTracer) {
        m_meshStore->endVisibilityTrace(
            previousTracer->coord(),
            previousTracer,
            ChunkVisibilityDrawOutcome::TraceReplacedBeforeDraw);
    }
    m_visibilityTracer = std::move(tracer);
}

void ChunkStreamer::setChunkLoader(ChunkLoadCallback loader) {
    m_chunkLoader = std::move(loader);
    for (const ChunkCoord& coord : m_desired) {
        queueLoadGen(coord);
    }
}

void ChunkStreamer::setChunkPendingCallback(ChunkPendingCallback pending) {
    m_chunkPending = std::move(pending);
}

void ChunkStreamer::setChunkLoadDrain(ChunkLoadDrainCallback drain) {
    m_chunkLoadDrain = std::move(drain);
}

void ChunkStreamer::setChunkLoadCancel(ChunkLoadCancelCallback cancel) {
    m_chunkLoadCancel = std::move(cancel);
}

void ChunkStreamer::setChunkLoadWorkCallback(ChunkLoadWorkCallback work) {
    m_chunkLoadWork = std::move(work);
    refreshDiagnostics(false);
}

void ChunkStreamer::setChunkEvictionCallback(ChunkEvictionCallback evict) {
    m_chunkEviction = std::move(evict);
}

void ChunkStreamer::markSpawnDiscoveryComplete() {
    m_spawnDiscoveryComplete = true;
    refreshDiagnostics(false);
}

void ChunkStreamer::prioritizeMesh(ChunkCoord coord) {
    queueDirtyMesh(coord, true);
    refreshDiagnostics(false);
}

void ChunkStreamer::update(const glm::vec3& cameraPos) {
    m_workMetrics.lastUpdateDesiredBuildCoordinatesInspected = 0;
    m_workMetrics.lastUpdateSchedulerCoordinatesInspected = 0;
    m_workMetrics.lastUpdateCacheEvictionCoordinatesInspected = 0;
    m_workMetrics.lastUpdateResidentEvictionCoordinatesInspected = 0;
    m_workMetrics.lastUpdateDeferredEvictionCoordinatesInspected = 0;
    if (!m_chunkManager || !m_generator || !m_meshStore) {
        return;
    }

    m_initialStreamingBegun = true;
    ++m_streamingUpdateSequence;
    StreamingDiagnosticSnapshot beforeUpdate = collectDiagnostics();
    m_workObservedThisUpdate = !beforeUpdate.workEmpty();
    m_workStartedThisUpdate = false;

    uint64_t desiredBuildCoordinatesInspected = 0;
    uint64_t schedulerCoordinatesInspected = 0;
    uint64_t cacheEvictionCoordinatesInspected = 0;
    uint64_t residentEvictionCoordinatesInspected = 0;
    uint64_t deferredEvictionCoordinatesInspected = 0;

    ChunkCoord center = cameraToChunk(cameraPos);
    int viewDistance = std::max(0, m_config.viewDistanceChunks);
    int unloadDistance = std::max(viewDistance, m_config.unloadDistanceChunks);
    int viewRadiusSq = viewDistance * viewDistance;
    int unloadRadiusSq = unloadDistance * unloadDistance;

    bool rebuildDesired = !m_lastCenter ||
        *m_lastCenter != center ||
        m_lastViewDistance != viewDistance ||
        m_lastUnloadDistance != unloadDistance;
    bool cacheEvictionNeeded = rebuildDesired;

    if (rebuildDesired) {
        PROFILE_SCOPE("Streaming/Update/DesiredBuild");
        auto previousDesired = std::move(m_desiredSet);
        auto previouslyQueued = std::move(m_loadGenQueued);
        m_loadGenQueue.clear();
        m_loadGenQueued.clear();

        std::vector<std::pair<int, ChunkCoord>> desired;
        desired.reserve(static_cast<size_t>(viewDistance * 2 + 1) *
                        static_cast<size_t>(viewDistance * 2 + 1) *
                        static_cast<size_t>(viewDistance * 2 + 1));

        for (int dz = -viewDistance; dz <= viewDistance; ++dz) {
            for (int dy = -viewDistance; dy <= viewDistance; ++dy) {
                for (int dx = -viewDistance; dx <= viewDistance; ++dx) {
                    ++desiredBuildCoordinatesInspected;
                    ChunkCoord coord{center.x + dx, center.y + dy, center.z + dz};
                    int distSq = distanceSquared(center, coord);
                    if (distSq > viewRadiusSq) {
                        continue;
                    }
                    desired.emplace_back(distSq, coord);
                }
            }
        }

        std::sort(desired.begin(), desired.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });

        m_desired.clear();
        m_desiredSet.clear();
        m_desiredPriority.clear();
        m_desired.reserve(desired.size());
        m_desiredSet.reserve(desired.size());
        m_desiredPriority.reserve(desired.size());
        for (size_t priority = 0; priority < desired.size(); ++priority) {
            const auto& entry = desired[priority];
            m_desired.push_back(entry.second);
            m_desiredSet.insert(entry.second);
            m_desiredPriority.emplace(entry.second, priority);
        }
        deferredEvictionCoordinatesInspected = retireIneligibleEvictions(
            center, viewRadiusSq, unloadRadiusSq);
        reprioritizeDirtyMeshes();
        for (const ChunkCoord& coord : m_desired) {
            const bool newlyDesired =
                previousDesired.find(coord) == previousDesired.end();
            if (newlyDesired) {
                beginCameraVisibilityTrace(coord);
            }
            if (previouslyQueued.find(coord) != previouslyQueued.end() ||
                newlyDesired) {
                queueLoadGen(coord);
            }
        }
        for (const ChunkCoord& coord : previousDesired) {
            if (m_desiredSet.find(coord) == m_desiredSet.end()) {
                bool retainedMesh = dirtyMeshPriority(coord).has_value();
                m_generationCapacityWaiting.erase(coord);
                m_missingMeshCapacityWaiting.erase(coord);
                if (!retainedMesh) {
                    m_meshDependencyWaiting.erase(coord);
                }
                eraseFailure(
                    m_generationErrors, m_generationFailureVersion, coord);
                eraseFailure(m_loadErrors, m_loadFailureVersion, coord);
                if (!retainedMesh) {
                    eraseFailure(m_meshErrors, m_meshFailureVersion, coord);
                }
                completePendingVisibilityTrace(
                    coord,
                    ChunkVisibilityLifecycleKind::CameraDemand,
                    ChunkVisibilityOutcome::CameraLeft);
                auto flightIt = m_meshInFlight.find(coord);
                if (flightIt != m_meshInFlight.end() &&
                    flightIt->second.visibilityKind ==
                        ChunkVisibilityLifecycleKind::CameraDemand) {
                    completeInFlightVisibilityTrace(
                        flightIt->second,
                        ChunkVisibilityOutcome::CameraLeft);
                }
                if (m_meshStore && m_visibilityTracer &&
                    m_visibilityTracer->traces(coord)) {
                    m_meshStore->endCameraVisibilityTrace(coord);
                }
                queueLoadedNeighbors(coord);
            }
        }

        m_lastCenter = center;
        m_lastViewDistance = viewDistance;
        m_lastUnloadDistance = unloadDistance;

        for (auto it = m_states.begin(); it != m_states.end(); ) {
            bool retainedMeshRequest =
                it->second == ChunkState::QueuedMesh &&
                dirtyMeshPriority(it->first).has_value();
            if ((it->second == ChunkState::QueuedGen ||
                 it->second == ChunkState::QueuedMesh ||
                 it->second == ChunkState::GenerationFailed) &&
                m_desiredSet.find(it->first) == m_desiredSet.end() &&
                !retainedMeshRequest) {
                if (it->second == ChunkState::QueuedGen) {
                    auto cancelIt = m_genCancel.find(it->first);
                    if (cancelIt != m_genCancel.end()) {
                        cancelIt->second->store(true, std::memory_order_relaxed);
                        m_genCancel.erase(cancelIt);
                    }
                }
                it = m_states.erase(it);
                continue;
            }
            ++it;
        }

        if (!m_loadPending.empty()) {
            for (auto it = m_loadPending.begin(); it != m_loadPending.end(); ) {
                if (m_desiredSet.find(it->first) == m_desiredSet.end()) {
                    ChunkCoord coord = it->first;
                    ++it;
                    cancelPendingLoad(coord);
                } else {
                    ++it;
                }
            }
        }
    }

    retryDeferredEvictions(center, unloadRadiusSq);

    if (!m_chunkLoadDrain && !m_loadPending.empty()) {
        for (auto it = m_loadPending.begin(); it != m_loadPending.end(); ) {
            ++schedulerCoordinatesInspected;
            ChunkCoord coord = it->first;
            bool resident = m_chunkManager->getChunk(coord) != nullptr;
            bool resolved = resident;
            if (!resolved && m_chunkPending) {
                resolved = !m_chunkPending(coord);
            } else if (!resolved) {
                queueLoadGen(coord);
                ++it;
                continue;
            }
            if (resolved) {
                ++it;
                if (resident) {
                    observeVisibilityDataReady(
                        coord, ChunkVisibilityOrigin::Persisted);
                    cancelPendingLoad(coord);
                    observeVisibilityNeighborReadiness(coord);
                } else {
                    m_loadPending.erase(coord);
                }
                queueLoadGen(coord);
            } else {
                ++it;
            }
        }
    }

    auto consumeDirtyMeshNotifications = [&]() {
        for (const ChunkCoord& coord :
             m_chunkManager->consumeDirtyMeshNotifications()) {
            if (m_chunkManager->getChunk(coord)) {
                cancelPendingLoad(coord);
            }
            queueDirtyMesh(coord);
        }
    };
    consumeDirtyMeshNotifications();

    size_t genLimit = (m_config.genQueueLimit <= 0)
        ? std::numeric_limits<size_t>::max()
        : static_cast<size_t>(m_config.genQueueLimit);
    size_t meshLimit = (m_config.meshQueueLimit <= 0)
        ? std::numeric_limits<size_t>::max()
        : static_cast<size_t>(m_config.meshQueueLimit);
    size_t meshLimitMissing = meshLimit;
    size_t meshLimitDirty = meshLimit;
    if (meshLimit != std::numeric_limits<size_t>::max()) {
        if (meshLimit == 1) {
            meshLimitMissing = 1;
            meshLimitDirty = 1;
        } else {
            size_t reserve = meshLimit / 4;
            if (reserve == 0) {
                reserve = 1;
            }
            if (reserve >= meshLimit) {
                reserve = meshLimit - 1;
            }
            meshLimitMissing = meshLimit - reserve;
            meshLimitDirty = reserve;
        }
    }

    bool genFull = m_inFlightGen >= genLimit;
    bool meshFull = m_inFlightMesh >= meshLimit;
    bool priorityMeshPending =
        !m_dirtyMeshQueue.empty() &&
        (m_dirtyMeshQueue.top().prioritized ||
         m_priorityMeshRequests.find(m_dirtyMeshQueue.top().coord) !=
             m_priorityMeshRequests.end());
    bool dirtyHasSingleSlotTurn =
        meshLimit == 1 &&
        m_nextSingleSlotMeshKind == MeshRequestKind::Dirty &&
        !m_dirtyMeshQueue.empty();
    bool meshFullMissing = m_inFlightMeshMissing >= meshLimitMissing;
    bool meshFullDirty = m_inFlightMeshDirty >= meshLimitDirty;
    std::vector<PendingDirtyMesh> obsoleteFlightBlocked;

    auto scheduleDirtyMeshes = [&]() {
        if (m_dirtyMeshQueue.empty()) {
            return;
        }

        PROFILE_SCOPE("Streaming/Update/MeshDirty");
        while (!m_dirtyMeshQueue.empty()) {
            const ChunkCoord pendingCoord = m_dirtyMeshQueue.top().coord;
            auto pendingFlightIt = m_meshInFlight.find(pendingCoord);
            if (m_dirtyMeshQueued.find(pendingCoord) != m_dirtyMeshQueued.end() &&
                pendingFlightIt != m_meshInFlight.end() &&
                pendingFlightIt->second.obsolete) {
                PendingDirtyMesh blocked = m_dirtyMeshQueue.top();
                m_dirtyMeshQueue.pop();
                auto existing = std::find_if(
                    obsoleteFlightBlocked.begin(),
                    obsoleteFlightBlocked.end(),
                    [&](const PendingDirtyMesh& request) {
                        return request.coord == blocked.coord;
                    });
                if (existing == obsoleteFlightBlocked.end()) {
                    ++schedulerCoordinatesInspected;
                    obsoleteFlightBlocked.push_back(blocked);
                } else {
                    existing->priority = std::min(
                        existing->priority, blocked.priority);
                    existing->prioritized =
                        existing->prioritized || blocked.prioritized;
                }
                continue;
            }
            if (meshFull ||
                (meshFullDirty &&
                 !m_dirtyMeshQueue.top().prioritized &&
                 m_priorityMeshRequests.find(m_dirtyMeshQueue.top().coord) ==
                     m_priorityMeshRequests.end())) {
                break;
            }

            PendingDirtyMesh request = m_dirtyMeshQueue.top();
            m_dirtyMeshQueue.pop();
            ChunkCoord coord = request.coord;
            if (m_dirtyMeshQueued.erase(coord) == 0) {
                continue;
            }
            bool prioritized = request.prioritized ||
                m_priorityMeshRequests.find(coord) !=
                    m_priorityMeshRequests.end();
            ++schedulerCoordinatesInspected;

            if (!dirtyMeshPriority(coord)) {
                m_priorityMeshRequests.erase(coord);
                continue;
            }

            ChunkState state = ChunkState::Missing;
            auto stateIt = m_states.find(coord);
            if (stateIt != m_states.end()) {
                state = stateIt->second;
            }

            Chunk* chunk = m_chunkManager->getChunk(coord);
            if (!chunk) {
                continue;
            }

            if (state == ChunkState::QueuedMesh) {
                auto flightIt = m_meshInFlight.find(coord);
                if (chunk->isDirty() && flightIt != m_meshInFlight.end() &&
                    flightIt->second.observedRevision != chunk->meshRevision()) {
                    flightIt->second.observedRevision = chunk->meshRevision();
                    ++m_workMetrics.meshInvalidations;
                    ++m_workMetrics.meshRequestsCoalesced;
                }
                if (flightIt != m_meshInFlight.end()) {
                    flightIt->second.prioritized =
                        flightIt->second.prioritized || prioritized;
                    m_priorityMeshRequests.erase(coord);
                }
                continue;
            }

            bool hasMesh = m_meshStore && m_meshStore->contains(coord);
            bool isMeshed = hasMesh || state == ChunkState::ReadyMesh;
            if (isMeshed && !chunk->isDirty()) {
                m_priorityMeshRequests.erase(coord);
                continue;
            }
            if (!isMeshed && !prioritized) {
                continue;
            }

            if (!hasAllNeighborsLoaded(coord)) {
                waitForMeshDependencies(coord);
                continue;
            }

            MeshRequestKind kind = isMeshed
                ? MeshRequestKind::Dirty
                : MeshRequestKind::Missing;
            bool kindFull = kind == MeshRequestKind::Dirty
                ? meshFullDirty
                : meshFullMissing;
            if (kindFull) {
                m_dirtyMeshQueued.insert(coord);
                m_dirtyMeshQueue.push({request.priority, coord, prioritized});
                break;
            }

            enqueueMesh(coord, *chunk, kind, prioritized);
            meshFullDirty = m_inFlightMeshDirty >= meshLimitDirty;
            meshFullMissing = m_inFlightMeshMissing >= meshLimitMissing;
            meshFull = m_inFlightMesh >= meshLimit;
        }
    };

    if (priorityMeshPending || dirtyHasSingleSlotTurn) {
        scheduleDirtyMeshes();
    }

    if (!m_loadGenQueue.empty()) {
        PROFILE_SCOPE("Streaming/Update/LoadGen");
        size_t budget = (m_config.updateBudgetPerFrame <= 0)
            ? std::numeric_limits<size_t>::max()
            : static_cast<size_t>(m_config.updateBudgetPerFrame);
        size_t queued = 0;
        size_t scanned = 0;
        size_t candidates = m_loadGenQueue.size();
        while (queued < budget && scanned < candidates && !m_loadGenQueue.empty()) {
            if (genFull && meshFullMissing) {
                break;
            }

            ChunkCoord coord = m_loadGenQueue.front();
            m_loadGenQueue.pop_front();
            m_loadGenQueued.erase(coord);
            ++scanned;
            ++schedulerCoordinatesInspected;

            if (m_desiredSet.find(coord) == m_desiredSet.end()) {
                continue;
            }

            ChunkState state = ChunkState::Missing;
            auto stateIt = m_states.find(coord);
            if (stateIt != m_states.end()) {
                state = stateIt->second;
            }

            Chunk* chunk = m_chunkManager->getChunk(coord);
            ChunkLoadRequestResult loadResult = ChunkLoadRequestResult::Missing;
            ChunkLoadRequestId loadRequestId = 0;
            if (!chunk && state != ChunkState::QueuedGen && m_chunkLoader) {
                markVisibilityStage(
                    coord, ChunkVisibilityStage::DataRequest);
                auto pendingIt = m_loadPending.find(coord);
                bool wasPending = pendingIt != m_loadPending.end();
                loadRequestId = wasPending
                    ? pendingIt->second
                    : nextLoadRequestId();
                loadResult = m_chunkLoader({coord, loadRequestId});
                if (loadResult != ChunkLoadRequestResult::Missing && !wasPending) {
                    ++m_workMetrics.chunkLoadRequestsStarted;
                }
                chunk = m_chunkManager->getChunk(coord);
                if (chunk) {
                    cancelPendingLoad(coord);
                    observeVisibilityDataReady(
                        coord, ChunkVisibilityOrigin::Persisted);
                    observeVisibilityNeighborReadiness(coord);
                }
            }

            if (chunk) {
                cancelPendingLoad(coord);
                if (m_generator &&
                    chunk->worldGenVersion() != m_generator->config().world.version) {
                    if (!evictChunk(coord, true)) {
                        continue;
                    }
                    if (!genFull) {
                        enqueueGeneration(coord);
                        genFull = m_inFlightGen >= genLimit;
                        ++queued;
                    } else {
                        waitForGenerationCapacity(coord);
                    }
                    continue;
                }
                m_versionReplacementWaiting.erase(coord);
                eraseFailure(
                    m_evictionErrors, m_evictionFailureVersion, coord);

                m_cache.touch(coord);
                cacheEvictionNeeded = true;
                bool hasMesh = m_meshStore && m_meshStore->contains(coord);
                bool isMeshed = hasMesh || state == ChunkState::ReadyMesh;
                if (stateIt == m_states.end() || state == ChunkState::QueuedGen) {
                    state = isMeshed ? ChunkState::ReadyMesh : ChunkState::ReadyData;
                    m_states[coord] = state;
                    queueLoadedNeighbors(coord);
                }

                if (chunk->isEmpty()) {
                    if (m_meshStore) {
                        m_meshStore->remove(coord);
                    }
                    chunk->clearDirty();
                    m_priorityMeshRequests.erase(coord);
                    m_states[coord] = ChunkState::ReadyMesh;
                    m_countedMeshRetryRevisions.erase(coord);
                    completePendingVisibilityTrace(
                        coord,
                        ChunkVisibilityOutcome::VoxelEmpty);
                    continue;
                }

                if (isMeshed && chunk->isDirty()) {
                    queueDirtyMesh(coord);
                }

                if (!isMeshed && state != ChunkState::QueuedMesh) {
                    bool coordinateMeshInFlight =
                        m_meshInFlight.find(coord) != m_meshInFlight.end();
                    bool neighborsLoaded = hasAllNeighborsLoaded(coord);
                    if (coordinateMeshInFlight) {
                        waitForMissingMeshCapacity(coord);
                    } else if (!meshFull && !meshFullMissing &&
                               neighborsLoaded) {
                        enqueueMesh(coord, *chunk, MeshRequestKind::Missing);
                        meshFullMissing = m_inFlightMeshMissing >= meshLimitMissing;
                        meshFull = m_inFlightMesh >= meshLimit;
                        ++queued;
                    } else if (meshFull || meshFullMissing) {
                        waitForMissingMeshCapacity(coord);
                    } else {
                        waitForMeshDependencies(coord);
                    }
                }
                continue;
            }

            if (state == ChunkState::QueuedGen) {
                continue;
            }

            if (loadResult != ChunkLoadRequestResult::Missing) {
                m_loadPending.insert_or_assign(coord, loadRequestId);
                ++queued;
                continue;
            }
            eraseFailure(m_loadErrors, m_loadFailureVersion, coord);
            if (m_chunkPending && m_chunkPending(coord)) {
                markVisibilityStage(
                    coord, ChunkVisibilityStage::DataRequest);
                if (loadRequestId == 0) {
                    loadRequestId = nextLoadRequestId();
                }
                m_loadPending.insert_or_assign(coord, loadRequestId);
                ++queued;
                continue;
            }

            if (!genFull) {
                enqueueGeneration(coord);
                genFull = m_inFlightGen >= genLimit;
                ++queued;
            } else {
                waitForGenerationCapacity(coord);
            }
        }
    }

    scheduleDirtyMeshes();
    for (const PendingDirtyMesh& blocked : obsoleteFlightBlocked) {
        m_dirtyMeshQueue.push(blocked);
    }

    if (rebuildDesired) {
        PROFILE_SCOPE("Streaming/Update/Evict");
        std::vector<ChunkCoord> toEvict;
        m_chunkManager->forEachChunk([&](ChunkCoord coord, const Chunk&) {
            ++residentEvictionCoordinatesInspected;
            int distSq = distanceSquared(center, coord);
            if (distSq > unloadRadiusSq) {
                toEvict.push_back(coord);
            }
        });

        for (const ChunkCoord& coord : toEvict) {
            if (evictChunk(coord)) {
                m_cache.erase(coord);
            }
        }

    }

    if (cacheEvictionNeeded) {
        PROFILE_SCOPE("Streaming/Update/CacheEvict");
        m_cache.evict(
            m_desiredSet,
            [this](ChunkCoord coord) { return evictChunk(coord); });
        cacheEvictionCoordinatesInspected = m_cache.lastEvictionInspections();
    }
    consumeDirtyMeshNotifications();

    m_workMetrics.lastUpdateDesiredBuildCoordinatesInspected =
        desiredBuildCoordinatesInspected;
    m_workMetrics.lastUpdateSchedulerCoordinatesInspected =
        schedulerCoordinatesInspected;
    m_workMetrics.lastUpdateCacheEvictionCoordinatesInspected =
        cacheEvictionCoordinatesInspected;
    m_workMetrics.lastUpdateResidentEvictionCoordinatesInspected =
        residentEvictionCoordinatesInspected;
    m_workMetrics.lastUpdateDeferredEvictionCoordinatesInspected =
        deferredEvictionCoordinatesInspected;
    m_workMetrics.desiredBuildCoordinatesInspected +=
        desiredBuildCoordinatesInspected;
    m_workMetrics.schedulerCoordinatesInspected += schedulerCoordinatesInspected;
    m_workMetrics.cacheEvictionCoordinatesInspected +=
        cacheEvictionCoordinatesInspected;
    m_workMetrics.residentEvictionCoordinatesInspected +=
        residentEvictionCoordinatesInspected;
    m_workMetrics.deferredEvictionCoordinatesInspected +=
        deferredEvictionCoordinatesInspected;
    StreamingDiagnosticSnapshot afterUpdate = collectDiagnostics();
    m_workObservedThisUpdate = m_workObservedThisUpdate || !afterUpdate.workEmpty();
    m_workStartedThisUpdate =
        beforeUpdate.generation.started != afterUpdate.generation.started ||
        beforeUpdate.chunkLoad.started != afterUpdate.chunkLoad.started ||
        beforeUpdate.mesh.started != afterUpdate.mesh.started;
    refreshDiagnostics(false);
}

ChunkCoord ChunkStreamer::cameraToChunk(const glm::vec3& cameraPos) const {
    return worldToChunk(
        static_cast<int>(std::floor(cameraPos.x)),
        static_cast<int>(std::floor(cameraPos.y)),
        static_cast<int>(std::floor(cameraPos.z))
    );
}

void ChunkStreamer::processCompletions() {
    if (!m_chunkManager) {
        return;
    }

    StreamingDiagnosticSnapshot beforeCompletions = collectDiagnostics();
    m_workObservedThisUpdate =
        m_workObservedThisUpdate || !beforeCompletions.workEmpty();

    size_t loadBudget = (m_config.loadApplyBudgetPerFrame <= 0)
        ? std::numeric_limits<size_t>::max()
        : static_cast<size_t>(m_config.loadApplyBudgetPerFrame);
    if (m_chunkLoadDrain) {
        PROFILE_SCOPE("Streaming/LoadDrain");
        for (const ChunkLoadCompletion& completion : m_chunkLoadDrain(loadBudget)) {
            auto pendingIt = m_loadPending.find(completion.coord);
            if (pendingIt == m_loadPending.end() ||
                pendingIt->second != completion.requestId ||
                m_desiredSet.find(completion.coord) == m_desiredSet.end()) {
                continue;
            }
            m_loadPending.erase(pendingIt);
            if (completion.outcome == ChunkLoadOutcome::Failed) {
                setFailure(
                    m_loadErrors,
                    m_loadFailureVersion,
                    completion.coord,
                    failureDiagnostic(
                        "load", completion.coord, completion.error));
                completePendingVisibilityTrace(
                    completion.coord,
                    ChunkVisibilityOutcome::Failed);
                continue;
            }
            eraseFailure(
                m_loadErrors, m_loadFailureVersion, completion.coord);
            if (completion.outcome == ChunkLoadOutcome::Loaded &&
                m_visibilityTracer &&
                m_chunkManager->getChunk(completion.coord)) {
                observeVisibilityDataReady(
                    completion.coord, ChunkVisibilityOrigin::Persisted);
                observeVisibilityNeighborReadiness(completion.coord);
            }
            queueLoadGen(completion.coord);
        }
    }
    size_t budget = (m_config.applyBudgetPerFrame <= 0)
        ? std::numeric_limits<size_t>::max()
        : static_cast<size_t>(m_config.applyBudgetPerFrame);
    {
        PROFILE_SCOPE("Streaming/GenApply");
        applyGenCompletions(budget);
    }
    {
        PROFILE_SCOPE("Streaming/MeshApply");
        applyMeshCompletions(budget);
    }

    StreamingDiagnosticSnapshot afterCompletions = collectDiagnostics();
    m_workObservedThisUpdate =
        m_workObservedThisUpdate || !afterCompletions.workEmpty();
    m_workStartedThisUpdate = m_workStartedThisUpdate ||
        beforeCompletions.generation.started != afterCompletions.generation.started ||
        beforeCompletions.chunkLoad.started != afterCompletions.chunkLoad.started ||
        beforeCompletions.mesh.started != afterCompletions.mesh.started;

    bool advanceWindow = m_lifecycleUpdateSequence != m_streamingUpdateSequence;
    refreshDiagnostics(advanceWindow);
    if (advanceWindow) {
        m_lifecycleUpdateSequence = m_streamingUpdateSequence;
    }
}

void ChunkStreamer::getDebugStates(std::vector<DebugChunkState>& out) const {
    out.clear();
    out.reserve(m_states.size());

    for (const auto& [coord, state] : m_states) {
        DebugState debugState;
        switch (state) {
            case ChunkState::QueuedGen:
                debugState = DebugState::QueuedGen;
                break;
            case ChunkState::ReadyData:
                debugState = DebugState::ReadyData;
                break;
            case ChunkState::QueuedMesh:
                debugState = DebugState::QueuedMesh;
                break;
            case ChunkState::ReadyMesh:
                debugState = DebugState::ReadyMesh;
                break;
            case ChunkState::GenerationFailed:
                debugState = DebugState::GenerationFailed;
                break;
            case ChunkState::MeshFailed:
                debugState = DebugState::MeshFailed;
                break;
            default:
                continue;
        }
        if (debugState == DebugState::ReadyData && m_chunkManager) {
            if (Chunk* chunk = m_chunkManager->getChunk(coord)) {
                if (chunk->loadedFromDisk()) {
                    debugState = DebugState::LoadedFromDisk;
                }
            }
        }
        out.push_back({coord, debugState});
    }
}

void ChunkStreamer::reset() {
    for (const auto& pending : m_pendingVisibilityTraces) {
        if (pending) {
            completePendingVisibilityTrace(
                pending->key.coord,
                ChunkVisibilityOutcome::Reset);
            break;
        }
    }
    for (auto& entry : m_genCancel) {
        entry.second->store(true, std::memory_order_relaxed);
    }

    uint64_t nextEpoch = m_workEpoch.fetch_add(1, std::memory_order_relaxed) + 1;
    if (nextEpoch == 0) {
        m_workEpoch.store(1, std::memory_order_relaxed);
    }
    m_states.clear();
    for (auto& entry : m_meshInFlight) {
        entry.second.obsolete = true;
        completeInFlightVisibilityTrace(
            entry.second,
            ChunkVisibilityOutcome::Reset);
    }
    m_countedMeshRetryRevisions.clear();
    m_cache = ChunkCache();
    m_cache.setMaxChunks(m_config.maxResidentChunks);
    m_desired.clear();
    m_desiredSet.clear();
    m_desiredPriority.clear();
    m_dirtyMeshQueue = {};
    m_dirtyMeshQueued.clear();
    m_priorityMeshRequests.clear();
    m_evictionRetryAfter.clear();
    m_versionReplacementRetries.clear();
    m_versionReplacementWaiting.clear();
    clearFailures(m_generationErrors, m_generationFailureVersion);
    clearFailures(m_loadErrors, m_loadFailureVersion);
    clearFailures(m_meshErrors, m_meshFailureVersion);
    clearFailures(m_evictionErrors, m_evictionFailureVersion);
    m_nextEvictionRetrySequence = 0;
    m_loadGenQueue.clear();
    m_loadGenQueued.clear();
    m_generationCapacityWait.clear();
    m_generationCapacityWaiting.clear();
    m_missingMeshCapacityWait.clear();
    m_missingMeshCapacityWaiting.clear();
    m_meshDependencyWaiting.clear();
    if (m_chunkLoadCancel) {
        for (const auto& pending : m_loadPending) {
            m_chunkLoadCancel(pending.first);
        }
    }
    m_loadPending.clear();
    m_lastCenter.reset();
    m_lastViewDistance = -1;
    m_lastUnloadDistance = -1;
    m_initialStreamingBegun = false;
    m_workObservedThisUpdate = false;
    m_workStartedThisUpdate = false;
    m_nextSingleSlotMeshKind = MeshRequestKind::Missing;
    m_streamingUpdateSequence = 0;
    m_lifecycleUpdateSequence = 0;
    m_genCancel.clear();

    applyGenCompletions(std::numeric_limits<size_t>::max());
    applyMeshCompletions(std::numeric_limits<size_t>::max());
    refreshDiagnostics(false);
}

bool ChunkStreamer::evictChunk(ChunkCoord coord, bool versionReplacement) {
    bool replacementPending = versionReplacement ||
        m_versionReplacementRetries.find(coord) !=
            m_versionReplacementRetries.end() ||
        m_versionReplacementWaiting.find(coord) !=
            m_versionReplacementWaiting.end();
    Chunk* chunk = m_chunkManager ? m_chunkManager->getChunk(coord) : nullptr;
    if (chunk && chunk->isPersistDirty()) {
        auto retryIt = m_evictionRetryAfter.find(coord);
        if (retryIt != m_evictionRetryAfter.end() &&
            retryIt->second > m_streamingUpdateSequence) {
            return false;
        }
        if (!m_chunkEviction || !m_chunkEviction(coord)) {
            deferEviction(coord, replacementPending);
            return false;
        }
        chunk = m_chunkManager->getChunk(coord);
        if (chunk && chunk->isPersistDirty()) {
            deferEviction(coord, replacementPending);
            return false;
        }
    }

    m_evictionRetryAfter.erase(coord);
    m_versionReplacementRetries.erase(coord);
    eraseFailure(m_evictionErrors, m_evictionFailureVersion, coord);
    if (replacementPending) {
        m_versionReplacementWaiting.insert(coord);
    } else {
        m_versionReplacementWaiting.erase(coord);
    }
    if (m_meshStore) {
        m_meshStore->remove(coord);
    }
    if (std::any_of(
            m_pendingVisibilityTraces.begin(),
            m_pendingVisibilityTraces.end(),
            [&](const auto& pending) {
                return pending && pending->key.coord == coord;
            })) {
        completePendingVisibilityTrace(
            coord, ChunkVisibilityOutcome::Stale);
    }
    if (chunk) {
        chunk->setLoadedFromDisk(false);
        m_chunkManager->unloadChunk(coord);
    }
    m_states.erase(coord);
    m_dirtyMeshQueued.erase(coord);
    m_missingMeshCapacityWaiting.erase(coord);
    m_meshDependencyWaiting.erase(coord);
    m_priorityMeshRequests.erase(coord);
    m_countedMeshRetryRevisions.erase(coord);
    eraseFailure(m_generationErrors, m_generationFailureVersion, coord);
    eraseFailure(m_loadErrors, m_loadFailureVersion, coord);
    eraseFailure(m_meshErrors, m_meshFailureVersion, coord);
    return true;
}

void ChunkStreamer::deferEviction(ChunkCoord coord, bool versionReplacement) {
    uint64_t retrySequence = m_streamingUpdateSequence + kEvictionRetryDelayUpdates;
    m_evictionRetryAfter[coord] = retrySequence;
    setFailure(
        m_evictionErrors,
        m_evictionFailureVersion,
        coord,
        failureDiagnostic(
            "eviction persistence", coord, "persistence did not complete"));
    if (versionReplacement) {
        m_versionReplacementRetries.insert(coord);
        m_versionReplacementWaiting.erase(coord);
    } else {
        m_versionReplacementRetries.erase(coord);
    }
    if (m_nextEvictionRetrySequence == 0 ||
        retrySequence < m_nextEvictionRetrySequence) {
        m_nextEvictionRetrySequence = retrySequence;
    }
}

uint64_t ChunkStreamer::retireIneligibleEvictions(ChunkCoord center,
                                                  int viewRadiusSq,
                                                  int unloadRadiusSq) {
    uint64_t inspected = 0;
    m_nextEvictionRetrySequence = 0;
    for (auto it = m_evictionRetryAfter.begin();
         it != m_evictionRetryAfter.end();) {
        ++inspected;
        const ChunkCoord coord = it->first;
        if (m_versionReplacementRetries.find(coord) !=
            m_versionReplacementRetries.end()) {
            if (m_nextEvictionRetrySequence == 0 ||
                it->second < m_nextEvictionRetrySequence) {
                m_nextEvictionRetrySequence = it->second;
            }
            ++it;
            continue;
        }

        int distSq = distanceSquared(center, coord);
        bool desired = distSq <= viewRadiusSq;
        bool outsideUnloadRadius = distSq > unloadRadiusSq;
        bool cachePressure = m_cache.maxChunks() > 0 &&
            m_cache.size() > m_cache.maxChunks() && !desired;
        if (!outsideUnloadRadius && !cachePressure) {
            eraseFailure(
                m_evictionErrors, m_evictionFailureVersion, coord);
            it = m_evictionRetryAfter.erase(it);
            continue;
        }

        if (m_nextEvictionRetrySequence == 0 ||
            it->second < m_nextEvictionRetrySequence) {
            m_nextEvictionRetrySequence = it->second;
        }
        ++it;
    }
    return inspected;
}

void ChunkStreamer::retryDeferredEvictions(ChunkCoord center, int unloadRadiusSq) {
    if (m_nextEvictionRetrySequence == 0 ||
        m_streamingUpdateSequence < m_nextEvictionRetrySequence) {
        return;
    }

    std::vector<std::pair<ChunkCoord, bool>> due;
    m_nextEvictionRetrySequence = 0;
    for (auto it = m_evictionRetryAfter.begin();
         it != m_evictionRetryAfter.end();) {
        if (it->second <= m_streamingUpdateSequence) {
            bool versionReplacement =
                m_versionReplacementRetries.erase(it->first) > 0;
            due.emplace_back(it->first, versionReplacement);
            it = m_evictionRetryAfter.erase(it);
            continue;
        }
        if (m_nextEvictionRetrySequence == 0 ||
            it->second < m_nextEvictionRetrySequence) {
            m_nextEvictionRetrySequence = it->second;
        }
        ++it;
    }

    for (const auto& [coord, versionReplacement] : due) {
        if (versionReplacement) {
            if (m_desiredSet.find(coord) != m_desiredSet.end()) {
                m_versionReplacementWaiting.insert(coord);
                queueLoadGen(coord);
                continue;
            }
            if (evictChunk(coord, true)) {
                m_cache.erase(coord);
            }
            continue;
        }

        bool outsideUnloadRadius = distanceSquared(center, coord) > unloadRadiusSq;
        bool cachePressure =
            m_cache.maxChunks() > 0 &&
            m_cache.size() > m_cache.maxChunks() &&
            m_desiredSet.find(coord) == m_desiredSet.end();
        if ((outsideUnloadRadius || cachePressure) && evictChunk(coord)) {
            m_cache.erase(coord);
        } else if (!outsideUnloadRadius && !cachePressure) {
            eraseFailure(
                m_evictionErrors, m_evictionFailureVersion, coord);
        }
    }
}

StreamingDiagnosticSnapshot ChunkStreamer::collectDiagnostics() {
    StreamingDiagnosticSnapshot snapshot;
    size_t generationPending = m_generationCapacityWaiting.size();
    size_t meshPending =
        m_missingMeshCapacityWaiting.size() + m_meshDependencyWaiting.size();
    for (const ChunkCoord& coord : m_dirtyMeshQueued) {
        if (m_missingMeshCapacityWaiting.find(coord) ==
                m_missingMeshCapacityWaiting.end() &&
            m_meshDependencyWaiting.find(coord) == m_meshDependencyWaiting.end() &&
            m_meshInFlight.find(coord) == m_meshInFlight.end()) {
            ++meshPending;
        }
    }
    for (const ChunkCoord& coord : m_loadGenQueued) {
        if (m_loadPending.find(coord) != m_loadPending.end()) {
            continue;
        }

        ChunkState state = ChunkState::Missing;
        auto stateIt = m_states.find(coord);
        if (stateIt != m_states.end()) {
            state = stateIt->second;
        }
        if (state == ChunkState::QueuedGen || state == ChunkState::QueuedMesh) {
            continue;
        }

        Chunk* chunk = m_chunkManager ? m_chunkManager->getChunk(coord) : nullptr;
        if (!chunk) {
            ++generationPending;
            continue;
        }
        if (chunk->isEmpty() ||
            m_missingMeshCapacityWaiting.find(coord) !=
                m_missingMeshCapacityWaiting.end() ||
            m_meshDependencyWaiting.find(coord) != m_meshDependencyWaiting.end() ||
            m_dirtyMeshQueued.find(coord) != m_dirtyMeshQueued.end()) {
            continue;
        }

        bool hasMesh = m_meshStore && m_meshStore->contains(coord);
        bool isMeshed = hasMesh || state == ChunkState::ReadyMesh;
        if (!isMeshed || chunk->isDirty()) {
            ++meshPending;
        }
    }

    snapshot.generation = StreamingWorkCount{
        .pending = generationPending,
        .inFlight = m_inFlightGen,
        .started = m_workMetrics.generationJobsStarted,
        .terminalErrors = m_generationErrors.size(),
        .lastError = diagnosticForLowestCoordinate(m_generationErrors),
        .failureVersion = m_generationFailureVersion
    };
    snapshot.mesh = StreamingWorkCount{
        .pending = meshPending,
        .inFlight = m_inFlightMesh,
        .started = m_workMetrics.meshJobsStarted,
        .terminalErrors = m_meshErrors.size(),
        .lastError = diagnosticForLowestCoordinate(m_meshErrors),
        .failureVersion = m_meshFailureVersion
    };
    if (m_chunkLoadWork) {
        snapshot.chunkLoad = m_chunkLoadWork();
    } else {
        snapshot.chunkLoad = StreamingWorkCount{
            .pending = m_loadPending.size(),
            .inFlight = 0,
            .started = m_workMetrics.chunkLoadRequestsStarted
        };
    }
    snapshot.chunkLoad.terminalErrors += m_loadErrors.size();
    if (!m_loadErrors.empty()) {
        snapshot.chunkLoad.lastError =
            diagnosticForLowestCoordinate(m_loadErrors);
    }
    if (snapshot.chunkLoad.failureVersion != m_observedLoaderFailureVersion ||
        m_loadFailureVersion != m_observedLoadFailureVersion) {
        m_observedLoaderFailureVersion = snapshot.chunkLoad.failureVersion;
        m_observedLoadFailureVersion = m_loadFailureVersion;
        advanceFailureVersion(m_chunkLoadFailureVersion);
    }
    snapshot.chunkLoad.failureVersion = m_chunkLoadFailureVersion;
    snapshot.eviction = StreamingWorkCount{
        .pending = m_evictionRetryAfter.size() +
            m_versionReplacementWaiting.size(),
        .terminalErrors = m_evictionErrors.size(),
        .lastError = diagnosticForLowestCoordinate(m_evictionErrors),
        .failureVersion = m_evictionFailureVersion
    };
    return snapshot;
}

void ChunkStreamer::refreshDiagnostics(bool advanceWindow) {
    StreamingDiagnosticSnapshot next = collectDiagnostics();
    next.state = m_diagnostics.state;
    next.stableUpdates = m_diagnostics.stableUpdates;

    if (!m_spawnDiscoveryComplete) {
        next.state = StreamingLifecycleState::DiscoveringSpawn;
        next.stableUpdates = 0;
    } else if (!m_initialStreamingBegun) {
        next.state = StreamingLifecycleState::AwaitingInitialStream;
        next.stableUpdates = 0;
    } else if (!next.workEmpty() || m_workObservedThisUpdate || m_workStartedThisUpdate) {
        next.state = StreamingLifecycleState::Streaming;
        next.stableUpdates = 0;
    } else if (advanceWindow) {
        if (next.stableUpdates < StreamingDiagnosticSnapshot::QuiescenceUpdateWindow) {
            ++next.stableUpdates;
        }
        next.state =
            next.stableUpdates >= StreamingDiagnosticSnapshot::QuiescenceUpdateWindow
            ? StreamingLifecycleState::Quiescent
            : StreamingLifecycleState::Stabilizing;
    }

    m_diagnostics = next;
}

void ChunkStreamer::applyGenCompletions(size_t budget) {
    size_t applied = 0;
    GenResult genResult;
    while (applied < budget && m_genComplete.tryPop(genResult)) {
        if (m_inFlightGen > 0) {
            --m_inFlightGen;
        }
        wakeGenerationCapacityWaiter();

        if (genResult.workEpoch !=
            m_workEpoch.load(std::memory_order_relaxed)) {
            continue;
        }
        if (!m_generator ||
            genResult.worldGenVersion != m_generator->config().world.version) {
            continue;
        }

        if (genResult.cancelToken) {
            auto cancelIt = m_genCancel.find(genResult.coord);
            if (cancelIt != m_genCancel.end() && cancelIt->second == genResult.cancelToken) {
                m_genCancel.erase(cancelIt);
            }
        }

        if (genResult.cancelled || (genResult.cancelToken &&
            genResult.cancelToken->load(std::memory_order_relaxed))) {
            continue;
        }

        auto stateIt = m_states.find(genResult.coord);
        if (stateIt == m_states.end() || stateIt->second != ChunkState::QueuedGen) {
            continue;
        }

        if (genResult.failed) {
            stateIt->second = ChunkState::GenerationFailed;
            ++m_workMetrics.generationJobsFailed;
            setFailure(
                m_generationErrors,
                m_generationFailureVersion,
                genResult.coord,
                failureDiagnostic(
                    "generation", genResult.coord, genResult.error));
            spdlog::error("Chunk generation failed at ({}, {}, {}): {}",
                          genResult.coord.x,
                          genResult.coord.y,
                          genResult.coord.z,
                          genResult.error);
            completePendingVisibilityTrace(
                genResult.coord,
                ChunkVisibilityOutcome::Failed);
            ++applied;
            continue;
        }

        cancelPendingLoad(genResult.coord);
        eraseFailure(
            m_generationErrors, m_generationFailureVersion, genResult.coord);
        eraseFailure(m_loadErrors, m_loadFailureVersion, genResult.coord);
        m_versionReplacementWaiting.erase(genResult.coord);
        Chunk& chunk = m_chunkManager->getOrCreateChunk(genResult.coord);
        if (m_registry) {
            chunk.copyFrom(genResult.blocks, *m_registry);
        } else {
            chunk.copyFrom(genResult.blocks);
        }
        chunk.clearPersistDirty();
        chunk.setLoadedFromDisk(false);
        chunk.setWorldGenVersion(genResult.worldGenVersion);
        observeVisibilityDataReady(
            genResult.coord, ChunkVisibilityOrigin::Generated);
        observeVisibilityNeighborReadiness(genResult.coord);

        if (m_benchmark) {
            m_benchmark->addGeneration(genResult.seconds);
        }

        if (chunk.isEmpty()) {
            if (m_meshStore) {
                m_meshStore->remove(genResult.coord);
            }
            chunk.clearDirty();
            m_priorityMeshRequests.erase(genResult.coord);
            eraseFailure(m_meshErrors, m_meshFailureVersion, genResult.coord);
            stateIt->second = ChunkState::ReadyMesh;
            completePendingVisibilityTrace(
                genResult.coord,
                ChunkVisibilityOutcome::VoxelEmpty);
        } else {
            stateIt->second = ChunkState::ReadyData;
        }
        queueLoadGen(genResult.coord);
        queueLoadedNeighbors(genResult.coord);
        if (!chunk.isEmpty()) {
            m_chunkManager->invalidateFaceNeighbors(genResult.coord);
        }
        ++applied;
    }
}

void ChunkStreamer::applyMeshCompletions(size_t budget) {
    size_t applied = 0;
    MeshResult meshResult;
    while (applied < budget && m_meshComplete.tryPop(meshResult)) {
        ++m_workMetrics.meshJobsCompleted;
        auto completeVisibility = [&](ChunkVisibilityOutcome outcome) {
            if (meshResult.visibilityTracer && meshResult.visibilityTrace) {
                meshResult.visibilityTracer->complete(
                    *meshResult.visibilityTrace, outcome);
            }
        };
        auto flightIt = m_meshInFlight.find(meshResult.coord);
        if (flightIt == m_meshInFlight.end() ||
            flightIt->second.requestId != meshResult.requestId) {
            ++m_workMetrics.meshJobsRejectedStale;
            completeVisibility(ChunkVisibilityOutcome::Stale);
            queueLoadGen(meshResult.coord);
            continue;
        }

        MeshInFlight flight = flightIt->second;
        if (m_inFlightMesh > 0) {
            --m_inFlightMesh;
        }
        if (flight.kind == MeshRequestKind::Missing) {
            if (m_inFlightMeshMissing > 0) {
                --m_inFlightMeshMissing;
            }
        } else if (m_inFlightMeshDirty > 0) {
            --m_inFlightMeshDirty;
        }
        m_meshInFlight.erase(flightIt);
        wakeMissingMeshCapacityWaiter();

        if (flight.obsolete ||
            flight.workEpoch != m_workEpoch.load(std::memory_order_relaxed) ||
            meshResult.workEpoch != flight.workEpoch) {
            ++m_workMetrics.meshJobsRejectedStale;
            completeVisibility(ChunkVisibilityOutcome::Stale);
            continue;
        }

        auto stateIt = m_states.find(meshResult.coord);
        if (stateIt == m_states.end() || stateIt->second != ChunkState::QueuedMesh) {
            ++m_workMetrics.meshJobsRejectedStale;
            completeVisibility(ChunkVisibilityOutcome::Stale);
            queueLoadGen(meshResult.coord);
            continue;
        }

        Chunk* chunk = m_chunkManager->getChunk(meshResult.coord);
        if (!chunk) {
            m_states.erase(meshResult.coord);
            ++m_workMetrics.meshJobsRejectedStale;
            completeVisibility(ChunkVisibilityOutcome::Stale);
            continue;
        }

        uint32_t currentRevision = chunk->meshRevision();
        bool replaced = chunk->m_instanceId != meshResult.chunkInstanceId;
        if (replaced || currentRevision != meshResult.revision) {
            if (replaced || flight.observedRevision != currentRevision) {
                ++m_workMetrics.meshInvalidations;
                ++m_workMetrics.meshRequestsCoalesced;
            }
            if (!chunk->isEmpty()) {
                chunk->markDirty();
            }
            if (flight.kind == MeshRequestKind::Dirty) {
                m_countedMeshRetryRevisions[meshResult.coord] = currentRevision;
            }
            stateIt->second = ChunkState::ReadyData;
            ++m_workMetrics.meshJobsRejectedStale;
            completeVisibility(ChunkVisibilityOutcome::Stale);
            queueLoadGen(meshResult.coord);
            queueDirtyMesh(meshResult.coord, flight.prioritized);
            continue;
        }

        std::optional<ChunkVisibilityTraceLink> visibilityTrace;
        if (flight.visibilityTracer && flight.visibilityTrace) {
            visibilityTrace = ChunkVisibilityTraceLink{
                *flight.visibilityTrace,
                flight.visibilityKind,
                flight.visibilityTracer
            };
        }

        if (meshResult.failed) {
            chunk->m_dirty = true;
            stateIt->second = ChunkState::MeshFailed;
            m_dirtyMeshQueued.erase(meshResult.coord);
            m_missingMeshCapacityWaiting.erase(meshResult.coord);
            m_meshDependencyWaiting.erase(meshResult.coord);
            m_priorityMeshRequests.erase(meshResult.coord);
            ++m_workMetrics.meshJobsFailed;
            completeVisibility(ChunkVisibilityOutcome::Failed);
            setFailure(
                m_meshErrors,
                m_meshFailureVersion,
                meshResult.coord,
                failureDiagnostic(
                    "mesh build", meshResult.coord, meshResult.error));
            spdlog::error("Chunk mesh build failed at ({}, {}, {}): {}",
                          meshResult.coord.x,
                          meshResult.coord.y,
                          meshResult.coord.z,
                          meshResult.error);
            ++applied;
            continue;
        }

        if (meshResult.empty) {
            if (m_meshStore) {
                if (chunk->isEmpty()) {
                    m_meshStore->remove(meshResult.coord);
                } else {
                    m_meshStore->set(
                        meshResult.coord,
                        std::move(meshResult.mesh),
                        visibilityTrace);
                }
            }
        } else if (m_meshStore) {
            m_meshStore->set(
                meshResult.coord,
                std::move(meshResult.mesh),
                visibilityTrace);
        }
        chunk->clearDirty();
        eraseFailure(m_meshErrors, m_meshFailureVersion, meshResult.coord);
        stateIt->second = ChunkState::ReadyMesh;
        completeVisibility(meshResult.empty
                ? ChunkVisibilityOutcome::AcceptedEmptyGeometry
                : ChunkVisibilityOutcome::AcceptedNonemptyGeometry);

        if (m_benchmark) {
            m_benchmark->addMesh(meshResult.seconds, meshResult.empty);
        }
        ++m_workMetrics.meshJobsAccepted;
        ++applied;
    }
}

void ChunkStreamer::cancelPendingLoad(ChunkCoord coord) {
    auto pendingIt = m_loadPending.find(coord);
    if (pendingIt == m_loadPending.end()) {
        if (m_chunkManager && m_chunkManager->getChunk(coord)) {
            eraseFailure(m_loadErrors, m_loadFailureVersion, coord);
        }
        return;
    }
    if (m_chunkLoadCancel) {
        m_chunkLoadCancel(coord);
    }
    m_loadPending.erase(pendingIt);
    eraseFailure(m_loadErrors, m_loadFailureVersion, coord);
}

ChunkLoadRequestId ChunkStreamer::nextLoadRequestId() {
    ChunkLoadRequestId requestId = m_nextLoadRequestId++;
    if (m_nextLoadRequestId == 0) {
        m_nextLoadRequestId = 1;
    }
    return requestId;
}

void ChunkStreamer::queueLoadGen(ChunkCoord coord) {
    if (m_desiredSet.find(coord) == m_desiredSet.end()) {
        return;
    }
    m_generationCapacityWaiting.erase(coord);
    m_missingMeshCapacityWaiting.erase(coord);
    m_meshDependencyWaiting.erase(coord);
    if (m_loadGenQueued.insert(coord).second) {
        m_loadGenQueue.push_back(coord);
    }
    if (m_priorityMeshRequests.find(coord) != m_priorityMeshRequests.end()) {
        queueDirtyMesh(coord);
    }
    markVisibilityMeshEligible(coord, false);
}

void ChunkStreamer::waitForGenerationCapacity(ChunkCoord coord) {
    if (m_desiredSet.find(coord) == m_desiredSet.end()) {
        return;
    }
    if (m_generationCapacityWaiting.insert(coord).second) {
        m_generationCapacityWait.push_back(coord);
    }
}

void ChunkStreamer::waitForMissingMeshCapacity(ChunkCoord coord) {
    if (m_desiredSet.find(coord) == m_desiredSet.end()) {
        return;
    }
    if (m_missingMeshCapacityWaiting.insert(coord).second) {
        m_missingMeshCapacityWait.push_back(coord);
    }
}

void ChunkStreamer::waitForMeshDependencies(ChunkCoord coord) {
    if (!dirtyMeshPriority(coord)) {
        return;
    }
    m_meshDependencyWaiting.insert(coord);
}

void ChunkStreamer::wakeGenerationCapacityWaiter() {
    while (!m_generationCapacityWait.empty()) {
        ChunkCoord coord = m_generationCapacityWait.front();
        m_generationCapacityWait.pop_front();
        if (m_generationCapacityWaiting.erase(coord) == 0 ||
            m_desiredSet.find(coord) == m_desiredSet.end()) {
            continue;
        }
        queueLoadGen(coord);
        return;
    }
}

void ChunkStreamer::wakeMissingMeshCapacityWaiter() {
    while (!m_missingMeshCapacityWait.empty()) {
        ChunkCoord coord = m_missingMeshCapacityWait.front();
        m_missingMeshCapacityWait.pop_front();
        if (m_missingMeshCapacityWaiting.erase(coord) == 0 ||
            m_desiredSet.find(coord) == m_desiredSet.end()) {
            continue;
        }
        queueLoadGen(coord);
        return;
    }
}

void ChunkStreamer::queueLoadedNeighbors(ChunkCoord coord) {
    if (!m_chunkManager) {
        return;
    }
    for (int i = 0; i < DirectionCount; ++i) {
        Direction dir = static_cast<Direction>(i);
        int dx = 0;
        int dy = 0;
        int dz = 0;
        directionOffset(dir, dx, dy, dz);
        ChunkCoord neighbor = coord.offset(dx, dy, dz);
        if (m_chunkManager->getChunk(neighbor)) {
            if (m_desiredSet.find(neighbor) != m_desiredSet.end()) {
                queueLoadGen(neighbor);
            } else if (m_meshDependencyWaiting.find(neighbor) !=
                       m_meshDependencyWaiting.end()) {
                queueDirtyMesh(neighbor);
            }
        }
    }
}

std::optional<size_t> ChunkStreamer::dirtyMeshPriority(ChunkCoord coord) const {
    auto priorityIt = m_desiredPriority.find(coord);
    if (priorityIt != m_desiredPriority.end()) {
        return priorityIt->second;
    }
    if (m_chunkManager && m_chunkManager->getChunk(coord) &&
        m_meshStore && m_meshStore->contains(coord)) {
        return m_desired.size();
    }
    return std::nullopt;
}

void ChunkStreamer::queueDirtyMesh(ChunkCoord coord, bool prioritize) {
    const auto priority = dirtyMeshPriority(coord);
    if (!priority) {
        return;
    }
    Chunk* chunk = m_chunkManager
        ? m_chunkManager->getChunk(coord)
        : nullptr;
    const bool hasMesh = m_meshStore && m_meshStore->contains(coord);
    const bool meshInFlight =
        m_meshInFlight.find(coord) != m_meshInFlight.end();
    const bool replacementNeeded =
        (!hasMesh || (chunk && chunk->isDirty())) &&
        (!meshInFlight || (chunk && chunk->isDirty()));
    if (replacementNeeded) {
        const auto lifecycleKind = hasMesh
            ? ChunkVisibilityLifecycleKind::Remesh
            : ChunkVisibilityLifecycleKind::CameraDemand;
        ensureVisibilityTrace(
            coord,
            lifecycleKind,
            lifecycleKind == ChunkVisibilityLifecycleKind::Remesh
                ? ChunkVisibilityOrigin::Remesh
                : ChunkVisibilityOrigin::ResidentLeftCensored);
    }
    m_meshDependencyWaiting.erase(coord);
    bool newlyQueued = m_dirtyMeshQueued.insert(coord).second;
    bool promoted = prioritize && m_priorityMeshRequests.insert(coord).second;
    bool prioritized = m_priorityMeshRequests.find(coord) !=
        m_priorityMeshRequests.end();
    if (newlyQueued || promoted) {
        m_dirtyMeshQueue.push({*priority, coord, prioritized});
    }
    markVisibilityMeshEligible(coord, false);
}

void ChunkStreamer::ensureVisibilityTrace(
    ChunkCoord coord,
    ChunkVisibilityLifecycleKind kind,
    ChunkVisibilityOrigin origin) {
    if (!m_visibilityTracer || !m_visibilityTracer->traces(coord)) {
        return;
    }
    auto& slot = m_pendingVisibilityTraces[visibilityKindIndex(kind)];
    if (slot && slot->key.coord == coord) {
        return;
    }

    PendingVisibilityTrace pending;
    pending.kind = kind;
    pending.tracer = m_visibilityTracer;
    const auto key = pending.tracer->begin(pending.kind, origin);
    if (!key) {
        return;
    }
    pending.key = *key;
    slot = std::move(pending);
}

void ChunkStreamer::beginCameraVisibilityTrace(ChunkCoord coord) {
    const auto kind = ChunkVisibilityLifecycleKind::CameraDemand;
    if (!m_visibilityTracer || !m_visibilityTracer->traces(coord)) {
        return;
    }
    ensureVisibilityTrace(
        coord,
        kind,
        m_chunkManager && m_chunkManager->getChunk(coord)
            ? ChunkVisibilityOrigin::ResidentLeftCensored
            : ChunkVisibilityOrigin::Unresolved);
    markVisibilityStage(coord, ChunkVisibilityStage::Desired);
    auto& pending = m_pendingVisibilityTraces[visibilityKindIndex(kind)];
    if (!pending || pending->key.coord != coord || !m_meshStore) {
        return;
    }

    const ChunkVisibilityTraceLink link{
        pending->key,
        pending->kind,
        pending->tracer
    };
    const auto attachment =
        m_meshStore->attachCachedVisibilityTrace(coord, link);
    if (attachment == CachedMeshTraceAttachment::Missing) {
        return;
    }
    completePendingVisibilityTrace(
        coord,
        kind,
        attachment == CachedMeshTraceAttachment::EmptyGeometry
            ? ChunkVisibilityOutcome::CachedEmptyGeometry
            : ChunkVisibilityOutcome::CachedNonemptyGeometry);
}

void ChunkStreamer::observeVisibilityDataReady(
    ChunkCoord coord,
    ChunkVisibilityOrigin origin) {
    const auto kind = ChunkVisibilityLifecycleKind::CameraDemand;
    ensureVisibilityTrace(coord, kind);
    auto& pending = m_pendingVisibilityTraces[visibilityKindIndex(kind)];
    if (!pending || pending->key.coord != coord) {
        return;
    }
    pending->tracer->markDataReady(pending->key, origin);
}

bool ChunkStreamer::areFaceNeighbors(ChunkCoord lhs, ChunkCoord rhs) {
    const int64_t dx = std::abs(
        static_cast<int64_t>(lhs.x) - static_cast<int64_t>(rhs.x));
    const int64_t dy = std::abs(
        static_cast<int64_t>(lhs.y) - static_cast<int64_t>(rhs.y));
    const int64_t dz = std::abs(
        static_cast<int64_t>(lhs.z) - static_cast<int64_t>(rhs.z));
    return dx + dy + dz == 1;
}

void ChunkStreamer::observeVisibilityNeighborReadiness(ChunkCoord coord) {
    if (!m_visibilityTracer) {
        return;
    }
    const ChunkCoord traced = m_visibilityTracer->coord();
    if (coord == traced) {
        markVisibilityMeshEligible(traced, false);
    } else if (areFaceNeighbors(traced, coord) &&
               m_desiredSet.find(coord) != m_desiredSet.end()) {
        markVisibilityMeshEligible(traced, true);
    }
}

void ChunkStreamer::markVisibilityMeshEligible(
    ChunkCoord coord,
    bool neighborBecameReady) {
    if (!m_visibilityTracer || !m_visibilityTracer->traces(coord) ||
        !m_chunkManager || !m_meshStore) {
        return;
    }

    Chunk* chunk = m_chunkManager->getChunk(coord);
    if (!chunk) {
        return;
    }

    ChunkState state = ChunkState::Missing;
    auto stateIt = m_states.find(coord);
    if (stateIt != m_states.end()) {
        state = stateIt->second;
    }
    const bool isMeshed =
        m_meshStore->contains(coord) || state == ChunkState::ReadyMesh;
    const bool initialMeshRequested = !isMeshed &&
        (m_desiredSet.find(coord) != m_desiredSet.end() ||
         m_priorityMeshRequests.find(coord) !=
             m_priorityMeshRequests.end());
    const bool meshInFlight =
        m_meshInFlight.find(coord) != m_meshInFlight.end();
    const bool remeshRequested = chunk->isDirty() &&
        (isMeshed || meshInFlight);
    if ((!initialMeshRequested && !remeshRequested) || chunk->isEmpty() ||
        !hasAllNeighborsLoaded(coord)) {
        return;
    }
    if (initialMeshRequested &&
        m_loadPending.find(coord) != m_loadPending.end()) {
        return;
    }
    if (meshInFlight && !neighborBecameReady) {
        return;
    }

    const auto kind = remeshRequested
        ? ChunkVisibilityLifecycleKind::Remesh
        : ChunkVisibilityLifecycleKind::CameraDemand;
    if (neighborBecameReady) {
        ensureVisibilityTrace(
            coord,
            kind,
            kind == ChunkVisibilityLifecycleKind::Remesh
                ? ChunkVisibilityOrigin::Remesh
                : ChunkVisibilityOrigin::ResidentLeftCensored);
    }
    auto& pending = m_pendingVisibilityTraces[visibilityKindIndex(kind)];
    if (!pending || pending->key.coord != coord) {
        return;
    }

    if (neighborBecameReady && meshInFlight) {
        pending->tracer->mark(
            pending->key,
            ChunkVisibilityStage::NeighborReady);
    } else if (neighborBecameReady) {
        pending->tracer->mark(
            pending->key,
            {
                ChunkVisibilityStage::NeighborReady,
                ChunkVisibilityStage::MeshEligible,
                ChunkVisibilityStage::SchedulerWait
            });
    } else if (!meshInFlight) {
        pending->tracer->mark(
            pending->key,
            {
                ChunkVisibilityStage::MeshEligible,
                ChunkVisibilityStage::SchedulerWait
            });
    }
}

void ChunkStreamer::markVisibilityStage(ChunkCoord coord,
                                        ChunkVisibilityStage stage) {
    const auto kind = ChunkVisibilityLifecycleKind::CameraDemand;
    auto& pending = m_pendingVisibilityTraces[visibilityKindIndex(kind)];
    if ((!pending || pending->key.coord != coord) &&
        stage == ChunkVisibilityStage::DataRequest) {
        ensureVisibilityTrace(coord, kind);
    }
    if (!pending || pending->key.coord != coord ||
        stage == ChunkVisibilityStage::Count) {
        return;
    }
    pending->tracer->mark(pending->key, stage);
}

std::optional<ChunkVisibilityTraceLink>
ChunkStreamer::bindVisibilityTrace(
    ChunkCoord coord,
    const ChunkVisibilityMeshTaskIdentity& meshTask,
    ChunkVisibilityLifecycleKind kind) {
    ensureVisibilityTrace(
        coord,
        kind,
        kind == ChunkVisibilityLifecycleKind::Remesh
            ? ChunkVisibilityOrigin::Remesh
            : ChunkVisibilityOrigin::ResidentLeftCensored);
    auto& pending = m_pendingVisibilityTraces[visibilityKindIndex(kind)];
    if (!pending || pending->key.coord != coord) {
        return std::nullopt;
    }

    pending->tracer->bindMeshTask(pending->key, meshTask);
    const ChunkVisibilityTraceLink link{
        pending->key,
        pending->kind,
        pending->tracer
    };
    pending.reset();
    return link;
}

void ChunkStreamer::completePendingVisibilityTrace(
    ChunkCoord coord,
    ChunkVisibilityOutcome outcome) {
    for (const auto kind : {
             ChunkVisibilityLifecycleKind::CameraDemand,
             ChunkVisibilityLifecycleKind::Remesh}) {
        completePendingVisibilityTrace(coord, kind, outcome);
    }
}

void ChunkStreamer::completePendingVisibilityTrace(
    ChunkCoord coord,
    ChunkVisibilityLifecycleKind kind,
    ChunkVisibilityOutcome outcome) {
    auto& pending = m_pendingVisibilityTraces[visibilityKindIndex(kind)];
    if (!pending || pending->key.coord != coord) {
        return;
    }
    pending->tracer->complete(pending->key, outcome);
    pending.reset();
}

void ChunkStreamer::completeInFlightVisibilityTrace(
    MeshInFlight& flight,
    ChunkVisibilityOutcome outcome) {
    if (flight.visibilityTracer && flight.visibilityTrace) {
        flight.visibilityTracer->complete(*flight.visibilityTrace, outcome);
        flight.visibilityTrace.reset();
        flight.visibilityTracer.reset();
    }
}

void ChunkStreamer::abandonVisibilityTraces(
    ChunkVisibilityOutcome outcome) {
    for (const auto& pending : m_pendingVisibilityTraces) {
        if (pending) {
            completePendingVisibilityTrace(
                pending->key.coord,
                outcome);
            break;
        }
    }

    for (auto& [coord, flight] : m_meshInFlight) {
        completeInFlightVisibilityTrace(flight, outcome);
    }

    MeshResult meshResult;
    while (m_meshComplete.tryPop(meshResult)) {
        if (meshResult.visibilityTracer && meshResult.visibilityTrace) {
            meshResult.visibilityTracer->complete(
                *meshResult.visibilityTrace,
                outcome);
        }
    }
}

void ChunkStreamer::reprioritizeDirtyMeshes() {
    decltype(m_dirtyMeshQueue) prioritized;
    std::unordered_set<ChunkCoord, ChunkCoordHash> retained;
    retained.reserve(m_dirtyMeshQueued.size());
    for (auto it = m_priorityMeshRequests.begin();
         it != m_priorityMeshRequests.end(); ) {
        if (!dirtyMeshPriority(*it)) {
            it = m_priorityMeshRequests.erase(it);
        } else {
            ++it;
        }
    }
    for (const ChunkCoord& coord : m_dirtyMeshQueued) {
        const auto priority = dirtyMeshPriority(coord);
        if (!priority) {
            continue;
        }
        bool isPrioritized = m_priorityMeshRequests.find(coord) !=
            m_priorityMeshRequests.end();
        retained.insert(coord);
        prioritized.push({*priority, coord, isPrioritized});
    }
    m_dirtyMeshQueue = std::move(prioritized);
    m_dirtyMeshQueued = std::move(retained);
}

void ChunkStreamer::enqueueGeneration(ChunkCoord coord) {
    if (m_config.genQueueLimit > 0 &&
        m_inFlightGen >= m_config.genQueueLimit) {
        return;
    }

    ensureVisibilityTrace(
        coord,
        ChunkVisibilityLifecycleKind::CameraDemand);
    markVisibilityStage(coord, ChunkVisibilityStage::DataRequest);

    m_states[coord] = ChunkState::QueuedGen;
    ++m_inFlightGen;
    ++m_workMetrics.generationJobsStarted;

    auto cancelToken = std::make_shared<std::atomic_bool>(false);
    m_genCancel[coord] = cancelToken;
    auto generator = m_generator;
    const uint32_t worldGenVersion = generator
        ? generator->config().world.version
        : 0;
    uint64_t workEpoch = m_workEpoch.load(std::memory_order_relaxed);
    auto generationStartCallback = m_generationStartCallback;
    auto job = [this,
                generator,
                coord,
                cancelToken,
                worldGenVersion,
                workEpoch,
                generationStartCallback = std::move(generationStartCallback)]() {
        GenResult result;
        result.coord = coord;
        result.workEpoch = workEpoch;
        result.worldGenVersion = worldGenVersion;
        result.cancelToken = cancelToken;
        if (cancelToken->load(std::memory_order_relaxed)) {
            result.cancelled = true;
            m_genComplete.push(std::move(result));
            return;
        }

        try {
            if (generationStartCallback) {
                generationStartCallback();
            }
            if (workEpoch != m_workEpoch.load(std::memory_order_relaxed) ||
                cancelToken->load(std::memory_order_relaxed)) {
                result.cancelled = true;
                m_genComplete.push(std::move(result));
                return;
            }
            ChunkBuffer buffer;
            auto start = std::chrono::steady_clock::now();
            generator->generate(coord, buffer, cancelToken.get());
            auto end = std::chrono::steady_clock::now();

            result.blocks = buffer.blocks;
            result.seconds = std::chrono::duration<double>(end - start).count();
        } catch (const std::exception& e) {
            result.failed = true;
            result.error = e.what();
        } catch (...) {
            result.failed = true;
            result.error = "unknown error";
        }
        result.cancelled = cancelToken->load(std::memory_order_relaxed);
        m_genComplete.push(std::move(result));
    };

    if (m_genPool && m_genPool->threadCount() > 0) {
        m_genPool->enqueue(std::move(job));
    } else {
        job();
    }
}

void ChunkStreamer::enqueueMesh(ChunkCoord coord,
                                Chunk& chunk,
                                MeshRequestKind kind,
                                bool prioritized) {
    if (!m_registry) {
        return;
    }
    if (m_config.meshQueueLimit > 0 &&
        m_inFlightMesh >= m_config.meshQueueLimit) {
        return;
    }
    if (m_meshInFlight.find(coord) != m_meshInFlight.end()) {
        return;
    }

    prioritized = prioritized ||
        m_priorityMeshRequests.find(coord) != m_priorityMeshRequests.end();

    chunk.clearDirty();

    MeshTask task;
    task.coord = coord;
    task.requestId = m_nextMeshRequestId++;
    task.workEpoch = m_workEpoch.load(std::memory_order_relaxed);
    if (m_nextMeshRequestId == 0) {
        m_nextMeshRequestId = 1;
    }
    task.chunkInstanceId = chunk.m_instanceId;
    task.revision = chunk.meshRevision();
    chunk.copyBlocks(task.blocks);

    std::array<const Chunk*, 27> neighborChunks{};
    auto neighborIndex = [](int dx, int dy, int dz) {
        return (dx + 1) + (dy + 1) * 3 + (dz + 1) * 9;
    };
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                ChunkCoord neighborCoord = coord.offset(dx, dy, dz);
                neighborChunks[neighborIndex(dx, dy, dz)] = m_chunkManager->getChunk(neighborCoord);
            }
        }
    }
    neighborChunks[neighborIndex(0, 0, 0)] = &chunk;

    BlockState air;
    for (int pz = 0; pz < kPaddedSize; ++pz) {
        int lz = pz - 1;
        for (int py = 0; py < kPaddedSize; ++py) {
            int ly = py - 1;
            for (int px = 0; px < kPaddedSize; ++px) {
                int lx = px - 1;

                int ox = 0;
                int oy = 0;
                int oz = 0;
                int sx = lx;
                int sy = ly;
                int sz = lz;

                if (sx < 0) {
                    ox = -1;
                    sx += Chunk::SIZE;
                } else if (sx >= Chunk::SIZE) {
                    ox = 1;
                    sx -= Chunk::SIZE;
                }
                if (sy < 0) {
                    oy = -1;
                    sy += Chunk::SIZE;
                } else if (sy >= Chunk::SIZE) {
                    oy = 1;
                    sy -= Chunk::SIZE;
                }
                if (sz < 0) {
                    oz = -1;
                    sz += Chunk::SIZE;
                } else if (sz >= Chunk::SIZE) {
                    oz = 1;
                    sz -= Chunk::SIZE;
                }

                const Chunk* source = neighborChunks[neighborIndex(ox, oy, oz)];
                size_t index = static_cast<size_t>(px)
                    + static_cast<size_t>(py) * kPaddedSize
                    + static_cast<size_t>(pz) * kPaddedSize * kPaddedSize;
                if (source) {
                    task.paddedBlocks[index] = source->getBlock(sx, sy, sz);
                } else {
                    task.paddedBlocks[index] = air;
                }
            }
        }
    }

    const ChunkVisibilityLifecycleKind visibilityKind =
        kind == MeshRequestKind::Dirty
        ? ChunkVisibilityLifecycleKind::Remesh
        : ChunkVisibilityLifecycleKind::CameraDemand;
    const auto visibilityLink = bindVisibilityTrace(
        coord,
        ChunkVisibilityMeshTaskIdentity{
            task.requestId,
            task.workEpoch,
            task.chunkInstanceId,
            task.revision
        },
        visibilityKind);
    if (visibilityLink) {
        task.visibilityTrace = visibilityLink->key;
        task.visibilityTracer = visibilityLink->tracer;
        task.visibilityKind = visibilityLink->kind;
    }

    m_states[coord] = ChunkState::QueuedMesh;
    ++m_inFlightMesh;
    m_meshInFlight[coord] = MeshInFlight{
        .kind = kind,
        .requestId = task.requestId,
        .workEpoch = task.workEpoch,
        .observedRevision = task.revision,
        .prioritized = prioritized,
        .visibilityTrace = task.visibilityTrace,
        .visibilityTracer = task.visibilityTracer,
        .visibilityKind = task.visibilityKind
    };
    m_priorityMeshRequests.erase(coord);
    ++m_workMetrics.meshJobsStarted;
    if (kind == MeshRequestKind::Missing) {
        ++m_inFlightMeshMissing;
    } else {
        ++m_inFlightMeshDirty;
        auto retryIt = m_countedMeshRetryRevisions.find(coord);
        bool invalidationAlreadyCounted = retryIt != m_countedMeshRetryRevisions.end() &&
            retryIt->second == task.revision;
        if (retryIt != m_countedMeshRetryRevisions.end()) {
            m_countedMeshRetryRevisions.erase(retryIt);
        }
        if (!invalidationAlreadyCounted) {
            ++m_workMetrics.meshInvalidations;
        }
    }
    if (m_config.meshQueueLimit == 1) {
        m_nextSingleSlotMeshKind = kind == MeshRequestKind::Missing
            ? MeshRequestKind::Dirty
            : MeshRequestKind::Missing;
    }

    BlockRegistry* registry = m_registry;
    TextureAtlas* atlas = m_atlas;
    const auto visibilityTrace = task.visibilityTrace;
    const auto visibilityTracer = task.visibilityTracer;
    auto meshBuildStartCallback = m_meshBuildStartCallback;
    auto job = [this,
                task = std::move(task),
                registry,
                atlas,
                meshBuildStartCallback = std::move(meshBuildStartCallback)]() mutable {
        MeshResult result;
        result.coord = task.coord;
        result.requestId = task.requestId;
        result.workEpoch = task.workEpoch;
        result.chunkInstanceId = task.chunkInstanceId;
        result.revision = task.revision;
        result.visibilityTrace = task.visibilityTrace;
        result.visibilityTracer = task.visibilityTracer;
        if (result.visibilityTracer && result.visibilityTrace) {
            result.visibilityTracer->mark(
                *result.visibilityTrace,
                ChunkVisibilityStage::WorkerStart);
        }
        try {
            if (meshBuildStartCallback) {
                meshBuildStartCallback();
            }
            if (task.workEpoch ==
                m_workEpoch.load(std::memory_order_relaxed)) {
                Chunk chunk(task.coord);
                chunk.copyFrom(task.blocks);

                std::array<const Chunk*, DirectionCount> neighborPtrs{};

                MeshBuilder builder;
                MeshBuilder::BuildContext ctx{
                    .chunk = chunk,
                    .registry = *registry,
                    .atlas = atlas,
                    .neighbors = neighborPtrs,
                    .paddedBlocks = &task.paddedBlocks
                };

                auto start = std::chrono::steady_clock::now();
                result.mesh = builder.build(ctx);
                auto end = std::chrono::steady_clock::now();

                result.seconds =
                    std::chrono::duration<double>(end - start).count();
                result.empty = result.mesh.isEmpty();
            }
        } catch (const std::exception& e) {
            result.failed = true;
            result.error = e.what();
        } catch (...) {
            result.failed = true;
            result.error = "unknown error";
        }
        if (result.visibilityTracer && result.visibilityTrace) {
            result.visibilityTracer->mark(
                *result.visibilityTrace,
                ChunkVisibilityStage::WorkerFinish);
        }
        m_meshComplete.push(std::move(result));
    };

    if (visibilityTracer && visibilityTrace) {
        visibilityTracer->mark(
            *visibilityTrace,
            ChunkVisibilityStage::PoolSubmit);
    }
    if (m_meshPool && m_meshPool->threadCount() > 0) {
        m_meshPool->enqueue(std::move(job));
    } else {
        job();
    }
}

void ChunkStreamer::ensureThreadPool() {
    size_t desired = 0;
    if (m_config.workerThreads > 0) {
        desired = static_cast<size_t>(m_config.workerThreads);
    }
    if (desired == 0) {
        m_genPool.reset();
        m_meshPool.reset();
        return;
    }
    size_t meshThreads = desired / 2;
    size_t genThreads = desired - meshThreads;
    if (meshThreads == 0) {
        meshThreads = 0;
    }
    if (!m_genPool || m_genPool->threadCount() != genThreads) {
        m_genPool = std::make_unique<detail::ThreadPool>(genThreads);
    }
    if (!m_meshPool || m_meshPool->threadCount() != meshThreads) {
        m_meshPool = std::make_unique<detail::ThreadPool>(meshThreads);
    }
}

bool ChunkStreamer::hasAllNeighborsLoaded(ChunkCoord coord) const {
    if (!m_chunkManager) {
        return false;
    }
    for (int i = 0; i < DirectionCount; ++i) {
        Direction dir = static_cast<Direction>(i);
        int dx = 0;
        int dy = 0;
        int dz = 0;
        directionOffset(dir, dx, dy, dz);
        ChunkCoord neighbor = coord.offset(dx, dy, dz);
        if (m_chunkManager->getChunk(neighbor)) {
            continue;
        }
        if (m_desiredSet.find(neighbor) == m_desiredSet.end()) {
            continue;
        }
        return false;
    }
    return true;
}

} // namespace Rigel::Voxel
