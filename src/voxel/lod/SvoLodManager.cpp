#include "Rigel/Voxel/Lod/SvoLodManager.h"

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
}

void SvoLodManager::initialize() {
}

void SvoLodManager::update(const glm::vec3& cameraPos) {
    (void)cameraPos;
    if (!m_config.enabled) {
        return;
    }
    ++m_telemetry.updateCalls;
}

void SvoLodManager::reset() {
    m_telemetry = {};
}

void SvoLodManager::releaseRenderResources() {
}

} // namespace Rigel::Voxel
