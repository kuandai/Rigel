#pragma once

#include "Rigel/Voxel/RenderConfig.h"
#include "Rigel/Voxel/ChunkTasks.h"
#include "Rigel/Voxel/VoxelLod/GeneratorSource.h"
#include "Rigel/Voxel/VoxelLod/VoxelPageCpu.h"
#include "Rigel/Voxel/VoxelLod/VoxelPageTree.h"

#include <glm/vec3.hpp>
#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Rigel::Voxel {

class ChunkManager;
class BlockRegistry;

struct VoxelSvoTelemetry {
    uint64_t updateCalls = 0;
    uint64_t uploadCalls = 0;
    uint64_t bricksSampled = 0;
    uint64_t voxelsSampled = 0;
    uint64_t loadedHits = 0;
    uint64_t persistenceHits = 0;
    uint64_t generatorHits = 0;
    uint64_t mipBuildMicros = 0;
    uint32_t activePages = 0;
    uint32_t pagesQueued = 0;
    uint32_t pagesBuilding = 0;
    uint32_t pagesReadyCpu = 0;
    uint32_t pagesUploaded = 0;
    std::array<uint32_t, 16> readyCpuPagesPerLevel{};
    std::array<uint64_t, 16> readyCpuNodesPerLevel{};
    uint64_t cpuBytesCurrent = 0;
    uint64_t gpuBytesCurrent = 0;
};

enum class VoxelPageState : uint8_t {
    Missing = 0,
    QueuedSample,
    BuildingCpu,
    ReadyCpu
};

struct VoxelSvoPageInfo {
    VoxelPageState state = VoxelPageState::Missing;
    uint64_t desiredRevision = 0;
    uint64_t queuedRevision = 0;
    uint64_t appliedRevision = 0;
    uint32_t nodeCount = 0;
    uint16_t leafMinVoxels = 1;
};

// Skeleton for a voxel-base far LOD system (Voxy-style) driven by render.svo_voxel.
//
// This intentionally does not generate any geometry yet; it only provides the
// module boundary and lifecycle wiring so later sprints can fill in the pipeline.
class VoxelSvoLodManager {
public:
    void setConfig(const VoxelSvoConfig& config);
    const VoxelSvoConfig& config() const { return m_config; }

    void setBuildThreads(size_t threadCount);
    void setChunkGenerator(GeneratorSource::ChunkGenerateCallback generator);

    void bind(const ChunkManager* chunkManager, const BlockRegistry* registry);

    void initialize();
    void update(const glm::vec3& cameraPos);
    void uploadRenderResources();
    void reset();
    void releaseRenderResources();

    const VoxelSvoTelemetry& telemetry() const { return m_telemetry; }
    size_t pageCount() const;
    std::optional<VoxelSvoPageInfo> pageInfo(const VoxelPageKey& key) const;
    void collectDebugPages(std::vector<std::pair<VoxelPageKey, VoxelSvoPageInfo>>& out) const;

private:
    struct PageRecord {
        VoxelPageKey key{};
        VoxelPageState state = VoxelPageState::Missing;
        uint64_t desiredRevision = 0;
        uint64_t queuedRevision = 0;
        uint64_t appliedRevision = 0;
        uint32_t nodeCount = 0;
        uint16_t leafMinVoxels = 1;
        uint64_t lastTouchedFrame = 0;
        std::shared_ptr<std::atomic_bool> cancel;
        VoxelPageCpu cpu;
        VoxelPageTree tree;
    };

    struct PageBuildOutput {
        VoxelPageKey key{};
        uint64_t revision = 0;
        uint16_t leafMinVoxels = 1;
        BrickSampleStatus sampleStatus = BrickSampleStatus::Miss;
        size_t sampledVoxels = 0;
        uint64_t mipBuildMicros = 0;
        VoxelPageCpu cpu;
        VoxelPageTree tree;
    };

    static VoxelSvoConfig sanitizeConfig(VoxelSvoConfig config);
    void ensureBuildPool();
    void processBuildCompletions();
    void seedDesiredPages(const glm::vec3& cameraPos);
    void enqueueBuild(const VoxelPageKey& key, uint64_t revision);
    void enforcePageLimit(const glm::vec3& cameraPos);
    PageRecord* findPage(const VoxelPageKey& key);
    const PageRecord* findPage(const VoxelPageKey& key) const;

    VoxelSvoConfig m_config{};
    VoxelSvoTelemetry m_telemetry{};
    const ChunkManager* m_chunkManager = nullptr;
    const BlockRegistry* m_registry = nullptr;
    size_t m_buildThreads = 1;
    GeneratorSource::ChunkGenerateCallback m_chunkGenerator;
    std::unique_ptr<detail::ThreadPool> m_buildPool;
    detail::ConcurrentQueue<PageBuildOutput> m_buildComplete;
    std::unordered_map<VoxelPageKey, PageRecord, VoxelPageKeyHash> m_pages;
    std::deque<VoxelPageKey> m_buildQueue;
    std::unordered_set<VoxelPageKey, VoxelPageKeyHash> m_buildQueued;
    uint64_t m_frameCounter = 0;
    glm::vec3 m_lastCameraPos{0.0f};
    bool m_initialized = false;
};

} // namespace Rigel::Voxel
