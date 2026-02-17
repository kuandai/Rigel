#include "TestFramework.h"

#include "Rigel/Voxel/BlockType.h"
#include "Rigel/Voxel/World.h"
#include "Rigel/Voxel/WorldGenConfig.h"
#include "Rigel/Voxel/WorldGenerator.h"
#include "Rigel/Voxel/WorldResources.h"
#include "Rigel/Voxel/WorldView.h"

using namespace Rigel::Voxel;

TEST_CASE(WorldView_SetRenderConfigSyncsSvoConfig) {
    WorldResources resources;
    World world(resources);
    WorldView view(world, resources);

    WorldRenderConfig config;
    config.svo.enabled = true;
    config.svo.nearMeshRadiusChunks = 5;
    config.svo.lodStartRadiusChunks = 9;
    config.svo.lodCellSpanChunks = 4;
    config.svo.lodMaxCells = 333;
    config.svo.lodCopyBudgetPerFrame = 6;
    config.svo.lodApplyBudgetPerFrame = 7;

    view.setRenderConfig(config);

    const auto& svo = view.svoConfig();
    CHECK(svo.enabled);
    CHECK_EQ(svo.nearMeshRadiusChunks, 5);
    CHECK_EQ(svo.lodStartRadiusChunks, 9);
    CHECK_EQ(svo.lodCellSpanChunks, 4);
    CHECK_EQ(svo.lodMaxCells, 333);
    CHECK_EQ(svo.lodCopyBudgetPerFrame, 6);
    CHECK_EQ(svo.lodApplyBudgetPerFrame, 7);
}

TEST_CASE(WorldView_SvoLifecycleHooksUpdateAndResetTelemetry) {
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
    genConfig.stream.viewDistanceChunks = 0;
    genConfig.stream.unloadDistanceChunks = 0;
    genConfig.stream.genQueueLimit = 0;
    genConfig.stream.meshQueueLimit = 0;
    genConfig.stream.applyBudgetPerFrame = 0;
    genConfig.stream.workerThreads = 0;

    auto generator = std::make_shared<WorldGenerator>(resources.registry());
    generator->setConfig(genConfig);
    world.setGenerator(generator);
    view.setGenerator(generator);
    view.setStreamConfig(genConfig.stream);

    WorldRenderConfig config;
    config.svo.enabled = true;
    view.setRenderConfig(config);

    view.updateStreaming(glm::vec3(0.0f, 0.0f, 0.0f));
    view.updateStreaming(glm::vec3(1.0f, 2.0f, 3.0f));

    CHECK(view.svoTelemetry().updateCalls >= 1u);
    CHECK(view.svoTelemetry().updateCalls <= 2u);

    view.clear();
    CHECK_EQ(view.svoTelemetry().updateCalls, 0u);
}

TEST_CASE(WorldView_SvoPipelineBindsChunkManagerDataSource) {
    WorldResources resources;
    World world(resources);
    WorldView view(world, resources);

    BlockType stone;
    stone.identifier = "rigel:stone";
    stone.isOpaque = true;
    resources.registry().registerBlock(stone.identifier, stone);
    auto stoneId = resources.registry().findByIdentifier(stone.identifier);
    CHECK(stoneId.has_value());

    BlockState state;
    state.id = *stoneId;
    world.setBlock(33, 33, 33, state);

    WorldRenderConfig config;
    config.svo.enabled = true;
    config.svo.lodCellSpanChunks = 4;
    config.svo.lodCopyBudgetPerFrame = 1;
    config.svo.lodApplyBudgetPerFrame = 0;
    view.setRenderConfig(config);

    view.updateStreaming(glm::vec3(0.0f, 0.0f, 0.0f));
    CHECK_EQ(view.svoTelemetry().copiedCells, 1u);
    CHECK(view.svoTelemetry().activeCells > 0u);
}

TEST_CASE(WorldView_SvoUpdateIsThrottledWhenChunkStreamingIsOverloaded) {
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
    config.svo.enabled = true;
    config.svo.lodCopyBudgetPerFrame = 1;
    config.svo.lodApplyBudgetPerFrame = 1;
    view.setRenderConfig(config);

    for (int i = 0; i < 12; ++i) {
        view.updateStreaming(glm::vec3(0.0f, 0.0f, 0.0f));
    }

    CHECK(view.svoTelemetry().updateCalls > 0u);
    CHECK(view.svoTelemetry().updateCalls < 12u);
}

TEST_CASE(WorldView_SvoClearRelease_IsIdempotentAndReinitializable) {
    WorldResources resources;
    World world(resources);
    WorldView view(world, resources);

    BlockType stone;
    stone.identifier = "rigel:stone";
    stone.isOpaque = true;
    resources.registry().registerBlock(stone.identifier, stone);
    auto stoneId = resources.registry().findByIdentifier(stone.identifier);
    CHECK(stoneId.has_value());
    if (!stoneId) {
        return;
    }

    BlockState state;
    state.id = *stoneId;
    world.setBlock(33, 33, 33, state);

    WorldRenderConfig config;
    config.svo.enabled = true;
    config.svo.lodCellSpanChunks = 4;
    config.svo.lodCopyBudgetPerFrame = 8;
    config.svo.lodApplyBudgetPerFrame = 8;
    view.setRenderConfig(config);

    view.updateStreaming(glm::vec3(0.0f, 0.0f, 0.0f));
    CHECK(view.svoTelemetry().updateCalls >= 1u);

    view.clear();
    CHECK_EQ(view.svoTelemetry().updateCalls, 0u);
    CHECK_EQ(view.svoTelemetry().activeCells, 0u);

    view.releaseRenderResources();
    view.clear();
    view.releaseRenderResources();

    view.updateStreaming(glm::vec3(0.0f, 0.0f, 0.0f));
    CHECK(view.svoTelemetry().updateCalls >= 1u);
}
