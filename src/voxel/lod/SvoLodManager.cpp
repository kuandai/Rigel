#include "Rigel/Voxel/Lod/SvoLodManager.h"

#include <GL/glew.h>
#include <glm/geometric.hpp>

#include <algorithm>
#include <chrono>
#include <limits>
#include <tuple>
#include <vector>

namespace Rigel::Voxel {

namespace {

int ceilPow2(int value) {
    int v = std::max(1, value);
    int p = 1;
    while (p < v) {
        p <<= 1;
    }
    return p;
}

float distanceSqToAabb(const glm::vec3& point,
                       const glm::vec3& aabbMin,
                       const glm::vec3& aabbMax) {
    const float dx = (point.x < aabbMin.x)
        ? (aabbMin.x - point.x)
        : ((point.x > aabbMax.x) ? (point.x - aabbMax.x) : 0.0f);
    const float dy = (point.y < aabbMin.y)
        ? (aabbMin.y - point.y)
        : ((point.y > aabbMax.y) ? (point.y - aabbMax.y) : 0.0f);
    const float dz = (point.z < aabbMin.z)
        ? (aabbMin.z - point.z)
        : ((point.z > aabbMax.z) ? (point.z - aabbMax.z) : 0.0f);
    return dx * dx + dy * dy + dz * dz;
}

glm::vec3 cellWorldCenter(const LodCellKey& key,
                          int spanChunks,
                          int rootSizeChunks) {
    constexpr float kChunkWorld = static_cast<float>(Chunk::SIZE);
    const float baseX = static_cast<float>(key.x * spanChunks) * kChunkWorld;
    const float baseY = static_cast<float>(key.y * spanChunks) * kChunkWorld;
    const float baseZ = static_cast<float>(key.z * spanChunks) * kChunkWorld;
    const float half = static_cast<float>(rootSizeChunks) * kChunkWorld * 0.5f;
    return glm::vec3(baseX + half, baseY + half, baseZ + half);
}

void collectOpaqueLeavesRecursive(const std::vector<LodSvoNode>& nodes,
                                  uint32_t nodeIndex,
                                  int baseChunkX,
                                  int baseChunkY,
                                  int baseChunkZ,
                                  int localX,
                                  int localY,
                                  int localZ,
                                  int nodeSizeChunks,
                                  std::vector<SvoLodManager::OpaqueDrawInstance>& out) {
    if (nodeIndex == LodSvoNode::INVALID_INDEX || nodeIndex >= nodes.size()) {
        return;
    }

    const LodSvoNode& node = nodes[nodeIndex];
    if (node.kind == LodNodeKind::Empty) {
        return;
    }

    if (node.childMask == 0) {
        if (node.materialClass == LodMaterialClass::Opaque) {
            constexpr float kChunkWorld = static_cast<float>(Chunk::SIZE);
            SvoLodManager::OpaqueDrawInstance instance;
            instance.worldMin = glm::vec3(
                static_cast<float>(baseChunkX + localX) * kChunkWorld,
                static_cast<float>(baseChunkY + localY) * kChunkWorld,
                static_cast<float>(baseChunkZ + localZ) * kChunkWorld
            );
            instance.worldSize = static_cast<float>(nodeSizeChunks) * kChunkWorld;
            out.push_back(instance);
        }
        return;
    }

    if (nodeSizeChunks <= 1) {
        return;
    }

    const int half = nodeSizeChunks / 2;
    for (int child = 0; child < 8; ++child) {
        if ((node.childMask & static_cast<uint8_t>(1u << child)) == 0) {
            continue;
        }

        const int ox = (child & 1) ? half : 0;
        const int oy = (child & 2) ? half : 0;
        const int oz = (child & 4) ? half : 0;
        collectOpaqueLeavesRecursive(nodes,
                                     node.children[child],
                                     baseChunkX,
                                     baseChunkY,
                                     baseChunkZ,
                                     localX + ox,
                                     localY + oy,
                                     localZ + oz,
                                     half,
                                     out);
    }
}

} // namespace

SvoLodConfig SvoLodManager::sanitizeConfig(SvoLodConfig config) {
    if (config.nearMeshRadiusChunks < 0) {
        config.nearMeshRadiusChunks = 0;
    }
    if (config.lodStartRadiusChunks < config.nearMeshRadiusChunks) {
        config.lodStartRadiusChunks = config.nearMeshRadiusChunks;
    }
    if (config.lodCellSpanChunks < 1) {
        config.lodCellSpanChunks = 1;
    }
    if (config.lodMaxCells < 0) {
        config.lodMaxCells = 0;
    }
    if (config.lodMaxCpuBytes < 0) {
        config.lodMaxCpuBytes = 0;
    }
    if (config.lodMaxGpuBytes < 0) {
        config.lodMaxGpuBytes = 0;
    }
    if (config.lodCopyBudgetPerFrame < 0) {
        config.lodCopyBudgetPerFrame = 0;
    }
    if (config.lodApplyBudgetPerFrame < 0) {
        config.lodApplyBudgetPerFrame = 0;
    }
    return config;
}

void SvoLodManager::setConfig(const SvoLodConfig& config) {
    m_config = sanitizeConfig(config);
    enforceCellLimit();
}

void SvoLodManager::setBuildThreads(size_t threadCount) {
    m_buildThreads = threadCount;
    if (m_buildPool) {
        m_buildPool->stop();
        m_buildPool.reset();
    }
}

void SvoLodManager::bind(const ChunkManager* chunkManager, const BlockRegistry* registry) {
    m_chunkManager = chunkManager;
    m_registry = registry;
}

void SvoLodManager::initialize() {
    ensureBuildPool();
}

void SvoLodManager::update(const glm::vec3& cameraPos) {
    m_lastCameraPos = cameraPos;
    if (!m_config.enabled) {
        m_telemetry.scanMicros = 0;
        m_telemetry.copyMicros = 0;
        m_telemetry.applyMicros = 0;
        updateTelemetry();
        return;
    }

    ++m_telemetry.updateCalls;
    ++m_frameCounter;
    ensureBuildPool();
    const auto scanStart = std::chrono::steady_clock::now();
    scanChunkChanges();
    const auto scanEnd = std::chrono::steady_clock::now();
    processCopyBudget();
    const auto copyEnd = std::chrono::steady_clock::now();
    processApplyBudget();
    const auto applyEnd = std::chrono::steady_clock::now();
    enforceCellLimit();
    m_telemetry.scanMicros = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(scanEnd - scanStart).count());
    m_telemetry.copyMicros = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(copyEnd - scanEnd).count());
    m_telemetry.applyMicros = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(applyEnd - copyEnd).count());

    updateTelemetry();
}

