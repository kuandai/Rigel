#pragma once

/**
 * @file RenderConfig.h
 * @brief Render configuration for voxel rendering.
 */

#include <glm/vec3.hpp>
#include <cstdint>

namespace Rigel::Voxel {

struct ShadowConfig {
    static constexpr int MaxCascades = 4;

    bool enabled = false;
    int cascades = 3;
    int mapSize = 1024;
    float maxDistance = 200.0f;
    float splitLambda = 0.5f;
    float bias = 0.0005f;
    float normalBias = 0.005f;
    int pcfRadius = 1;
    int pcfRadiusNear = 1;
    int pcfRadiusFar = 1;
    float transparentScale = 1.0f;
    float strength = 1.0f;
    float fadePower = 1.0f;
};

struct TaaConfig {
    bool enabled = false;
    float blend = 0.9f;
    float jitterScale = 1.0f;
};

struct SvoLodConfig {
    bool enabled = false;
    int nearMeshRadiusChunks = 8;
    int lodStartRadiusChunks = 10;
    int lodCellSpanChunks = 8;
    int lodMaxCells = 1024;
    int64_t lodMaxCpuBytes = 0;
    int64_t lodMaxGpuBytes = 0;
    int lodCopyBudgetPerFrame = 4;
    int lodApplyBudgetPerFrame = 4;
};


struct WorldRenderConfig {
    float renderDistance = 256.0f;
    glm::vec3 sunDirection = glm::vec3(0.5f, 1.0f, 0.3f);
    float transparentAlpha = 0.5f;
    ShadowConfig shadow;
    TaaConfig taa;
    SvoLodConfig svo;
    bool profilingEnabled = false;
};

} // namespace Rigel::Voxel
