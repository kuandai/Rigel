#include "Rigel/Voxel/Lod/SvoLodTransition.h"

#include "Rigel/Voxel/Chunk.h"

#include <algorithm>

namespace Rigel::Voxel {

LodDistanceBands makeLodDistanceBands(const SvoLodConfig& config,
                                      float renderDistanceWorld) {
    LodDistanceBands bands;

    constexpr float kChunkWorld = static_cast<float>(Chunk::SIZE);
    const float nearRadius = static_cast<float>(std::max(0, config.nearMeshRadiusChunks)) *
        kChunkWorld;
    const float lodStart = static_cast<float>(
        std::max(config.lodStartRadiusChunks, config.nearMeshRadiusChunks)) * kChunkWorld;
    const float hysteresis = std::max(kChunkWorld, lodStart - nearRadius);

    bands.nearEnterWorld = nearRadius;
    bands.nearExitWorld = nearRadius + hysteresis;
    bands.lodEnterWorld = lodStart;
    bands.lodExitWorld = std::max(0.0f, lodStart - hysteresis);
    bands.renderDistanceWorld = std::max(0.0f, renderDistanceWorld);
    return bands;
}

bool shouldRenderNearChunk(float distanceSqWorld,
                           bool wasVisible,
                           const LodDistanceBands& bands) {
    const float renderDistanceSq = bands.renderDistanceWorld * bands.renderDistanceWorld;
    if (distanceSqWorld > renderDistanceSq) {
        return false;
    }

    const float nearEnterSq = bands.nearEnterWorld * bands.nearEnterWorld;
    if (distanceSqWorld <= nearEnterSq) {
        return true;
    }

    if (!wasVisible) {
        return false;
    }

    const float nearExitSq = bands.nearExitWorld * bands.nearExitWorld;
    return distanceSqWorld <= nearExitSq;
}

bool shouldRenderFarLod(float distanceSqWorld,
                        bool wasVisible,
                        const LodDistanceBands& bands) {
    const float renderDistanceSq = bands.renderDistanceWorld * bands.renderDistanceWorld;
    if (distanceSqWorld > renderDistanceSq) {
        return false;
    }

    const float lodEnterSq = bands.lodEnterWorld * bands.lodEnterWorld;
    if (distanceSqWorld >= lodEnterSq) {
        return true;
    }

    if (!wasVisible) {
        return false;
    }

    const float lodExitSq = bands.lodExitWorld * bands.lodExitWorld;
    return distanceSqWorld >= lodExitSq;
}

} // namespace Rigel::Voxel