void SvoLodManager::uploadRenderResources() {
    if (!m_config.enabled) {
        m_telemetry.uploadMicros = 0;
        updateTelemetry();
        return;
    }

    const auto start = std::chrono::steady_clock::now();
    processUploadBudget();
    const auto end = std::chrono::steady_clock::now();
    m_telemetry.uploadMicros = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
    updateTelemetry();
}

void SvoLodManager::reset() {
    releaseRenderResources();

    if (m_buildPool) {
        m_buildPool->stop();
        m_buildPool.reset();
    }

    m_knownChunkRevisions.clear();
    m_cells.clear();
    m_gpuCells.clear();
    m_dirtyQueue.clear();
    m_dirtyQueued.clear();
    m_uploadQueue.clear();
    m_uploadQueued.clear();
    m_frameCounter = 0;
    m_lastCameraPos = glm::vec3(0.0f);

    LodBuildOutput output;
    while (m_buildComplete.tryPop(output)) {
    }

    m_telemetry = {};
}

void SvoLodManager::releaseRenderResources() {
    for (auto& [key, gpu] : m_gpuCells) {
        (void)key;
        if (gpu.nodeBuffer != 0) {
            GLuint buffer = static_cast<GLuint>(gpu.nodeBuffer);
            glDeleteBuffers(1, &buffer);
            gpu.nodeBuffer = 0;
        }
    }
    m_gpuCells.clear();
    m_uploadQueue.clear();
    m_uploadQueued.clear();
}

