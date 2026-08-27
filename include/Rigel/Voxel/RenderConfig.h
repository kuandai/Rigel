#pragma once

#include "RenderProfile.h"

namespace Rigel::Voxel {

// Transitional source-compatible value for low-level callers while renderer
// policy moves to RenderProfile. Normal application startup does not construct
// or load this type.
struct ShadowConfig : ShadowProfile {
    int pcfRadius = 2;
    float maxDistance = maximumDistanceWorldUnits;
};

struct TaaConfig : TemporalAAProfile {};

struct WorldRenderConfig {
    float renderDistance = 256.0f;
    glm::vec3 sunDirection = glm::vec3(0.5f, 1.0f, 0.3f);
    float transparentAlpha = 0.5f;
    ShadowConfig shadow;
    TaaConfig taa;
};

} // namespace Rigel::Voxel
