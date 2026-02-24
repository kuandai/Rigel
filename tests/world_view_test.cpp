#include "TestFramework.h"

#include "Rigel/Voxel/BlockType.h"
#include "Rigel/Voxel/World.h"
#include "Rigel/Voxel/WorldGenConfig.h"
#include "Rigel/Voxel/WorldGenerator.h"
#include "Rigel/Voxel/WorldResources.h"
#include "Rigel/Voxel/WorldView.h"

using namespace Rigel::Voxel;

TEST_CASE(WorldView_SetRenderConfigSyncsSvoVoxelConfig) {
    WorldResources resources;
    World world(resources);
    WorldView view(world, resources);

    WorldRenderConfig config;
    config.svoVoxel.enabled = true;
    config.svoVoxel.nearMeshRadiusChunks = 6;
    config.svoVoxel.maxRadiusChunks = 40;
    config.svoVoxel.transitionBandChunks = 3;
    config.svoVoxel.levels = 3;
    config.svoVoxel.pageSizeVoxels = 64;
    config.svoVoxel.minLeafVoxels = 4;
    config.svoVoxel.buildBudgetPagesPerFrame = 7;
    config.svoVoxel.applyBudgetPagesPerFrame = 8;
    config.svoVoxel.uploadBudgetPagesPerFrame = 9;
    config.svoVoxel.maxResidentPages = 777;
    config.svoVoxel.maxCpuBytes = 1234;
    config.svoVoxel.maxGpuBytes = 5678;

    view.setRenderConfig(config);

    const auto& svo = view.svoVoxelConfig();
    CHECK(svo.enabled);
    CHECK_EQ(svo.nearMeshRadiusChunks, 6);
    CHECK_EQ(svo.maxRadiusChunks, 40);
    CHECK_EQ(svo.transitionBandChunks, 3);
    CHECK_EQ(svo.levels, 3);
    CHECK_EQ(svo.pageSizeVoxels, 64);
    CHECK_EQ(svo.minLeafVoxels, 4);
    CHECK_EQ(svo.buildBudgetPagesPerFrame, 7);
    CHECK_EQ(svo.applyBudgetPagesPerFrame, 8);
    CHECK_EQ(svo.uploadBudgetPagesPerFrame, 9);
    CHECK_EQ(svo.maxResidentPages, 777);
    CHECK_EQ(svo.maxCpuBytes, static_cast<int64_t>(1234));
    CHECK_EQ(svo.maxGpuBytes, static_cast<int64_t>(5678));
}

TEST_CASE(WorldView_NearTerrainRenderToggle_DefaultOnAndMutable) {
    WorldResources resources;
    World world(resources);
    WorldView view(world, resources);

    CHECK(view.nearTerrainRenderingEnabled());
    view.setNearTerrainRenderingEnabled(false);
    CHECK(!view.nearTerrainRenderingEnabled());
    view.setNearTerrainRenderingEnabled(true);
    CHECK(view.nearTerrainRenderingEnabled());
}

TEST_CASE(WorldView_SetRenderConfig_ToggleSvoVoxelHotReloadResetsAndReenables) {
    WorldResources resources;
    World world(resources);
    WorldView view(world, resources);

    WorldRenderConfig config;
    config.svoVoxel.enabled = true;
    view.setRenderConfig(config);

    view.updateStreaming(glm::vec3(0.0f, 0.0f, 0.0f));
    CHECK(view.svoVoxelTelemetry().updateCalls >= 1u);

    config.svoVoxel.enabled = false;
    view.setRenderConfig(config);
    CHECK(!view.renderConfig().svoVoxel.enabled);
    view.updateStreaming(glm::vec3(0.0f, 0.0f, 0.0f));
    CHECK_EQ(view.svoVoxelTelemetry().updateCalls, 0u);
    CHECK_EQ(view.svoVoxelTelemetry().activePages, 0u);

    config.svoVoxel.enabled = true;
    view.setRenderConfig(config);
    CHECK(view.renderConfig().svoVoxel.enabled);
    view.updateStreaming(glm::vec3(0.0f, 0.0f, 0.0f));
    CHECK(view.svoVoxelTelemetry().updateCalls >= 1u);
}

TEST_CASE(WorldView_VoxelSvoUpdateIsThrottledWhenChunkStreamingIsOverloaded) {
    WorldResources resources;
    World world(resources);
    WorldView view(world, resources);

    BlockType solid;
    solid.identifier = "rigel:stone";
    resources.registry().registerBlock(solid.identifier, solid);

    BlockType surface;
    surface.identifier = "rigel:grass";
    resources.registry().registerBlock(surface.identifier, surface);

    WorldGenConfig genConfig;
    genConfig.solidBlock = solid.identifier;
    genConfig.surfaceBlock = surface.identifier;
    genConfig.stream.viewDistanceChunks = 1;
    genConfig.stream.unloadDistanceChunks = 1;
    genConfig.stream.genQueueLimit = 1;
    genConfig.stream.meshQueueLimit = 1;
    genConfig.stream.applyBudgetPerFrame = 0;
    genConfig.stream.workerThreads = 0;

    auto generator = std::make_shared<WorldGenerator>(resources.registry());
    generator->setConfig(genConfig);
    world.setGenerator(generator);
    view.setGenerator(generator);
    view.setStreamConfig(genConfig.stream);

    WorldRenderConfig config;
    config.svoVoxel.enabled = true;
    config.svoVoxel.maxResidentPages = 64;
    config.svoVoxel.buildBudgetPagesPerFrame = 1;
    config.svoVoxel.applyBudgetPagesPerFrame = 1;
    view.setRenderConfig(config);

    for (int i = 0; i < 12; ++i) {
        view.updateStreaming(glm::vec3(0.0f, 0.0f, 0.0f));
    }

    CHECK(view.svoVoxelTelemetry().updateCalls > 0u);
    CHECK(view.svoVoxelTelemetry().updateCalls < 12u);
}

TEST_CASE(WorldView_VoxelSvoClearRelease_IsIdempotentAndReinitializable) {
    WorldResources resources;
    World world(resources);
    WorldView view(world, resources);

    WorldRenderConfig config;
    config.svoVoxel.enabled = true;
    view.setRenderConfig(config);

    view.updateStreaming(glm::vec3(0.0f, 0.0f, 0.0f));
    CHECK(view.svoVoxelTelemetry().updateCalls >= 1u);

    view.clear();
    CHECK_EQ(view.svoVoxelTelemetry().updateCalls, 0u);
    CHECK_EQ(view.svoVoxelTelemetry().activePages, 0u);

    view.releaseRenderResources();
    view.clear();
    view.releaseRenderResources();

    view.updateStreaming(glm::vec3(0.0f, 0.0f, 0.0f));
    CHECK(view.svoVoxelTelemetry().updateCalls >= 1u);
}