std::optional<SvoLodManager::CellInfo> SvoLodManager::cellInfo(const LodCellKey& key) const {
    auto it = m_cells.find(key);
    if (it == m_cells.end()) {
        return std::nullopt;
    }
    CellInfo info;
    info.state = it->second.state;
    info.desiredRevision = it->second.desiredRevision;
    info.queuedRevision = it->second.queuedRevision;
    info.appliedRevision = it->second.appliedRevision;
    info.visibleAsFarLod = it->second.visibleAsFarLod;
    info.sampledChunks = it->second.sampledChunks;
    info.nodeCount = it->second.nodeCount;
    info.leafCount = it->second.leafCount;
    info.mixedNodeCount = it->second.mixedNodeCount;
    return info;
}

void SvoLodManager::collectDebugCells(std::vector<DebugCellState>& out) const {
    out.clear();
    if (!m_config.enabled) {
        return;
    }

    out.reserve(m_cells.size());
    const int spanChunks = std::max(1, m_config.lodCellSpanChunks);
    for (const auto& [key, cell] : m_cells) {
        if (cell.state == LodCellState::Missing) {
            continue;
        }
        out.push_back(DebugCellState{
            .key = key,
            .state = cell.state,
            .spanChunks = spanChunks,
            .visibleAsFarLod = cell.visibleAsFarLod
        });
    }
}

void SvoLodManager::collectOpaqueDrawInstances(std::vector<OpaqueDrawInstance>& out,
                                               const glm::vec3& cameraPos,
                                               float renderDistanceWorld) {
    out.clear();
    if (!m_config.enabled) {
        return;
    }

    const LodDistanceBands bands = makeLodDistanceBands(m_config, renderDistanceWorld);
    const int span = std::max(1, m_config.lodCellSpanChunks);
    const int rootSize = ceilPow2(span);
    constexpr float kChunkWorld = static_cast<float>(Chunk::SIZE);
    const float rootWorldSize = static_cast<float>(rootSize) * kChunkWorld;

    for (auto& [key, cell] : m_cells) {
        if (cell.state != LodCellState::Ready ||
            cell.nodes.empty() ||
            cell.rootNode == LodSvoNode::INVALID_INDEX) {
            cell.visibleAsFarLod = false;
            continue;
        }

        const glm::vec3 cellMin(
            static_cast<float>(key.x * span) * kChunkWorld,
            static_cast<float>(key.y * span) * kChunkWorld,
            static_cast<float>(key.z * span) * kChunkWorld
        );
        const glm::vec3 cellMax = cellMin + glm::vec3(rootWorldSize);
        const float distanceSq = distanceSqToAabb(cameraPos, cellMin, cellMax);
        cell.visibleAsFarLod = shouldRenderFarLod(distanceSq, cell.visibleAsFarLod, bands);
        if (!cell.visibleAsFarLod) {
            continue;
        }

        const int baseChunkX = key.x * span;
        const int baseChunkY = key.y * span;
        const int baseChunkZ = key.z * span;

        collectOpaqueLeavesRecursive(cell.nodes,
                                     cell.rootNode,
                                     baseChunkX,
                                     baseChunkY,
                                     baseChunkZ,
                                     0,
                                     0,
                                     0,
                                     rootSize,
                                     out);
    }
}

void SvoLodManager::ensureBuildPool() {
    if (m_buildThreads == 0) {
        if (m_buildPool) {
            m_buildPool->stop();
            m_buildPool.reset();
        }
        return;
    }

    if (!m_buildPool || m_buildPool->threadCount() != m_buildThreads) {
        if (m_buildPool) {
            m_buildPool->stop();
        }
        m_buildPool = std::make_unique<detail::ThreadPool>(m_buildThreads);
    }
}

