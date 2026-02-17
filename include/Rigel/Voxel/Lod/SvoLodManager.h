#pragma once

#include "Rigel/Voxel/Lod/SvoLodTypes.h"
#include "Rigel/Voxel/Lod/SvoLodTransition.h"
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
    uint32_t cellsMissing = 0;
    uint32_t cellsQueuedBuild = 0;
    uint32_t cellsBuilding = 0;
    uint32_t cellsReady = 0;
    uint32_t cellsStale = 0;
    uint32_t cellsEvicting = 0;
    uint32_t pendingCopies = 0;
    uint32_t pendingApplies = 0;
    uint32_t pendingUploads = 0;
    uint64_t cpuBytesCurrent = 0;
    uint64_t gpuBytesCurrent = 0;
    uint64_t scanMicros = 0;
    uint64_t copyMicros = 0;
    uint64_t applyMicros = 0;
    uint64_t uploadMicros = 0;
    uint64_t copiedCells = 0;
    uint64_t appliedCells = 0;
    uint64_t uploadedCells = 0;
    uint64_t uploadedBytes = 0;
    uint64_t updateCalls = 0;
};

class SvoLodManager {
public:
    struct DebugCellState {
        LodCellKey key{};
        LodCellState state = LodCellState::Missing;
        int spanChunks = 1;
        bool visibleAsFarLod = false;
    };

    struct OpaqueDrawInstance {
        glm::vec3 worldMin{0.0f};
        float worldSize = 0.0f;
    };

    struct CellInfo {
        LodCellState state = LodCellState::Missing;
        uint64_t desiredRevision = 0;
        uint64_t queuedRevision = 0;
        uint64_t appliedRevision = 0;
        bool visibleAsFarLod = false;
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
    void uploadRenderResources();
    void reset();
    void releaseRenderResources();

    const SvoLodTelemetry& telemetry() const { return m_telemetry; }
    size_t cellCount() const { return m_cells.size(); }
    std::optional<CellInfo> cellInfo(const LodCellKey& key) const;
    void collectDebugCells(std::vector<DebugCellState>& out) const;
    void collectOpaqueDrawInstances(std::vector<OpaqueDrawInstance>& out,
                                    const glm::vec3& cameraPos,
                                    float renderDistanceWorld);

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
        bool visibleAsFarLod = false;
        std::vector<LodSvoNode> nodes;
        uint32_t rootNode = LodSvoNode::INVALID_INDEX;
        uint64_t lastTouchedFrame = 0;
    };

    struct GpuCellRecord {
        unsigned int nodeBuffer = 0;
        uint64_t uploadedRevision = 0;
        uint32_t nodeCount = 0;
        uint64_t byteSize = 0;
    };

    static SvoLodConfig sanitizeConfig(SvoLodConfig config);

    void ensureBuildPool();
    void scanChunkChanges();
    void processCopyBudget();
    void processApplyBudget();
    void enqueueDirtyChunk(ChunkCoord coord);
    void enqueueDirtyCell(const LodCellKey& key);
    void requeueDirtyCell(const LodCellKey& key);
    void enqueueUploadCell(const LodCellKey& key);
    void processUploadBudget();
    void removeUploadCell(const LodCellKey& key);
    void releaseGpuCell(const LodCellKey& key);
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
    std::unordered_map<LodCellKey, GpuCellRecord, LodCellKeyHash> m_gpuCells;
    std::deque<LodCellKey> m_dirtyQueue;
    std::unordered_set<LodCellKey, LodCellKeyHash> m_dirtyQueued;
    std::deque<LodCellKey> m_uploadQueue;
    std::unordered_set<LodCellKey, LodCellKeyHash> m_uploadQueued;
    uint64_t m_frameCounter = 0;
    glm::vec3 m_lastCameraPos{0.0f};
};

} // namespace Rigel::Voxel
