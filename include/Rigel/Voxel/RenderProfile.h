#pragma once

/**
 * @file RenderProfile.h
 * @brief Shipped renderer policy and explicit low-level diagnostic inputs.
 */

#include <glm/vec3.hpp>

namespace Rigel::Voxel {

// The installed application uses these shipped values as one internal Shadows
// On profile. Tests and developer diagnostics may install an exact replacement
// through WorldView::setRenderProfileForDiagnostics().
struct ShadowProfile {
    static constexpr int MaxCascades = 4;
    static constexpr int MaxMapSize = 6144;
    static constexpr int MaxPcfRadius = 4;

    int cascades = 3;
    int mapSize = 6144;
    float maximumDistanceWorldUnits = 200.0f;
    float splitLambda = 0.25f;
    float bias = 0.001f;
    float normalBias = 0.02f;
    int pcfRadiusNear = 2;
    int pcfRadiusFar = 3;
    float transparentScale = 1.0f;
    float strength = 3.0f;
    float fadePower = 1.0f;
};

// TAA remains an internal renderer experiment. It is deliberately absent from
// UserPreferences and save-owned settings.
struct TemporalAAProfile {
    bool enabled = false;
    float blend = 0.95f;
    float jitterScale = 1.0f;
};

// Static art direction and low-level renderer tuning are shipped policy, not
// player or world configuration. The active View Distance policy supplies
// render and shadow ranges separately at frame construction time.
struct RenderProfile {
    glm::vec3 sunDirection = glm::vec3(0.5f, 1.0f, 0.3f);
    float transparentAlpha = 0.5f;
    ShadowProfile shadow;
    TemporalAAProfile temporalAA;
};

} // namespace Rigel::Voxel
