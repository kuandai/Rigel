#pragma once

#include "Rigel/Voxel/RenderConfig.h"

namespace Rigel::Voxel {

struct LodDistanceBands {
    float nearEnterWorld = 0.0f;
    float nearExitWorld = 0.0f;
    float lodEnterWorld = 0.0f;
    float lodExitWorld = 0.0f;
    float renderDistanceWorld = 0.0f;
};

LodDistanceBands makeLodDistanceBands(const SvoLodConfig& config,
                                      float renderDistanceWorld);
bool shouldRenderNearChunk(float distanceSqWorld,
                           bool wasVisible,
                           const LodDistanceBands& bands);
bool shouldRenderFarLod(float distanceSqWorld,
                        bool wasVisible,
                        const LodDistanceBands& bands);

} // namespace Rigel::Voxel
