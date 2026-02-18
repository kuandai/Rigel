#pragma once

#include "Rigel/Voxel/RenderConfig.h"

namespace Rigel::Voxel {

struct VoxelLodDistanceBands {
    float nearRadiusWorld = 0.0f;
    float farFadeStartWorld = 0.0f;
    float farFadeEndWorld = 0.0f;
    float renderDistanceWorld = 0.0f;
};

VoxelLodDistanceBands makeVoxelLodDistanceBands(const VoxelSvoConfig& config,
                                                float renderDistanceWorld);

bool shouldRenderNearVoxel(float distanceSqWorld,
                           bool wasVisible,
                           const VoxelLodDistanceBands& bands);

bool shouldRenderFarVoxel(float distanceSqWorld,
                          const VoxelLodDistanceBands& bands);

float computeFarVoxelFade(float distanceWorld,
                          const VoxelLodDistanceBands& bands);

} // namespace Rigel::Voxel
