#include "Rigel/Voxel/VoxelLod/VoxelLodTransition.h"

#include "Rigel/Voxel/Chunk.h"

#include <algorithm>

namespace Rigel::Voxel {

VoxelLodDistanceBands makeVoxelLodDistanceBands(const VoxelSvoConfig& config,
                                                float renderDistanceWorld) {
    VoxelLodDistanceBands bands{};

    const float chunkWorld = static_cast<float>(Chunk::SIZE);
    const float nearRadius = static_cast<float>(std::max(0, config.nearMeshRadiusChunks)) * chunkWorld;
    const float startRadius = static_cast<float>(
        std::max(config.startRadiusChunks, config.nearMeshRadiusChunks)) * chunkWorld;
    const float transitionBand = static_cast<float>(std::max(0, config.transitionBandChunks)) * chunkWorld;

    bands.nearRadiusWorld = nearRadius;
    bands.farFadeStartWorld = std::max(nearRadius, startRadius - transitionBand);
    bands.farFadeEndWorld = std::max(bands.farFadeStartWorld, startRadius + transitionBand);
    bands.renderDistanceWorld = std::max(0.0f, renderDistanceWorld);
    return bands;
}

bool shouldRenderNearVoxel(float distanceSqWorld,
                           bool wasVisible,
                           const VoxelLodDistanceBands& bands) {
    const float renderDistanceSq = bands.renderDistanceWorld * bands.renderDistanceWorld;
    if (distanceSqWorld > renderDistanceSq) {
        return false;
    }

    // Avoid dead zones: if far fade starts outside near radius, near rendering
    // stays active up to far fade start.
    const float nearEnter = std::max(bands.nearRadiusWorld, bands.farFadeStartWorld);
    const float nearEnterSq = nearEnter * nearEnter;
    if (distanceSqWorld <= nearEnterSq) {
        return true;
    }

    if (!wasVisible) {
        return false;
    }

    const float nearExitSq = bands.farFadeEndWorld * bands.farFadeEndWorld;
    return distanceSqWorld <= nearExitSq;
}

bool shouldRenderFarVoxel(float distanceSqWorld,
                          const VoxelLodDistanceBands& bands) {
    const float renderDistanceSq = bands.renderDistanceWorld * bands.renderDistanceWorld;
    if (distanceSqWorld > renderDistanceSq) {
        return false;
    }
    return distanceSqWorld >= bands.farFadeStartWorld * bands.farFadeStartWorld;
}

float computeFarVoxelFade(float distanceWorld,
                          const VoxelLodDistanceBands& bands) {
    if (distanceWorld <= bands.farFadeStartWorld) {
        return 0.0f;
    }
    if (distanceWorld >= bands.farFadeEndWorld) {
        return 1.0f;
    }

    const float width = bands.farFadeEndWorld - bands.farFadeStartWorld;
    if (width <= 0.0001f) {
        return 1.0f;
    }
    const float t = (distanceWorld - bands.farFadeStartWorld) / width;
    return std::clamp(t, 0.0f, 1.0f);
}

} // namespace Rigel::Voxel
