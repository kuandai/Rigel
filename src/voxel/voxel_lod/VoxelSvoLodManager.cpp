#include "Rigel/Voxel/VoxelLod/VoxelSvoLodManager.h"

#include <algorithm>

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

} // namespace

VoxelSvoConfig VoxelSvoLodManager::sanitizeConfig(VoxelSvoConfig config) {
    if (config.nearMeshRadiusChunks < 0) {
        config.nearMeshRadiusChunks = 0;
    }
    if (config.startRadiusChunks < config.nearMeshRadiusChunks) {
        config.startRadiusChunks = config.nearMeshRadiusChunks;
    }
    if (config.maxRadiusChunks < config.startRadiusChunks) {
        config.maxRadiusChunks = config.startRadiusChunks;
    }
    if (config.transitionBandChunks < 0) {
        config.transitionBandChunks = 0;
    }

    if (config.levels < 1) {
        config.levels = 1;
    } else if (config.levels > 16) {
        config.levels = 16;
    }

    config.pageSizeVoxels = ceilPow2(std::max(8, config.pageSizeVoxels));
    config.pageSizeVoxels = std::clamp(config.pageSizeVoxels, 8, 256);

    config.minLeafVoxels = ceilPow2(std::max(1, config.minLeafVoxels));
    if (config.minLeafVoxels > config.pageSizeVoxels) {
        config.minLeafVoxels = config.pageSizeVoxels;
    }

    if (config.buildBudgetPagesPerFrame < 0) {
        config.buildBudgetPagesPerFrame = 0;
    }
    if (config.applyBudgetPagesPerFrame < 0) {
        config.applyBudgetPagesPerFrame = 0;
    }
    if (config.uploadBudgetPagesPerFrame < 0) {
        config.uploadBudgetPagesPerFrame = 0;
    }
    if (config.maxResidentPages < 0) {
        config.maxResidentPages = 0;
    }
    if (config.maxCpuBytes < 0) {
        config.maxCpuBytes = 0;
    }
    if (config.maxGpuBytes < 0) {
        config.maxGpuBytes = 0;
    }

    return config;
}

void VoxelSvoLodManager::setConfig(const VoxelSvoConfig& config) {
    m_config = sanitizeConfig(config);
}

void VoxelSvoLodManager::bind(const ChunkManager* chunkManager, const BlockRegistry* registry) {
    m_chunkManager = chunkManager;
    m_registry = registry;
}

void VoxelSvoLodManager::initialize() {
    m_initialized = true;
}

void VoxelSvoLodManager::update(const glm::vec3& cameraPos) {
    m_lastCameraPos = cameraPos;
    if (!m_config.enabled) {
        m_telemetry.activePages = 0;
        m_telemetry.pagesQueued = 0;
        m_telemetry.pagesBuilding = 0;
        m_telemetry.pagesReadyCpu = 0;
        m_telemetry.pagesUploaded = 0;
        m_telemetry.cpuBytesCurrent = 0;
        m_telemetry.gpuBytesCurrent = 0;
        return;
    }

    (void)m_chunkManager;
    (void)m_registry;
    (void)m_initialized;
    ++m_telemetry.updateCalls;
}

void VoxelSvoLodManager::uploadRenderResources() {
    if (!m_config.enabled) {
        return;
    }
    ++m_telemetry.uploadCalls;
}

void VoxelSvoLodManager::reset() {
    releaseRenderResources();
    m_lastCameraPos = glm::vec3(0.0f);
    m_telemetry = {};
    m_initialized = false;
}

void VoxelSvoLodManager::releaseRenderResources() {
    // Skeleton: no GL resources are owned yet.
}

} // namespace Rigel::Voxel
