#include "Rigel/Voxel/ChunkStreamer.h"
#include "Rigel/Voxel/MeshBuilder.h"
#include "Rigel/Core/Profiler.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <limits>
#include <unordered_set>

namespace Rigel::Voxel {

namespace {
int distanceSquared(const ChunkCoord& a, const ChunkCoord& b) {
    int dx = a.x - b.x;
    int dy = a.y - b.y;
    int dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}
} // namespace

ChunkStreamer::~ChunkStreamer() {
    if (m_genPool) {
        m_genPool->stop();
        m_genPool.reset();
    }
    if (m_meshPool) {
        m_meshPool->stop();
        m_meshPool.reset();
    }
}

void ChunkStreamer::setConfig(const WorldGenConfig::StreamConfig& config) {
    m_config = config;
    m_cache.setMaxChunks(m_config.maxResidentChunks);
    m_desired.clear();
    m_desiredSet.clear();
    m_desiredPriority.clear();
    m_dirtyMeshQueue = {};
    m_dirtyMeshQueued.clear();
    m_loadGenQueue.clear();
    m_loadGenQueued.clear();
    m_generationCapacityWait.clear();
    m_generationCapacityWaiting.clear();
    m_missingMeshCapacityWait.clear();
    m_missingMeshCapacityWaiting.clear();
    m_lastCenter.reset();
    m_lastViewDistance = -1;
    m_lastUnloadDistance = -1;
    m_initialStreamingBegun = false;
    m_workObservedThisUpdate = false;
    m_workStartedThisUpdate = false;
    ensureThreadPool();
    refreshDiagnostics(false);
}

void ChunkStreamer::bind(ChunkManager* manager,
                         WorldMeshStore* meshStore,
                         BlockRegistry* registry,
                         TextureAtlas* atlas,
                         std::shared_ptr<WorldGenerator> generator) {
    m_chunkManager = manager;
    m_meshStore = meshStore;
    m_registry = registry;
    m_atlas = atlas;
    m_generator = std::move(generator);
    m_lastWorldGenVersion = m_generator ? m_generator->config().world.version : 0;
    m_dirtyMeshQueue = {};
    m_dirtyMeshQueued.clear();
    for (const ChunkCoord& coord : m_desired) {
        queueLoadGen(coord);
    }
}

void ChunkStreamer::setBenchmark(ChunkBenchmarkStats* stats) {
    m_benchmark = stats;
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

void ChunkStreamer::markSpawnDiscoveryComplete() {
    m_spawnDiscoveryComplete = true;
    refreshDiagnostics(false);
}

void ChunkStreamer::update(const glm::vec3& cameraPos) {
    m_workMetrics.lastUpdateDesiredBuildCoordinatesInspected = 0;
    m_workMetrics.lastUpdateSchedulerCoordinatesInspected = 0;
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

    ChunkCoord center = cameraToChunk(cameraPos);
    int viewDistance = std::max(0, m_config.viewDistanceChunks);
    int unloadDistance = std::max(viewDistance, m_config.unloadDistanceChunks);
    int viewRadiusSq = viewDistance * viewDistance;
    int unloadRadiusSq = unloadDistance * unloadDistance;

    bool rebuildDesired = !m_lastCenter ||
        *m_lastCenter != center ||
        m_lastViewDistance != viewDistance ||
        m_lastUnloadDistance != unloadDistance;

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
        reprioritizeDirtyMeshes();
        for (const ChunkCoord& coord : m_desired) {
            if (previouslyQueued.find(coord) != previouslyQueued.end() ||
                previousDesired.find(coord) == previousDesired.end()) {
                queueLoadGen(coord);
            }
        }
        for (const ChunkCoord& coord : previousDesired) {
            if (m_desiredSet.find(coord) == m_desiredSet.end()) {
                m_generationCapacityWaiting.erase(coord);
                m_missingMeshCapacityWaiting.erase(coord);
                queueLoadedNeighbors(coord);
            }
        }

        m_lastCenter = center;
        m_lastViewDistance = viewDistance;
        m_lastUnloadDistance = unloadDistance;

        for (auto it = m_states.begin(); it != m_states.end(); ) {
            if ((it->second == ChunkState::QueuedGen || it->second == ChunkState::QueuedMesh) &&
                m_desiredSet.find(it->first) == m_desiredSet.end()) {
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
                if (m_desiredSet.find(*it) == m_desiredSet.end()) {
                    if (m_chunkLoadCancel) {
                        m_chunkLoadCancel(*it);
                    }
                    it = m_loadPending.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }

    const uint32_t worldGenVersion = m_generator->config().world.version;
    bool worldGenChanged = m_lastWorldGenVersion != worldGenVersion;
    if (worldGenChanged) {
        for (const ChunkCoord& coord : m_desired) {
            queueLoadGen(coord);
        }
    }

    if (!m_chunkLoadDrain && !m_loadPending.empty()) {
        for (auto it = m_loadPending.begin(); it != m_loadPending.end(); ) {
            ++schedulerCoordinatesInspected;
            bool resolved = m_chunkManager->getChunk(*it) != nullptr;
            if (!resolved && m_chunkPending) {
                resolved = !m_chunkPending(*it);
            } else if (!resolved) {
                queueLoadGen(*it);
                ++it;
                continue;
            }
            if (resolved) {
                ChunkCoord coord = *it;
                it = m_loadPending.erase(it);
                queueLoadGen(coord);
            } else {
                ++it;
            }
        }
    }

    for (const ChunkCoord& coord : m_chunkManager->consumeDirtyMeshNotifications()) {
        queueDirtyMesh(coord);
    }

    size_t genLimit = (m_config.genQueueLimit <= 0)
        ? std::numeric_limits<size_t>::max()
        : static_cast<size_t>(m_config.genQueueLimit);
    size_t meshLimit = (m_config.meshQueueLimit <= 0)
        ? std::numeric_limits<size_t>::max()
        : static_cast<size_t>(m_config.meshQueueLimit);
    size_t meshLimitMissing = meshLimit;
    size_t meshLimitDirty = meshLimit;
    if (meshLimit != std::numeric_limits<size_t>::max()) {
        size_t reserve = meshLimit / 4;
        if (meshLimit > 1 && reserve == 0) {
            reserve = 1;
        }
        if (reserve >= meshLimit) {
            reserve = meshLimit - 1;
        }
        meshLimitMissing = meshLimit - reserve;
        meshLimitDirty = reserve;
    }

    bool genFull = m_inFlightGen >= genLimit;
    bool meshFull = m_inFlightMesh >= meshLimit;
    bool meshFullMissing = m_inFlightMeshMissing >= meshLimitMissing;
    bool meshFullDirty = m_inFlightMeshDirty >= meshLimitDirty;

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
            bool requested = false;
            if (!chunk && state != ChunkState::QueuedGen && m_chunkLoader) {
                bool wasPending = m_loadPending.find(coord) != m_loadPending.end();
                requested = m_chunkLoader(coord);
                if (requested && !wasPending) {
                    ++m_workMetrics.chunkLoadRequestsStarted;
                }
                chunk = m_chunkManager->getChunk(coord);
            }

            if (chunk) {
                m_loadPending.erase(coord);
                if (m_generator &&
                    chunk->worldGenVersion() != m_generator->config().world.version) {
                    if (m_meshStore) {
                        m_meshStore->remove(coord);
                    }
                    m_chunkManager->unloadChunk(coord);
                    m_states.erase(coord);
                    m_countedMeshRetryRevisions.erase(coord);
                    if (!genFull) {
                        enqueueGeneration(coord);
                        genFull = m_inFlightGen >= genLimit;
                        ++queued;
                    } else {
                        waitForGenerationCapacity(coord);
                    }
                    continue;
                }

                m_cache.touch(coord);
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
                    m_states[coord] = ChunkState::ReadyMesh;
                    m_countedMeshRetryRevisions.erase(coord);
                    continue;
                }

                if (isMeshed && chunk->isDirty()) {
                    queueDirtyMesh(coord);
                }

                if (!isMeshed && state != ChunkState::QueuedMesh) {
                    if (!meshFull && !meshFullMissing && hasAllNeighborsLoaded(coord)) {
                        enqueueMesh(coord, *chunk, MeshRequestKind::Missing);
                        meshFullMissing = m_inFlightMeshMissing >= meshLimitMissing;
                        meshFull = m_inFlightMesh >= meshLimit;
                        ++queued;
                    } else if (meshFull || meshFullMissing) {
                        waitForMissingMeshCapacity(coord);
                    }
                }
                continue;
            }

            if (state == ChunkState::QueuedGen) {
                continue;
            }

            if (requested) {
                m_loadPending.insert(coord);
                ++queued;
                continue;
            }
            if (m_chunkPending && m_chunkPending(coord)) {
                m_loadPending.insert(coord);
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

    if (!m_dirtyMeshQueue.empty()) {
        PROFILE_SCOPE("Streaming/Update/MeshDirty");
        while (!m_dirtyMeshQueue.empty()) {
            if (meshFull || meshFullDirty) {
                break;
            }

            ChunkCoord coord = m_dirtyMeshQueue.top().coord;
            m_dirtyMeshQueue.pop();
            if (m_dirtyMeshQueued.erase(coord) == 0) {
                continue;
            }
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
            if (!chunk || chunk->isEmpty()) {
                continue;
            }

            if (state == ChunkState::QueuedMesh && chunk->isDirty()) {
                auto flightIt = m_meshInFlight.find(coord);
                if (flightIt != m_meshInFlight.end() &&
                    flightIt->second.observedRevision != chunk->meshRevision()) {
                    flightIt->second.observedRevision = chunk->meshRevision();
                    ++m_workMetrics.meshInvalidations;
                    ++m_workMetrics.meshRequestsCoalesced;
                }
                continue;
            }

            bool hasMesh = m_meshStore && m_meshStore->contains(coord);
            bool isMeshed = hasMesh || state == ChunkState::ReadyMesh;
            if (!isMeshed || !chunk->isDirty() || state == ChunkState::QueuedMesh) {
                continue;
            }

            if (!hasAllNeighborsLoaded(coord)) {
                continue;
            }
            enqueueMesh(coord, *chunk, MeshRequestKind::Dirty);
            meshFullDirty = m_inFlightMeshDirty >= meshLimitDirty;
            meshFull = m_inFlightMesh >= meshLimit;
        }
    }

    if (rebuildDesired) {
        PROFILE_SCOPE("Streaming/Update/Evict");
        std::vector<ChunkCoord> toEvict;
        m_chunkManager->forEachChunk([&](ChunkCoord coord, const Chunk&) {
            int distSq = distanceSquared(center, coord);
            if (distSq > unloadRadiusSq) {
                toEvict.push_back(coord);
            }
        });

        for (const ChunkCoord& coord : toEvict) {
            if (m_meshStore) {
                m_meshStore->remove(coord);
            }
            if (Chunk* chunk = m_chunkManager->getChunk(coord)) {
                chunk->setLoadedFromDisk(false);
            }
            m_chunkManager->unloadChunk(coord);
            m_cache.erase(coord);
            m_states.erase(coord);
            m_countedMeshRetryRevisions.erase(coord);
        }

    }

    {
        PROFILE_SCOPE("Streaming/Update/CacheEvict");
        for (const ChunkCoord& coord : m_cache.evict(m_desiredSet)) {
            if (m_meshStore) {
                m_meshStore->remove(coord);
            }
            if (Chunk* chunk = m_chunkManager->getChunk(coord)) {
                chunk->setLoadedFromDisk(false);
            }
            m_chunkManager->unloadChunk(coord);
            m_states.erase(coord);
            m_countedMeshRetryRevisions.erase(coord);
        }
    }

    m_workMetrics.lastUpdateDesiredBuildCoordinatesInspected =
        desiredBuildCoordinatesInspected;
    m_workMetrics.lastUpdateSchedulerCoordinatesInspected =
        schedulerCoordinatesInspected;
    m_workMetrics.desiredBuildCoordinatesInspected +=
        desiredBuildCoordinatesInspected;
    m_workMetrics.schedulerCoordinatesInspected += schedulerCoordinatesInspected;
    m_lastWorldGenVersion = worldGenVersion;

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
        for (const ChunkCoord& coord : m_chunkLoadDrain(loadBudget)) {
            m_loadPending.erase(coord);
            queueLoadGen(coord);
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
    m_states.clear();
    m_inFlightGen = 0;
    for (auto& entry : m_meshInFlight) {
        entry.second.obsolete = true;
    }
    m_countedMeshRetryRevisions.clear();
    m_cache = ChunkCache();
    m_cache.setMaxChunks(m_config.maxResidentChunks);
    m_desired.clear();
    m_desiredSet.clear();
    m_desiredPriority.clear();
    m_dirtyMeshQueue = {};
    m_dirtyMeshQueued.clear();
    m_loadGenQueue.clear();
    m_loadGenQueued.clear();
    m_generationCapacityWait.clear();
    m_generationCapacityWaiting.clear();
    m_missingMeshCapacityWait.clear();
    m_missingMeshCapacityWaiting.clear();
    if (m_chunkLoadCancel) {
        for (const auto& coord : m_loadPending) {
            m_chunkLoadCancel(coord);
        }
    }
    m_loadPending.clear();
    m_lastCenter.reset();
    m_lastViewDistance = -1;
    m_lastUnloadDistance = -1;
    m_lastWorldGenVersion = m_generator ? m_generator->config().world.version : 0;
    m_initialStreamingBegun = false;
    m_workObservedThisUpdate = false;
    m_workStartedThisUpdate = false;
    m_streamingUpdateSequence = 0;
    m_lifecycleUpdateSequence = 0;
    for (auto& entry : m_genCancel) {
        entry.second->store(true, std::memory_order_relaxed);
    }
    m_genCancel.clear();

    GenResult genResult;
    while (m_genComplete.tryPop(genResult)) {
    }
    applyMeshCompletions(std::numeric_limits<size_t>::max());
    refreshDiagnostics(false);
}

StreamingDiagnosticSnapshot ChunkStreamer::collectDiagnostics() const {
    StreamingDiagnosticSnapshot snapshot;
    size_t generationPending = m_generationCapacityWaiting.size();
    size_t meshPending = m_missingMeshCapacityWaiting.size();
    for (const ChunkCoord& coord : m_dirtyMeshQueued) {
        if (m_missingMeshCapacityWaiting.find(coord) ==
                m_missingMeshCapacityWaiting.end() &&
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
        .started = m_workMetrics.generationJobsStarted
    };
    snapshot.mesh = StreamingWorkCount{
        .pending = meshPending,
        .inFlight = m_inFlightMesh,
        .started = m_workMetrics.meshJobsStarted
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

        Chunk& chunk = m_chunkManager->getOrCreateChunk(genResult.coord);
        if (m_registry) {
            chunk.copyFrom(genResult.blocks, *m_registry);
        } else {
            chunk.copyFrom(genResult.blocks);
        }
        chunk.clearPersistDirty();
        chunk.setLoadedFromDisk(false);
        chunk.setWorldGenVersion(genResult.worldGenVersion);

        if (m_benchmark) {
            m_benchmark->addGeneration(genResult.seconds);
        }

        if (chunk.isEmpty()) {
            if (m_meshStore) {
                m_meshStore->remove(genResult.coord);
            }
            chunk.clearDirty();
            stateIt->second = ChunkState::ReadyMesh;
        } else {
            stateIt->second = ChunkState::ReadyData;
        }
        queueLoadGen(genResult.coord);
        queueLoadedNeighbors(genResult.coord);
        for (int i = 0; i < DirectionCount; ++i) {
            Direction dir = static_cast<Direction>(i);
            int dx = 0;
            int dy = 0;
            int dz = 0;
            directionOffset(dir, dx, dy, dz);
            ChunkCoord neighborCoord = genResult.coord.offset(dx, dy, dz);
            Chunk* neighbor = m_chunkManager->getChunk(neighborCoord);
            if (neighbor) {
                neighbor->invalidateMesh();
            }
        }
        ++applied;
    }
}

void ChunkStreamer::applyMeshCompletions(size_t budget) {
    size_t applied = 0;
    MeshResult meshResult;
    while (applied < budget && m_meshComplete.tryPop(meshResult)) {
        ++m_workMetrics.meshJobsCompleted;
        auto flightIt = m_meshInFlight.find(meshResult.coord);
        if (flightIt == m_meshInFlight.end() ||
            flightIt->second.requestId != meshResult.requestId) {
            ++m_workMetrics.meshJobsRejectedStale;
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

        if (flight.obsolete) {
            ++m_workMetrics.meshJobsRejectedStale;
            queueLoadGen(meshResult.coord);
            continue;
        }

        auto stateIt = m_states.find(meshResult.coord);
        if (stateIt == m_states.end() || stateIt->second != ChunkState::QueuedMesh) {
            ++m_workMetrics.meshJobsRejectedStale;
            queueLoadGen(meshResult.coord);
            continue;
        }

        Chunk* chunk = m_chunkManager->getChunk(meshResult.coord);
        if (!chunk) {
            m_states.erase(meshResult.coord);
            ++m_workMetrics.meshJobsRejectedStale;
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
            queueLoadGen(meshResult.coord);
            queueDirtyMesh(meshResult.coord);
            continue;
        }

        if (meshResult.empty) {
            if (m_meshStore) {
                m_meshStore->remove(meshResult.coord);
            }
        } else if (m_meshStore) {
            m_meshStore->set(meshResult.coord, std::move(meshResult.mesh));
        }
        chunk->clearDirty();
        stateIt->second = ChunkState::ReadyMesh;

        if (m_benchmark) {
            m_benchmark->addMesh(meshResult.seconds, meshResult.empty);
        }
        ++m_workMetrics.meshJobsAccepted;
        ++applied;
    }
}

void ChunkStreamer::queueLoadGen(ChunkCoord coord) {
    if (m_desiredSet.find(coord) == m_desiredSet.end()) {
        return;
    }
    m_generationCapacityWaiting.erase(coord);
    m_missingMeshCapacityWaiting.erase(coord);
    if (m_loadGenQueued.insert(coord).second) {
        m_loadGenQueue.push_back(coord);
    }
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
            queueLoadGen(neighbor);
        }
    }
}

void ChunkStreamer::queueDirtyMesh(ChunkCoord coord) {
    auto priorityIt = m_desiredPriority.find(coord);
    if (priorityIt == m_desiredPriority.end()) {
        return;
    }
    if (m_dirtyMeshQueued.insert(coord).second) {
        m_dirtyMeshQueue.push({priorityIt->second, coord});
    }
}

void ChunkStreamer::reprioritizeDirtyMeshes() {
    decltype(m_dirtyMeshQueue) prioritized;
    std::unordered_set<ChunkCoord, ChunkCoordHash> retained;
    retained.reserve(m_dirtyMeshQueued.size());
    for (const ChunkCoord& coord : m_dirtyMeshQueued) {
        auto priorityIt = m_desiredPriority.find(coord);
        if (priorityIt == m_desiredPriority.end()) {
            continue;
        }
        retained.insert(coord);
        prioritized.push({priorityIt->second, coord});
    }
    m_dirtyMeshQueue = std::move(prioritized);
    m_dirtyMeshQueued = std::move(retained);
}

void ChunkStreamer::enqueueGeneration(ChunkCoord coord) {
    if (m_config.genQueueLimit > 0 &&
        m_inFlightGen >= m_config.genQueueLimit) {
        return;
    }

    m_states[coord] = ChunkState::QueuedGen;
    ++m_inFlightGen;
    ++m_workMetrics.generationJobsStarted;

    auto cancelToken = std::make_shared<std::atomic_bool>(false);
    m_genCancel[coord] = cancelToken;
    auto generator = m_generator;
    auto job = [this, generator, coord, cancelToken]() {
        if (cancelToken->load(std::memory_order_relaxed)) {
            GenResult result;
            result.coord = coord;
            result.cancelled = true;
            result.cancelToken = cancelToken;
            m_genComplete.push(std::move(result));
            return;
        }

        ChunkBuffer buffer;
        auto start = std::chrono::steady_clock::now();
        generator->generate(coord, buffer, cancelToken.get());
        auto end = std::chrono::steady_clock::now();

        GenResult result;
        result.coord = coord;
        result.blocks = buffer.blocks;
        result.worldGenVersion = generator ? generator->config().world.version : 0;
        result.seconds = std::chrono::duration<double>(end - start).count();
        result.cancelled = cancelToken->load(std::memory_order_relaxed);
        result.cancelToken = cancelToken;
        m_genComplete.push(std::move(result));
    };

    if (m_genPool && m_genPool->threadCount() > 0) {
        m_genPool->enqueue(std::move(job));
    } else {
        job();
    }
}

void ChunkStreamer::enqueueMesh(ChunkCoord coord, Chunk& chunk, MeshRequestKind kind) {
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

    chunk.clearDirty();

    MeshTask task;
    task.coord = coord;
    task.requestId = m_nextMeshRequestId++;
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

    m_states[coord] = ChunkState::QueuedMesh;
    ++m_inFlightMesh;
    m_meshInFlight[coord] = MeshInFlight{
        .kind = kind,
        .requestId = task.requestId,
        .observedRevision = task.revision
    };
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

    BlockRegistry* registry = m_registry;
    TextureAtlas* atlas = m_atlas;
    auto meshBuildStartCallback = m_meshBuildStartCallback;
    auto job = [this,
                task = std::move(task),
                registry,
                atlas,
                meshBuildStartCallback = std::move(meshBuildStartCallback)]() mutable {
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

        if (meshBuildStartCallback) {
            meshBuildStartCallback();
        }
        auto start = std::chrono::steady_clock::now();
        ChunkMesh mesh = builder.build(ctx);
        auto end = std::chrono::steady_clock::now();

        MeshResult result;
        result.coord = task.coord;
        result.requestId = task.requestId;
        result.chunkInstanceId = task.chunkInstanceId;
        result.revision = task.revision;
        result.mesh = std::move(mesh);
        result.seconds = std::chrono::duration<double>(end - start).count();
        result.empty = result.mesh.isEmpty();
        m_meshComplete.push(std::move(result));
    };

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