void SvoLodManager::scanChunkChanges() {
    if (!m_chunkManager) {
        return;
    }

    std::unordered_set<ChunkCoord, ChunkCoordHash> seen;
    seen.reserve(m_knownChunkRevisions.size() + 64);

    m_chunkManager->forEachChunk([&](ChunkCoord coord, const Chunk& chunk) {
        seen.insert(coord);
        uint32_t revision = chunk.meshRevision();
        auto it = m_knownChunkRevisions.find(coord);
        if (it == m_knownChunkRevisions.end() || it->second != revision) {
            enqueueDirtyChunk(coord);
            m_knownChunkRevisions[coord] = revision;
        }
    });

    for (auto it = m_knownChunkRevisions.begin(); it != m_knownChunkRevisions.end();) {
        if (seen.find(it->first) != seen.end()) {
            ++it;
            continue;
        }
        enqueueDirtyChunk(it->first);
        it = m_knownChunkRevisions.erase(it);
    }
}

void SvoLodManager::processCopyBudget() {
    size_t budget = static_cast<size_t>(std::max(0, m_config.lodCopyBudgetPerFrame));

    while (budget > 0 && !m_dirtyQueue.empty()) {
        LodCellKey key = m_dirtyQueue.front();
        m_dirtyQueue.pop_front();
        m_dirtyQueued.erase(key);

        auto it = m_cells.find(key);
        if (it == m_cells.end()) {
            continue;
        }
        CellRecord& cell = it->second;
        cell.lastTouchedFrame = m_frameCounter;

        const bool inFlight = (cell.queuedRevision != 0) &&
            (cell.queuedRevision > cell.appliedRevision);
        if (inFlight) {
            if (cell.desiredRevision > cell.queuedRevision) {
                requeueDirtyCell(key);
            }
            --budget;
            continue;
        }

        if (cell.desiredRevision <= cell.appliedRevision) {
            continue;
        }

        std::optional<LodBuildInput> input = makeBuildInput(key, cell.desiredRevision);
        if (!input || input->chunks.empty()) {
            cell.state = LodCellState::Missing;
            cell.sampledChunks = 0;
            cell.nodeCount = 0;
            cell.leafCount = 0;
            cell.mixedNodeCount = 0;
            cell.nonAirVoxelCount = 0;
            cell.opaqueVoxelCount = 0;
            cell.nonOpaqueVoxelCount = 0;
            cell.nodes.clear();
            cell.rootNode = LodSvoNode::INVALID_INDEX;
            cell.appliedRevision = cell.desiredRevision;
            cell.queuedRevision = 0;
            removeUploadCell(key);
            releaseGpuCell(key);
            --budget;
            continue;
        }

        cell.state = LodCellState::QueuedBuild;
        cell.queuedRevision = input->revision;
        ++m_telemetry.copiedCells;

        BlockRegistry const* registry = m_registry;
        if (m_buildPool && m_buildPool->threadCount() > 0) {
            LodBuildInput asyncInput = std::move(*input);
            m_buildPool->enqueue([this, asyncInput = std::move(asyncInput), registry]() {
                m_buildComplete.push(buildLodBuildOutput(asyncInput, registry));
            });
        } else {
            m_buildComplete.push(buildLodBuildOutput(*input, registry));
        }
        cell.state = LodCellState::Building;
        --budget;
    }
}

void SvoLodManager::processApplyBudget() {
    size_t budget = static_cast<size_t>(std::max(0, m_config.lodApplyBudgetPerFrame));

    LodBuildOutput output;
    while (budget > 0 && m_buildComplete.tryPop(output)) {
        auto it = m_cells.find(output.key);
        if (it == m_cells.end()) {
            continue;
        }

        CellRecord& cell = it->second;
        cell.lastTouchedFrame = m_frameCounter;

        if (output.revision != cell.queuedRevision || output.revision < cell.desiredRevision) {
            cell.queuedRevision = 0;
            cell.state = LodCellState::Stale;
            if (cell.desiredRevision > cell.appliedRevision) {
                requeueDirtyCell(output.key);
            }
            continue;
        }

        cell.appliedRevision = output.revision;
        cell.queuedRevision = 0;
        cell.state = LodCellState::Ready;
        cell.sampledChunks = output.sampledChunks;
        cell.nodeCount = output.nodeCount;
        cell.leafCount = output.leafCount;
        cell.mixedNodeCount = output.mixedNodeCount;
        cell.nonAirVoxelCount = output.nonAirVoxelCount;
        cell.opaqueVoxelCount = output.opaqueVoxelCount;
        cell.nonOpaqueVoxelCount = output.nonOpaqueVoxelCount;
        cell.nodes = std::move(output.nodes);
        cell.rootNode = output.rootNode;
        enqueueUploadCell(output.key);
        ++m_telemetry.appliedCells;
        --budget;
    }
}

