#include "TestFramework.h"

#include "Rigel/Voxel/World.h"
#include "Rigel/Voxel/WorldResources.h"
#include "Rigel/Voxel/WorldView.h"

using namespace Rigel::Voxel;

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
    config.stream.viewDistanceChunks = 0;
    config.stream.unloadDistanceChunks = 0;
    config.stream.genQueueLimit = 0;
    config.stream.meshQueueLimit = 0;
    config.stream.applyBudgetPerFrame = 0;
    config.stream.workerThreads = 0;

    auto generator = std::make_shared<WorldGenerator>(resources.registry());
    generator->setConfig(config);
    world.setGenerator(generator);
    view.setGenerator(generator);
    view.setStreamConfig(config.stream);

    view.updateStreaming(glm::vec3(0.0f));
    view.updateMeshes();
    CHECK_EQ(world.chunkManager().loadedChunkCount(), static_cast<size_t>(1));
}
