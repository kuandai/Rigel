#include "TestFramework.h"

#include "Rigel/Voxel/Lod/SvoLodTransition.h"

using namespace Rigel::Voxel;

TEST_CASE(SvoLodTransition_BandComputationUsesChunkScaleAndHysteresis) {
    SvoLodConfig config;
    config.enabled = true;
    config.nearMeshRadiusChunks = 8;
    config.lodStartRadiusChunks = 10;

    const LodDistanceBands bands = makeLodDistanceBands(config, 1024.0f);
    CHECK_EQ(static_cast<int>(bands.nearEnterWorld), 256);
    CHECK_EQ(static_cast<int>(bands.nearExitWorld), 320);
    CHECK_EQ(static_cast<int>(bands.lodEnterWorld), 320);
    CHECK_EQ(static_cast<int>(bands.lodExitWorld), 256);
}

TEST_CASE(SvoLodTransition_NearVisibilityRespectsHysteresis) {
    SvoLodConfig config;
    config.enabled = true;
    config.nearMeshRadiusChunks = 4;
    config.lodStartRadiusChunks = 6;
    const LodDistanceBands bands = makeLodDistanceBands(config, 1000.0f);

    const float nearEnter = bands.nearEnterWorld;
    const float nearExit = bands.nearExitWorld;

    CHECK(shouldRenderNearChunk((nearEnter - 1.0f) * (nearEnter - 1.0f), false, bands));
    CHECK(!shouldRenderNearChunk((nearEnter + 1.0f) * (nearEnter + 1.0f), false, bands));
    CHECK(shouldRenderNearChunk((nearExit - 1.0f) * (nearExit - 1.0f), true, bands));
    CHECK(!shouldRenderNearChunk((nearExit + 1.0f) * (nearExit + 1.0f), true, bands));
}

TEST_CASE(SvoLodTransition_FarVisibilityRespectsHysteresis) {
    SvoLodConfig config;
    config.enabled = true;
    config.nearMeshRadiusChunks = 4;
    config.lodStartRadiusChunks = 6;
    const LodDistanceBands bands = makeLodDistanceBands(config, 1000.0f);

    const float lodEnter = bands.lodEnterWorld;
    const float lodExit = bands.lodExitWorld;

    CHECK(shouldRenderFarLod((lodEnter + 1.0f) * (lodEnter + 1.0f), false, bands));
    CHECK(!shouldRenderFarLod((lodEnter - 1.0f) * (lodEnter - 1.0f), false, bands));
    CHECK(shouldRenderFarLod((lodExit + 1.0f) * (lodExit + 1.0f), true, bands));
    CHECK(!shouldRenderFarLod((lodExit - 1.0f) * (lodExit - 1.0f), true, bands));
}