void SvoLodManager::enqueueDirtyChunk(ChunkCoord coord) {
    for (const LodCellKey& key : touchedLodCellsForChunk(coord, m_config.lodCellSpanChunks)) {
        enqueueDirtyCell(key);
    }
}

void SvoLodManager::enqueueDirtyCell(const LodCellKey& key) {
    CellRecord& cell = m_cells[key];
    uint64_t nextRevision = cell.desiredRevision + 1;
    cell.desiredRevision = (nextRevision == 0) ? 1 : nextRevision;
    if (cell.state == LodCellState::Missing) {
        cell.state = LodCellState::Stale;
    } else if (cell.state == LodCellState::Ready) {
        cell.state = LodCellState::Stale;
    }

    requeueDirtyCell(key);
}

void SvoLodManager::requeueDirtyCell(const LodCellKey& key) {
    if (m_dirtyQueued.insert(key).second) {
        m_dirtyQueue.push_back(key);
    }
}

void SvoLodManager::enqueueUploadCell(const LodCellKey& key) {
    if (m_uploadQueued.insert(key).second) {
        m_uploadQueue.push_back(key);
    }
}

void SvoLodManager::processUploadBudget() {
    size_t budget = static_cast<size_t>(std::max(1, m_config.lodApplyBudgetPerFrame));
    while (budget > 0 && !m_uploadQueue.empty()) {
        LodCellKey key = m_uploadQueue.front();
        m_uploadQueue.pop_front();
        m_uploadQueued.erase(key);

        auto cellIt = m_cells.find(key);
        if (cellIt == m_cells.end()) {
            releaseGpuCell(key);
            --budget;
            continue;
        }

        CellRecord& cell = cellIt->second;
        if (cell.state != LodCellState::Ready || cell.appliedRevision == 0) {
            --budget;
            continue;
        }

        GpuCellRecord& gpu = m_gpuCells[key];
        if (gpu.uploadedRevision == cell.appliedRevision &&
            gpu.nodeCount == cell.nodeCount &&
            gpu.nodeBuffer != 0) {
            --budget;
            continue;
        }

        if (cell.nodes.empty()) {
            releaseGpuCell(key);
            --budget;
            continue;
        }

        if (gpu.nodeBuffer == 0) {
            GLuint buffer = 0;
            glGenBuffers(1, &buffer);
            gpu.nodeBuffer = static_cast<unsigned int>(buffer);
        }

        const size_t byteSize = cell.nodes.size() * sizeof(LodSvoNode);
        GLuint nodeBuffer = static_cast<GLuint>(gpu.nodeBuffer);
        glBindBuffer(GL_ARRAY_BUFFER, nodeBuffer);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(byteSize),
                     cell.nodes.data(),
                     GL_STATIC_DRAW);

        gpu.uploadedRevision = cell.appliedRevision;
        gpu.nodeCount = cell.nodeCount;
        gpu.byteSize = static_cast<uint64_t>(byteSize);
        ++m_telemetry.uploadedCells;
        m_telemetry.uploadedBytes += static_cast<uint64_t>(byteSize);
        --budget;
    }
}

void SvoLodManager::removeUploadCell(const LodCellKey& key) {
    if (m_uploadQueued.erase(key) == 0) {
        return;
    }
    m_uploadQueue.erase(
        std::remove(m_uploadQueue.begin(), m_uploadQueue.end(), key),
        m_uploadQueue.end()
    );
}

void SvoLodManager::releaseGpuCell(const LodCellKey& key) {
    auto it = m_gpuCells.find(key);
    if (it == m_gpuCells.end()) {
        return;
    }

    if (it->second.nodeBuffer != 0) {
        GLuint buffer = static_cast<GLuint>(it->second.nodeBuffer);
        glDeleteBuffers(1, &buffer);
    }
    m_gpuCells.erase(it);
}

