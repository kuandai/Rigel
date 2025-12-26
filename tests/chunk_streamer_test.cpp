#include "TestFramework.h"

#include "Rigel/Voxel/ChunkStreamer.h"

using namespace Rigel::Voxel;

namespace {
std::shared_ptr<WorldGenerator> makeGenerator(BlockRegistry& registry) {
    BlockType solid;
    solid.identifier = "rigel:stone";
    registry.registerBlock(solid.identifier, solid);

    BlockType surface;
    surface.identifier = "rigel:grass";
    registry.registerBlock(surface.identifier, surface);

    auto generator = std::make_shared<WorldGenerator>(registry);
    WorldGenConfig config;
    config.seed = 1;
    config.solidBlock = solid.identifier;
    config.surfaceBlock = surface.identifier;
    config.terrain.baseHeight = 0.0f;
    config.terrain.heightVariation = 0.0f;
    config.terrain.surfaceDepth = 1;
    generator->setConfig(config);
    return generator;
}
}

TEST_CASE(ChunkStreamer_GeneratesSphere) {
    ChunkManager manager;
    BlockRegistry registry;
    auto generator = makeGenerator(registry);

    ChunkStreamer streamer;
    WorldGenConfig::StreamConfig stream;
    stream.viewDistanceChunks = 1;
    stream.unloadDistanceChunks = 1;
    stream.maxGeneratePerFrame = 0;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    streamer.bind(&manager, nullptr, generator);

    streamer.update(glm::vec3(0.0f));
    CHECK_EQ(manager.loadedChunkCount(), static_cast<size_t>(7));
}

TEST_CASE(ChunkStreamer_RespectsCap) {
    ChunkManager manager;
    BlockRegistry registry;
    auto generator = makeGenerator(registry);

    ChunkStreamer streamer;
    WorldGenConfig::StreamConfig stream;
    stream.viewDistanceChunks = 1;
    stream.unloadDistanceChunks = 1;
    stream.maxGeneratePerFrame = 2;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    streamer.bind(&manager, nullptr, generator);

    streamer.update(glm::vec3(0.0f));
    CHECK_EQ(manager.loadedChunkCount(), static_cast<size_t>(2));
}

TEST_CASE(ChunkStreamer_EvictsOutsideRadius) {
    ChunkManager manager;
    BlockRegistry registry;
    auto generator = makeGenerator(registry);

    ChunkStreamer streamer;
    WorldGenConfig::StreamConfig stream;
    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 0;
    stream.maxGeneratePerFrame = 0;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    streamer.bind(&manager, nullptr, generator);

    streamer.update(glm::vec3(0.0f));
    CHECK_EQ(manager.loadedChunkCount(), static_cast<size_t>(1));

    streamer.update(glm::vec3(static_cast<float>(ChunkSize * 4), 0.0f, 0.0f));
    CHECK_EQ(manager.loadedChunkCount(), static_cast<size_t>(1));
}
