#include "TestFramework.h"

#include "Rigel/Voxel/WorldGenerator.h"

using namespace Rigel::Voxel;

namespace {
BlockRegistry makeRegistry() {
    BlockRegistry registry;

    BlockType solid;
    solid.identifier = "rigel:stone";
    registry.registerBlock(solid.identifier, solid);

    BlockType surface;
    surface.identifier = "rigel:grass";
    registry.registerBlock(surface.identifier, surface);

    return registry;
}

WorldGenConfig makeFlatConfig() {
    WorldGenConfig config;
    config.seed = 123;
    config.solidBlock = "rigel:stone";
    config.surfaceBlock = "rigel:grass";
    config.terrain.baseHeight = 0.0f;
    config.terrain.heightVariation = 0.0f;
    config.terrain.surfaceDepth = 1;
    return config;
}
}

TEST_CASE(WorldGenerator_FlatSurface) {
    BlockRegistry registry = makeRegistry();
    WorldGenerator generator(registry);

    WorldGenConfig config = makeFlatConfig();
    generator.setConfig(config);

    ChunkBuffer buffer;
    generator.generate({0, 0, 0}, buffer);

    BlockState surface = buffer.at(0, 0, 0);
    BlockState above = buffer.at(0, 1, 0);

    CHECK_EQ(surface.id.type, registry.findByIdentifier("rigel:grass")->type);
    CHECK(above.isAir());
}

TEST_CASE(WorldGenerator_DisableSurfaceStage) {
    BlockRegistry registry = makeRegistry();
    WorldGenerator generator(registry);

    WorldGenConfig config = makeFlatConfig();
    config.stageEnabled["surface_rules"] = false;
    generator.setConfig(config);

    ChunkBuffer buffer;
    generator.generate({0, 0, 0}, buffer);

    BlockState surface = buffer.at(0, 0, 0);
    CHECK_EQ(surface.id.type, registry.findByIdentifier("rigel:stone")->type);
}

TEST_CASE(WorldGenerator_Deterministic) {
    BlockRegistry registry = makeRegistry();
    WorldGenerator generator(registry);

    WorldGenConfig config = makeFlatConfig();
    generator.setConfig(config);

    ChunkBuffer a;
    ChunkBuffer b;
    generator.generate({1, 0, 0}, a);
    generator.generate({1, 0, 0}, b);

    CHECK_EQ(a.blocks, b.blocks);
}
