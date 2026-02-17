#pragma once

#include "Rigel/Voxel/Lod/SvoLodTypes.h"
#include "Rigel/Voxel/RenderConfig.h"
#include "Rigel/Voxel/ChunkTasks.h"
#include "Rigel/Voxel/ChunkManager.h"
#include "Rigel/Voxel/BlockRegistry.h"

#include <glm/vec3.hpp>
#include <cstdint>
#include <cstddef>
#include <deque>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Rigel::Voxel {

struct SvoLodTelemetry {
    uint32_t activeCells = 0;
    uint32_t pendingCopies = 0;
    uint32_t pendingApplies = 0;
    uint64_t copiedCells = 0;
    uint64_t appliedCells = 0;
    uint64_t updateCalls = 0;
};

class SvoLodManager {
public:
    struct CellInfo {
        LodCellState state = LodCellState::Missing;
        uint64_t desiredRevision = 0;
        uint64_t queuedRevision = 0;
        uint64_t appliedRevision = 0;
        uint32_t sampledChunks = 0;
        uint32_t nodeCount = 0;
        uint32_t leafCount = 0;
        uint32_t mixedNodeCount = 0;
    };

    void setConfig(const SvoLodConfig& config);
    const SvoLodConfig& config() const { return m_config; }
    void setBuildThreads(size_t threadCount);
    void bind(const ChunkManager* chunkManager, const BlockRegistry* registry);

    void initialize();
    void update(const glm::vec3& cameraPos);
    void reset();
    void releaseRenderResources();

    const SvoLodTelemetry& telemetry() const { return m_telemetry; }
    size_t cellCount() const { return m_cells.size(); }
    std::optional<CellInfo> cellInfo(const LodCellKey& key) const;

private:
    struct CellRecord {
        LodCellState state = LodCellState::Missing;
        uint64_t desiredRevision = 0;
        uint64_t queuedRevision = 0;
        uint64_t appliedRevision = 0;
        uint32_t sampledChunks = 0;
        uint32_t nodeCount = 0;
        uint32_t leafCount = 0;
        uint32_t mixedNodeCount = 0;
        uint64_t nonAirVoxelCount = 0;
        uint64_t opaqueVoxelCount = 0;
        uint64_t nonOpaqueVoxelCount = 0;
        std::vector<LodSvoNode> nodes;
        uint32_t rootNode = LodSvoNode::INVALID_INDEX;
        uint64_t lastTouchedFrame = 0;
    };

    static SvoLodConfig sanitizeConfig(SvoLodConfig config);

    void ensureBuildPool();
    void scanChunkChanges();
    void processCopyBudget();
    void processApplyBudget();
    void enqueueDirtyChunk(ChunkCoord coord);
    void enqueueDirtyCell(const LodCellKey& key);
    void requeueDirtyCell(const LodCellKey& key);
    void updateTelemetry();
    void enforceCellLimit();
    void removeDirtyCell(const LodCellKey& key);
    std::optional<LodBuildInput> makeBuildInput(const LodCellKey& key, uint64_t revision) const;

    SvoLodConfig m_config;
    SvoLodTelemetry m_telemetry;
    const ChunkManager* m_chunkManager = nullptr;
    const BlockRegistry* m_registry = nullptr;
    size_t m_buildThreads = 1;
    std::unique_ptr<detail::ThreadPool> m_buildPool;
    detail::ConcurrentQueue<LodBuildOutput> m_buildComplete;
    std::unordered_map<ChunkCoord, uint32_t, ChunkCoordHash> m_knownChunkRevisions;
    std::unordered_map<LodCellKey, CellRecord, LodCellKeyHash> m_cells;
    std::deque<LodCellKey> m_dirtyQueue;
    std::unordered_set<LodCellKey, LodCellKeyHash> m_dirtyQueued;
    uint64_t m_frameCounter = 0;
};

} // namespace Rigel::Voxel
