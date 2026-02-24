#include "TestFramework.h"

#include "Rigel/Voxel/VoxelLod/VoxelLodTransition.h"

#include <cmath>

using namespace Rigel::Voxel;

TEST_CASE(VoxelLodTransition_BandsScaleWithChunkSize) {
    VoxelSvoConfig config{};
    config.enabled = true;
    config.nearMeshRadiusChunks = 8;
    config.transitionBandChunks = 2;

    const VoxelLodDistanceBands bands = makeVoxelLodDistanceBands(config, 1024.0f);
    CHECK_EQ(static_cast<int>(bands.nearRadiusWorld), 256);
    CHECK_EQ(static_cast<int>(bands.farFadeStartWorld), 192);
    CHECK_EQ(static_cast<int>(bands.farFadeEndWorld), 320);
}

TEST_CASE(VoxelLodTransition_NearVisibilityAvoidsDeadZoneAndUsesHoldBand) {
    VoxelSvoConfig config{};
    config.enabled = true;
    config.nearMeshRadiusChunks = 8;
    config.transitionBandChunks = 2;

    const VoxelLodDistanceBands bands = makeVoxelLodDistanceBands(config, 2000.0f);
    const float nearEnter = bands.nearRadiusWorld;
    const float nearExit = bands.farFadeEndWorld;

    CHECK(shouldRenderNearVoxel((nearEnter - 1.0f) * (nearEnter - 1.0f), false, bands));
    CHECK(!shouldRenderNearVoxel((nearEnter + 1.0f) * (nearEnter + 1.0f), false, bands));
    CHECK(shouldRenderNearVoxel((nearExit - 1.0f) * (nearExit - 1.0f), true, bands));
    CHECK(!shouldRenderNearVoxel((nearExit + 1.0f) * (nearExit + 1.0f), true, bands));
}

TEST_CASE(VoxelLodTransition_FarFadeAndDistanceGateAreConsistent) {
    VoxelSvoConfig config{};
    config.enabled = true;
    config.nearMeshRadiusChunks = 8;
    config.transitionBandChunks = 2;

    const VoxelLodDistanceBands bands = makeVoxelLodDistanceBands(config, 500.0f);
    const float start = bands.farFadeStartWorld;
    const float end = bands.farFadeEndWorld;
    const float mid = (start + end) * 0.5f;

    CHECK(!shouldRenderFarVoxel((start - 1.0f) * (start - 1.0f), bands));
    CHECK(shouldRenderFarVoxel((start + 1.0f) * (start + 1.0f), bands));
    CHECK(std::abs(computeFarVoxelFade(start, bands) - 0.0f) <= 1e-6f);
    CHECK(std::abs(computeFarVoxelFade(mid, bands) - 0.5f) <= 1e-3f);
    CHECK(std::abs(computeFarVoxelFade(end, bands) - 1.0f) <= 1e-6f);
    CHECK(!shouldRenderFarVoxel(600.0f * 600.0f, bands));
}
