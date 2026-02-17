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
    m_lastCenter.reset();
    m_lastViewDistance = -1;
    m_lastUnloadDistance = -1;
    ensureThreadPool();
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
}

void ChunkStreamer::setBenchmark(ChunkBenchmarkStats* stats) {
    m_benchmark = stats;
}

void ChunkStreamer::setChunkLoader(ChunkLoadCallback loader) {
    m_chunkLoader = std::move(loader);
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

void ChunkStreamer::update(const glm::vec3& cameraPos) {
    if (!m_chunkManager || !m_generator || !m_meshStore) {
        return;
    }

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
        std::vector<std::pair<int, ChunkCoord>> desired;
        desired.reserve(static_cast<size_t>(viewDistance * 2 + 1) *
                        static_cast<size_t>(viewDistance * 2 + 1) *
                        static_cast<size_t>(viewDistance * 2 + 1));

        for (int dz = -viewDistance; dz <= viewDistance; ++dz) {
            for (int dy = -viewDistance; dy <= viewDistance; ++dy) {
                for (int dx = -viewDistance; dx <= viewDistance; ++dx) {
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
        m_desired.reserve(desired.size());
        m_desiredSet.reserve(desired.size());
        for (const auto& entry : desired) {
            m_desired.push_back(entry.second);
            m_desiredSet.insert(entry.second);
        }

        m_lastCenter = center;
        m_lastViewDistance = viewDistance;
        m_lastUnloadDistance = unloadDistance;
        m_dirtyCursor = 0;

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

        if (m_chunkLoadCancel && !m_loadPending.empty()) {
            for (auto it = m_loadPending.begin(); it != m_loadPending.end(); ) {
                if (m_desiredSet.find(*it) == m_desiredSet.end()) {
                    m_chunkLoadCancel(*it);
                    it = m_loadPending.erase(it);
                } else {
                    ++it;
                }
            }
        }
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

    {
        PROFILE_SCOPE("Streaming/Update/LoadGen");
        if (!m_desired.empty()) {
            size_t budget = (m_config.updateBudgetPerFrame <= 0)
                ? m_desired.size()
                : static_cast<size_t>(m_config.updateBudgetPerFrame);
            size_t queued = 0;
            size_t scanned = 0;
            size_t desiredCount = m_desired.size();
            while (queued < budget && scanned < desiredCount) {
                const ChunkCoord& coord = m_desired[scanned];
                ++scanned;

                if (genFull && meshFullMissing) {
                    break;
                }

                ChunkState state = ChunkState::Missing;
                auto stateIt = m_states.find(coord);
                if (stateIt != m_states.end()) {
                    state = stateIt->second;
                }

                Chunk* chunk = m_chunkManager->getChunk(coord);
                bool requested = false;
                if (!chunk && state != ChunkState::QueuedGen && m_chunkLoader) {
                    requested = m_chunkLoader(coord);
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
                        if (!genFull) {
                            enqueueGeneration(coord);
                            genFull = m_inFlightGen >= genLimit;
                            ++queued;
                        }
                        continue;
                    }

                    m_cache.touch(coord);
                    bool hasMesh = m_meshStore && m_meshStore->contains(coord);
                    bool isMeshed = hasMesh || state == ChunkState::ReadyMesh;
                    if (stateIt == m_states.end() || state == ChunkState::QueuedGen) {
                        state = isMeshed ? ChunkState::ReadyMesh : ChunkState::ReadyData;
                        m_states[coord] = state;
                    }

                    if (chunk->isEmpty()) {
                        if (m_meshStore) {
                            m_meshStore->remove(coord);
                        }
                        chunk->clearDirty();
                        m_states[coord] = ChunkState::ReadyMesh;
                        continue;
                    }

                    if (!isMeshed && state != ChunkState::QueuedMesh) {
                        bool allowMissingNeighbors = chunk->loadedFromDisk();
                        if (!meshFullMissing &&
                            (allowMissingNeighbors || hasAllNeighborsLoaded(coord))) {
                            enqueueMesh(coord, *chunk, MeshRequestKind::Missing);
                            meshFullMissing = m_inFlightMeshMissing >= meshLimitMissing;
                            meshFull = m_inFlightMesh >= meshLimit;
                            ++queued;
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
                m_loadPending.erase(coord);

                if (!genFull) {
                    enqueueGeneration(coord);
                    genFull = m_inFlightGen >= genLimit;
                    ++queued;
                }
            }
        }
    }

    {
        PROFILE_SCOPE("Streaming/Update/MeshDirty");
        if (m_dirtyCursor >= m_desired.size()) {
            m_dirtyCursor = 0;
        }
        size_t scanned = 0;
        while (!m_desired.empty() && scanned < m_desired.size()) {
            if (meshFull || meshFullDirty) {
                break;
            }
            const ChunkCoord& coord = m_desired[m_dirtyCursor];
            ++scanned;
            ++m_dirtyCursor;
            if (m_dirtyCursor >= m_desired.size()) {
                m_dirtyCursor = 0;
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
        }
    }

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

    size_t loadBudget = (m_config.loadApplyBudgetPerFrame <= 0)
        ? std::numeric_limits<size_t>::max()
        : static_cast<size_t>(m_config.loadApplyBudgetPerFrame);
    if (m_chunkLoadDrain) {
        PROFILE_SCOPE("Streaming/LoadDrain");
        m_chunkLoadDrain(loadBudget);
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

ChunkStreamer::QueuePressure ChunkStreamer::queuePressure() const {
    QueuePressure pressure;
    pressure.inFlightGeneration = m_inFlightGen;
    pressure.inFlightMeshing = m_inFlightMesh;
    pressure.pendingLoads = m_loadPending.size();

    for (const auto& [coord, state] : m_states) {
        (void)coord;
        switch (state) {
            case ChunkState::QueuedGen:
                ++pressure.queuedGeneration;
                break;
            case ChunkState::QueuedMesh:
                ++pressure.queuedMeshing;
                break;
            case ChunkState::ReadyData:
                ++pressure.readyForMeshing;
                break;
            default:
                break;
        }
    }

    const size_t genLimit = (m_config.genQueueLimit <= 0)
        ? std::numeric_limits<size_t>::max()
        : static_cast<size_t>(m_config.genQueueLimit);
    const size_t meshLimit = (m_config.meshQueueLimit <= 0)
        ? std::numeric_limits<size_t>::max()
        : static_cast<size_t>(m_config.meshQueueLimit);
    const bool genSaturated =
        genLimit != std::numeric_limits<size_t>::max() &&
        pressure.inFlightGeneration >= genLimit;
    const bool meshSaturated =
        meshLimit != std::numeric_limits<size_t>::max() &&
        pressure.inFlightMeshing >= meshLimit;

    pressure.overloaded =
        genSaturated ||
        meshSaturated ||
        pressure.pendingLoads > 0 ||
        pressure.queuedGeneration > 0 ||
        pressure.readyForMeshing > 0 ||
        pressure.queuedMeshing > 0;

    return pressure;
}

void ChunkStreamer::reset() {
    m_states.clear();
    m_inFlightGen = 0;
    m_inFlightMesh = 0;
    m_inFlightMeshMissing = 0;
    m_inFlightMeshDirty = 0;
    m_meshInFlight.clear();
    m_cache = ChunkCache();
    m_cache.setMaxChunks(m_config.maxResidentChunks);
    m_desired.clear();
    m_desiredSet.clear();
    if (m_chunkLoadCancel) {
        for (const auto& coord : m_loadPending) {
            m_chunkLoadCancel(coord);
        }
    }
    m_loadPending.clear();
    m_lastCenter.reset();
    m_lastViewDistance = -1;
    m_lastUnloadDistance = -1;
    m_dirtyCursor = 0;
    for (auto& entry : m_genCancel) {
        entry.second->store(true, std::memory_order_relaxed);
    }
    m_genCancel.clear();

    GenResult genResult;
    while (m_genComplete.tryPop(genResult)) {
    }
    MeshResult meshResult;
    while (m_meshComplete.tryPop(meshResult)) {
    }
}

void ChunkStreamer::applyGenCompletions(size_t budget) {
    size_t applied = 0;
    GenResult genResult;
    while (applied < budget && m_genComplete.tryPop(genResult)) {
        if (m_inFlightGen > 0) {
            --m_inFlightGen;
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
        for (int i = 0; i < DirectionCount; ++i) {
            Direction dir = static_cast<Direction>(i);
            int dx = 0;
            int dy = 0;
            int dz = 0;
            directionOffset(dir, dx, dy, dz);
            ChunkCoord neighborCoord = genResult.coord.offset(dx, dy, dz);
            Chunk* neighbor = m_chunkManager->getChunk(neighborCoord);
            if (neighbor) {
                neighbor->markDirty();
            }
        }
        ++applied;
    }
}

void ChunkStreamer::applyMeshCompletions(size_t budget) {
    size_t applied = 0;
    MeshResult meshResult;
    while (applied < budget && m_meshComplete.tryPop(meshResult)) {
        if (m_inFlightMesh > 0) {
            --m_inFlightMesh;
        }
        auto kindIt = m_meshInFlight.find(meshResult.coord);
        if (kindIt != m_meshInFlight.end()) {
            if (kindIt->second == MeshRequestKind::Missing) {
                if (m_inFlightMeshMissing > 0) {
                    --m_inFlightMeshMissing;
                }
            } else {
                if (m_inFlightMeshDirty > 0) {
                    --m_inFlightMeshDirty;
                }
            }
            m_meshInFlight.erase(kindIt);
        }

        auto stateIt = m_states.find(meshResult.coord);
        if (stateIt == m_states.end() || stateIt->second != ChunkState::QueuedMesh) {
            continue;
        }

        Chunk* chunk = m_chunkManager->getChunk(meshResult.coord);
        if (!chunk) {
            m_states.erase(meshResult.coord);
            continue;
        }

        if (chunk->meshRevision() != meshResult.revision) {
            stateIt->second = ChunkState::ReadyData;
            continue;
        }

        bool needsRemesh = chunk->isDirty();

        if (meshResult.empty) {
            if (m_meshStore) {
                m_meshStore->remove(meshResult.coord);
            }
        } else if (m_meshStore) {
            m_meshStore->set(meshResult.coord, std::move(meshResult.mesh));
        }
        if (needsRemesh) {
            stateIt->second = ChunkState::ReadyData;
        } else {
            chunk->clearDirty();
            stateIt->second = ChunkState::ReadyMesh;
        }

        if (m_benchmark) {
            m_benchmark->addMesh(meshResult.seconds, meshResult.empty);
        }
        ++applied;
    }
}

void ChunkStreamer::enqueueGeneration(ChunkCoord coord) {
    if (m_config.genQueueLimit > 0 &&
        m_inFlightGen >= m_config.genQueueLimit) {
        return;
    }

    m_states[coord] = ChunkState::QueuedGen;
    ++m_inFlightGen;

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
    m_meshInFlight[coord] = kind;
    if (kind == MeshRequestKind::Missing) {
        ++m_inFlightMeshMissing;
    } else {
        ++m_inFlightMeshDirty;
    }

    BlockRegistry* registry = m_registry;
    TextureAtlas* atlas = m_atlas;
    auto job = [this, task = std::move(task), registry, atlas]() mutable {
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
        ChunkMesh mesh = builder.build(ctx);
        auto end = std::chrono::steady_clock::now();

        MeshResult result;
        result.coord = task.coord;
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
