#include "Rigel/Voxel/Lod/SvoLodManager.h"

#include <algorithm>
#include <limits>
#include <vector>

namespace Rigel::Voxel {

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
    (void)cameraPos;
    if (!m_config.enabled) {
        updateTelemetry();
        return;
    }

    ++m_telemetry.updateCalls;
    ++m_frameCounter;
    ensureBuildPool();
    scanChunkChanges();
    processCopyBudget();
    processApplyBudget();
    enforceCellLimit();
    updateTelemetry();
}

void SvoLodManager::reset() {
    if (m_buildPool) {
        m_buildPool->stop();
        m_buildPool.reset();
    }

    m_knownChunkRevisions.clear();
    m_cells.clear();
    m_dirtyQueue.clear();
    m_dirtyQueued.clear();
    m_frameCounter = 0;

    LodBuildOutput output;
    while (m_buildComplete.tryPop(output)) {
    }

    m_telemetry = {};
}

void SvoLodManager::releaseRenderResources() {
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
    info.sampledChunks = it->second.sampledChunks;
    info.nodeCount = it->second.nodeCount;
    info.leafCount = it->second.leafCount;
    info.mixedNodeCount = it->second.mixedNodeCount;
    return info;
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

void SvoLodManager::updateTelemetry() {
    uint32_t active = 0;
    for (const auto& [key, cell] : m_cells) {
        (void)key;
        if (cell.state != LodCellState::Missing) {
            ++active;
        }
    }
    m_telemetry.activeCells = active;
    m_telemetry.pendingCopies = static_cast<uint32_t>(m_dirtyQueue.size());
    m_telemetry.pendingApplies = static_cast<uint32_t>(m_buildComplete.size());
}

void SvoLodManager::enforceCellLimit() {
    if (m_config.lodMaxCells <= 0) {
        return;
    }

    const size_t maxCells = static_cast<size_t>(m_config.lodMaxCells);
    while (m_cells.size() > maxCells) {
        auto victim = m_cells.end();
        uint64_t oldestFrame = std::numeric_limits<uint64_t>::max();

        for (auto it = m_cells.begin(); it != m_cells.end(); ++it) {
            if (it->second.state == LodCellState::Building ||
                it->second.state == LodCellState::QueuedBuild) {
                continue;
            }
            if (it->second.lastTouchedFrame < oldestFrame) {
                oldestFrame = it->second.lastTouchedFrame;
                victim = it;
            }
        }

        if (victim == m_cells.end()) {
            break;
        }
        removeDirtyCell(victim->first);
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