void SvoLodManager::updateTelemetry() {
    uint32_t active = 0;
    uint32_t missing = 0;
    uint32_t queuedBuild = 0;
    uint32_t building = 0;
    uint32_t ready = 0;
    uint32_t stale = 0;
    uint32_t evicting = 0;
    uint64_t cpuBytes = 0;
    for (const auto& [key, cell] : m_cells) {
        (void)key;
        cpuBytes += static_cast<uint64_t>(cell.nodes.size() * sizeof(LodSvoNode));
        if (cell.state != LodCellState::Missing) {
            ++active;
        }
        switch (cell.state) {
            case LodCellState::Missing:
                ++missing;
                break;
            case LodCellState::QueuedBuild:
                ++queuedBuild;
                break;
            case LodCellState::Building:
                ++building;
                break;
            case LodCellState::Ready:
                ++ready;
                break;
            case LodCellState::Stale:
                ++stale;
                break;
            case LodCellState::Evicting:
                ++evicting;
                break;
        }
    }
    uint64_t gpuBytes = 0;
    for (const auto& [key, gpu] : m_gpuCells) {
        (void)key;
        gpuBytes += gpu.byteSize;
    }

    m_telemetry.activeCells = active;
    m_telemetry.cellsMissing = missing;
    m_telemetry.cellsQueuedBuild = queuedBuild;
    m_telemetry.cellsBuilding = building;
    m_telemetry.cellsReady = ready;
    m_telemetry.cellsStale = stale;
    m_telemetry.cellsEvicting = evicting;
    m_telemetry.pendingCopies = static_cast<uint32_t>(m_dirtyQueue.size());
    m_telemetry.pendingApplies = static_cast<uint32_t>(m_buildComplete.size());
    m_telemetry.pendingUploads = static_cast<uint32_t>(m_uploadQueue.size());
    m_telemetry.cpuBytesCurrent = cpuBytes;
    m_telemetry.gpuBytesCurrent = gpuBytes;
}

