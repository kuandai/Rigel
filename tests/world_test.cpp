#include "TestFramework.h"

#include "Rigel/Voxel/World.h"
#include "Rigel/Voxel/WorldResources.h"
#include "Rigel/Voxel/WorldView.h"

#include <type_traits>

using namespace Rigel::Voxel;

static_assert(std::is_move_constructible_v<ChunkManager>);
static_assert(std::is_move_assignable_v<ChunkManager>);
static_assert(std::is_move_constructible_v<World>);
static_assert(std::is_move_assignable_v<World>);

TEST_CASE(World_StreamingPopulatesChunks) {
    WorldResources resources;
    World world(resources);
    WorldView view(world, resources);

    BlockType solid;
    solid.identifier = "rigel:stone";
    resources.registry().registerBlock(solid.identifier, solid);

    BlockType surface;
    surface.identifier = "rigel:grass";
    resources.registry().registerBlock(surface.identifier, surface);

    WorldGenConfig config;
    config.solidBlock = solid.identifier;
    config.surfaceBlock = surface.identifier;
    config.terrain.baseHeight = 0.0f;
    config.terrain.heightVariation = 0.0f;
    config.terrain.surfaceDepth = 1;
    StreamingConfig streaming;
    streaming.viewDistanceChunks = 0;
    streaming.unloadDistanceChunks = 0;
    streaming.genQueueLimit = 0;
    streaming.meshQueueLimit = 0;
    streaming.applyBudgetPerFrame = 0;
    streaming.workerThreads = 0;

    auto generator = std::make_shared<WorldGenerator>(resources.registry());
    generator->setConfig(config);
    world.setGenerator(generator);
    view.setGenerator(generator);
    view.setStreamConfig(streaming);

    view.updateStreaming(glm::vec3(0.0f));
    view.updateMeshes();
    CHECK_EQ(world.chunkManager().loadedChunkCount(), static_cast<size_t>(1));
}

TEST_CASE(WorldView_StreamAndRenderDistancesRemainIndependent) {
    WorldResources resources;
    World world(resources);
    WorldView view(world, resources);

    view.renderConfig().renderDistance = 300.0f;

    StreamingConfig streamConfig;
    streamConfig.viewDistanceChunks = 3;
    view.setStreamConfig(streamConfig);

    CHECK_EQ(view.viewDistanceChunks(), 3);
    CHECK_NEAR(view.renderConfig().renderDistance, 300.0f, 0.0001f);
}
