#include "TestFramework.h"

#include "Rigel/Voxel/World.h"

using namespace Rigel::Voxel;

TEST_CASE(World_StreamingPopulatesChunks) {
    World world;

    BlockType solid;
    solid.identifier = "rigel:stone";
    world.blockRegistry().registerBlock(solid.identifier, solid);

    BlockType surface;
    surface.identifier = "rigel:grass";
    world.blockRegistry().registerBlock(surface.identifier, surface);

    WorldGenConfig config;
    config.solidBlock = solid.identifier;
    config.surfaceBlock = surface.identifier;
    config.terrain.baseHeight = 0.0f;
    config.terrain.heightVariation = 0.0f;
    config.terrain.surfaceDepth = 1;
    config.stream.viewDistanceChunks = 0;
    config.stream.unloadDistanceChunks = 0;
    config.stream.maxGeneratePerFrame = 0;

    auto generator = std::make_shared<WorldGenerator>(world.blockRegistry());
    generator->setConfig(config);
    world.setGenerator(generator);
    world.setStreamConfig(config.stream);

    world.updateStreaming(glm::vec3(0.0f));
    CHECK_EQ(world.chunkManager().loadedChunkCount(), static_cast<size_t>(1));
}