void SvoLodManager::enforceCellLimit() {
    const bool enforceCount = (m_config.lodMaxCells > 0);
    const bool enforceCpu = (m_config.lodMaxCpuBytes > 0);
    const bool enforceGpu = (m_config.lodMaxGpuBytes > 0);
    if (!enforceCount && !enforceCpu && !enforceGpu) {
        return;
    }

    const int span = std::max(1, m_config.lodCellSpanChunks);
    const int rootSizeChunks = ceilPow2(span);
    auto cellBytes = [](const CellRecord& cell) -> uint64_t {
        return static_cast<uint64_t>(cell.nodes.size() * sizeof(LodSvoNode));
    };
    auto scoreForCell = [&](const LodCellKey& key, const CellRecord& cell)
        -> std::tuple<uint8_t, float, uint64_t> {
        const glm::vec3 center = cellWorldCenter(key, span, rootSizeChunks);
        const glm::vec3 delta = center - m_lastCameraPos;
        const float distanceSq = glm::dot(delta, delta);
        const uint8_t evictPriority = cell.visibleAsFarLod ? 0u : 1u;
        const uint64_t ageScore =
            std::numeric_limits<uint64_t>::max() - cell.lastTouchedFrame;
        return std::tuple<uint8_t, float, uint64_t>{
            evictPriority, distanceSq, ageScore
        };
    };
    auto scoreForGpu = [&](const LodCellKey& key) -> std::tuple<uint8_t, float, uint64_t> {
        auto cellIt = m_cells.find(key);
        if (cellIt == m_cells.end()) {
            return std::tuple<uint8_t, float, uint64_t>{
                0u, std::numeric_limits<float>::max(), 0u
            };
        }
        return scoreForCell(key, cellIt->second);
    };

    const size_t maxCells = enforceCount
        ? static_cast<size_t>(m_config.lodMaxCells)
        : std::numeric_limits<size_t>::max();
    const uint64_t maxCpuBytes = enforceCpu
        ? static_cast<uint64_t>(m_config.lodMaxCpuBytes)
        : std::numeric_limits<uint64_t>::max();
    const uint64_t maxGpuBytes = enforceGpu
        ? static_cast<uint64_t>(m_config.lodMaxGpuBytes)
        : std::numeric_limits<uint64_t>::max();

    uint64_t cpuBytes = 0;
    for (const auto& [key, cell] : m_cells) {
        (void)key;
        cpuBytes += cellBytes(cell);
    }
    uint64_t gpuBytes = 0;
    for (const auto& [key, gpu] : m_gpuCells) {
        (void)key;
        gpuBytes += gpu.byteSize;
    }

    auto overBudget = [&]() {
        return m_cells.size() > maxCells ||
               cpuBytes > maxCpuBytes ||
               gpuBytes > maxGpuBytes;
    };

    while (overBudget()) {
        if (gpuBytes > maxGpuBytes && !m_gpuCells.empty()) {
            auto gpuVictim = m_gpuCells.end();
            for (auto it = m_gpuCells.begin(); it != m_gpuCells.end(); ++it) {
                if (gpuVictim == m_gpuCells.end()) {
                    gpuVictim = it;
                    continue;
                }
                if (scoreForGpu(it->first) > scoreForGpu(gpuVictim->first)) {
                    gpuVictim = it;
                }
            }
            if (gpuVictim != m_gpuCells.end()) {
                if (gpuBytes >= gpuVictim->second.byteSize) {
                    gpuBytes -= gpuVictim->second.byteSize;
                } else {
                    gpuBytes = 0;
                }
                releaseGpuCell(gpuVictim->first);
                continue;
            }
        }

        auto victim = m_cells.end();
        for (auto it = m_cells.begin(); it != m_cells.end(); ++it) {
            if (it->second.state == LodCellState::Building ||
                it->second.state == LodCellState::QueuedBuild) {
                continue;
            }
            if (victim == m_cells.end()) {
                victim = it;
                continue;
            }
            if (scoreForCell(it->first, it->second) >
                scoreForCell(victim->first, victim->second)) {
                victim = it;
            }
        }

        if (victim == m_cells.end()) {
            break;
        }

        auto gpuIt = m_gpuCells.find(victim->first);
        if (gpuIt != m_gpuCells.end()) {
            if (gpuBytes >= gpuIt->second.byteSize) {
                gpuBytes -= gpuIt->second.byteSize;
            } else {
                gpuBytes = 0;
            }
        }
        uint64_t victimCpuBytes = cellBytes(victim->second);
        if (cpuBytes >= victimCpuBytes) {
            cpuBytes -= victimCpuBytes;
        } else {
            cpuBytes = 0;
        }

        removeDirtyCell(victim->first);
        removeUploadCell(victim->first);
        releaseGpuCell(victim->first);
        m_cells.erase(victim);
    }
}

void SvoLodManager::removeDirtyCell(const LodCellKey& key) {
    if (m_dirtyQueued.erase(key) == 0) {
        return;
    }
    m_dirtyQueue.erase(
        std::remove(m_dirtyQueue.begin(), m_dirtyQueue.end(), key),
        m_dirtyQueue.end()
    );
}

std::optional<LodBuildInput> SvoLodManager::makeBuildInput(const LodCellKey& key,
                                                           uint64_t revision) const {
    if (!m_chunkManager) {
        return std::nullopt;
    }

    const int span = std::max(1, m_config.lodCellSpanChunks);
    const int baseX = key.x * span;
    const int baseY = key.y * span;
    const int baseZ = key.z * span;

    LodBuildInput input;
    input.key = key;
    input.revision = revision;
    input.spanChunks = span;
    input.chunks.reserve(static_cast<size_t>(span * span * span));

    for (int dz = 0; dz < span; ++dz) {
        for (int dy = 0; dy < span; ++dy) {
            for (int dx = 0; dx < span; ++dx) {
                ChunkCoord coord{baseX + dx, baseY + dy, baseZ + dz};
                const Chunk* chunk = m_chunkManager->getChunk(coord);
                if (!chunk) {
                    continue;
                }

                LodChunkSnapshot snapshot;
                snapshot.coord = coord;
                chunk->copyBlocks(snapshot.blocks);
                input.chunks.push_back(std::move(snapshot));
            }
        }
    }

    return input;
}

} // namespace Rigel::Voxel
