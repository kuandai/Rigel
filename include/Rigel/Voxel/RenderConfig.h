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
    int lodViewDistanceChunks = 0;
    int lodCellSpanChunks = 8;
    int lodChunkSampleStep = 1;
    int lodMaxCells = 1024;
    int64_t lodMaxCpuBytes = 0;
    int64_t lodMaxGpuBytes = 0;
    int lodCopyBudgetPerFrame = 4;
    int lodApplyBudgetPerFrame = 4;
};

struct VoxelSvoConfig {
    bool enabled = false;

    // Chunk radii: voxel SVO starts outside near mesh radius.
    int nearMeshRadiusChunks = 8;
    int startRadiusChunks = 12;
    int maxRadiusChunks = 64;
    int transitionBandChunks = 2;

    // Clipmap layout / page representation.
    int levels = 4;
    int pageSizeVoxels = 64;   // Level 0 page dimensions (power of two).
    int minLeafVoxels = 1;     // MVP: global min leaf size (power of two).

    // Budgets (pages per frame).
    int buildBudgetPagesPerFrame = 1;
    int applyBudgetPagesPerFrame = 1;
    int uploadBudgetPagesPerFrame = 1;

    // Hard caps.
    int maxResidentPages = 512;
    int64_t maxCpuBytes = 256 * 1024 * 1024;
    int64_t maxGpuBytes = 256 * 1024 * 1024;
};


struct WorldRenderConfig {
    float renderDistance = 256.0f;
    glm::vec3 sunDirection = glm::vec3(0.5f, 1.0f, 0.3f);
    float transparentAlpha = 0.5f;
    ShadowConfig shadow;
    TaaConfig taa;
    SvoLodConfig svo;
    VoxelSvoConfig svoVoxel;
    bool profilingEnabled = false;
};

} // namespace Rigel::Voxel
