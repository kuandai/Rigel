#pragma once

#include "Rigel/Voxel/RenderConfig.h"

#include <glm/vec3.hpp>
#include <cstdint>

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
    uint32_t activePages = 0;
    uint32_t pagesQueued = 0;
    uint32_t pagesBuilding = 0;
    uint32_t pagesReadyCpu = 0;
    uint32_t pagesUploaded = 0;
    uint64_t cpuBytesCurrent = 0;
    uint64_t gpuBytesCurrent = 0;
};

// Skeleton for a voxel-base far LOD system (Voxy-style) driven by render.svo_voxel.
//
// This intentionally does not generate any geometry yet; it only provides the
// module boundary and lifecycle wiring so later sprints can fill in the pipeline.
class VoxelSvoLodManager {
public:
    void setConfig(const VoxelSvoConfig& config);
    const VoxelSvoConfig& config() const { return m_config; }

    void bind(const ChunkManager* chunkManager, const BlockRegistry* registry);

    void initialize();
    void update(const glm::vec3& cameraPos);
    void uploadRenderResources();
    void reset();
    void releaseRenderResources();

    const VoxelSvoTelemetry& telemetry() const { return m_telemetry; }

private:
    static VoxelSvoConfig sanitizeConfig(VoxelSvoConfig config);

    VoxelSvoConfig m_config{};
    VoxelSvoTelemetry m_telemetry{};
    const ChunkManager* m_chunkManager = nullptr;
    const BlockRegistry* m_registry = nullptr;
    glm::vec3 m_lastCameraPos{0.0f};
    bool m_initialized = false;
};

} // namespace Rigel::Voxel
