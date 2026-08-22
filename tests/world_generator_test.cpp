#include "TestFramework.h"

#include "Rigel/Voxel/WorldGenerator.h"

#include <limits>

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
    WorldGenConfig config = makeFlatConfig();
    WorldGenerator generator(registry, config);

    ChunkBuffer buffer;
    generator.generate({0, 0, 0}, buffer);

    BlockState surface = buffer.at(0, 0, 0);
    BlockState above = buffer.at(0, 1, 0);

    CHECK_EQ(surface.id.type, registry.findByIdentifier("rigel:grass")->type);
    CHECK(above.isAir());
}

TEST_CASE(WorldGenerator_DisableSurfaceStage) {
    BlockRegistry registry = makeRegistry();
    WorldGenConfig config = makeFlatConfig();
    config.stageEnabled["surface_rules"] = false;
    WorldGenerator generator(registry, config);

    ChunkBuffer buffer;
    generator.generate({0, 0, 0}, buffer);

    BlockState surface = buffer.at(0, 0, 0);
    CHECK_EQ(surface.id.type, registry.findByIdentifier("rigel:stone")->type);
}

TEST_CASE(WorldGenerator_CaveStageFlagControlsCarving) {
    BlockRegistry registry = makeRegistry();
    WorldGenConfig config = makeFlatConfig();
    config.densityGraph.outputs["base_density"] = "solid";
    config.densityGraph.outputs["cave_density"] = "cave";
    config.densityGraph.nodes = {
        WorldGenConfig::DensityNodeConfig{
            .id = "solid",
            .type = "constant",
            .value = 1.0f
        },
        WorldGenConfig::DensityNodeConfig{
            .id = "cave",
            .type = "constant",
            .value = 1.0f
        }
    };
    config.caves.threshold = 0.5f;
    config.stageEnabled["surface_rules"] = false;

    WorldGenerator cavesEnabledGenerator(registry, config);
    ChunkBuffer cavesEnabled;
    cavesEnabledGenerator.generate({0, 0, 0}, cavesEnabled);
    CHECK(cavesEnabled.at(0, 0, 0).isAir());

    config.stageEnabled["caves"] = false;
    WorldGenerator cavesDisabledGenerator(registry, config);
    ChunkBuffer cavesDisabled;
    cavesDisabledGenerator.generate({0, 0, 0}, cavesDisabled);
    CHECK_EQ(
        cavesDisabled.at(0, 0, 0).id.type,
        registry.findByIdentifier("rigel:stone")->type
    );
}

TEST_CASE(WorldGenerator_Deterministic) {
    BlockRegistry registry = makeRegistry();
    WorldGenConfig config = makeFlatConfig();
    WorldGenerator generator(registry, config);

    ChunkBuffer a;
    ChunkBuffer b;
    generator.generate({1, 0, 0}, a);
    generator.generate({1, 0, 0}, b);

    CHECK_EQ(a.blocks, b.blocks);
}

TEST_CASE(WorldGenerator_GeneratesAtLoopConfigurationBoundaries) {
    BlockRegistry registry = makeRegistry();
    WorldGenConfig config = makeFlatConfig();
    config.terrain.surfaceDepth = WorldGenConfig::MaxSurfaceDepth;
    config.structures.features.push_back({
        .name = "bounded",
        .block = "rigel:grass",
        .chance = 1.0f,
        .minHeight = WorldGenConfig::MaxStructureHeight,
        .maxHeight = WorldGenConfig::MaxStructureHeight
    });
    config.validate("boundary test configuration");
    WorldGenerator generator(registry, config);

    ChunkBuffer buffer;
    generator.generate({0, 31, 0}, buffer);

    CHECK_EQ(
        buffer.at(0, Chunk::SIZE - 1, 0).id.type,
        registry.findByIdentifier("rigel:grass")->type
    );
}

TEST_CASE(WorldGenerator_ValidatesProgrammaticLoopBoundsBeforeStages) {
    BlockRegistry registry = makeRegistry();

    WorldGenConfig surfaceConfig = makeFlatConfig();
    surfaceConfig.terrain.surfaceDepth = std::numeric_limits<int>::max();
    std::string surfaceDiagnostic;
    try {
        WorldGenerator generator(registry, surfaceConfig);
    } catch (const std::invalid_argument& error) {
        surfaceDiagnostic = error.what();
    }
    CHECK_EQ(
        surfaceDiagnostic,
        "Invalid configuration value 'terrain.surface_depth' in "
        "'WorldGenerator configuration': must be no greater than 32"
    );

    WorldGenConfig featureConfig = makeFlatConfig();
    featureConfig.structures.features.push_back({
        .name = "invalid",
        .block = "rigel:grass",
        .chance = 1.0f,
        .minHeight = std::numeric_limits<int>::min(),
        .maxHeight = std::numeric_limits<int>::max()
    });
    std::string featureDiagnostic;
    try {
        WorldGenerator generator(registry, featureConfig);
    } catch (const std::invalid_argument& error) {
        featureDiagnostic = error.what();
    }
    CHECK_EQ(
        featureDiagnostic,
        "Invalid configuration value 'structures.features[0].max_height' in "
        "'WorldGenerator configuration': must be no greater than 1024"
    );

    WorldGenConfig listConfig = makeFlatConfig();
    listConfig.biomes.entries.resize(WorldGenConfig::MaxBiomeEntries + 1);
    std::string listDiagnostic;
    try {
        WorldGenerator generator(registry, listConfig);
    } catch (const std::invalid_argument& error) {
        listDiagnostic = error.what();
    }
    CHECK_EQ(
        listDiagnostic,
        "Invalid configuration value 'biomes.entries' in "
        "'WorldGenerator configuration': must contain no more than 32 entries"
    );
}

TEST_CASE(WorldGenerator_AcceptsProgrammaticMixedSignFeatureRange) {
    BlockRegistry registry = makeRegistry();
    WorldGenConfig config = makeFlatConfig();
    config.structures.features.push_back({
        .name = "mixed",
        .block = "rigel:grass",
        .chance = 1.0f,
        .minHeight = std::numeric_limits<int>::min(),
        .maxHeight = WorldGenConfig::MaxStructureHeight
    });

    WorldGenerator generator(registry, config);
    ChunkBuffer buffer;
    CHECK_NO_THROW(generator.generate({0, 0, 0}, buffer));
}

TEST_CASE(WorldGenerator_RejectsMissingRequiredBlock) {
    BlockRegistry registry = makeRegistry();
    WorldGenConfig config = makeFlatConfig();
    config.solidBlock = "rigel:missing";

    std::string diagnostic;
    try {
        WorldGenerator generator(registry, config);
    } catch (const std::exception& e) {
        diagnostic = e.what();
    }

    CHECK(diagnostic.find("required solid block") != std::string::npos);
    CHECK(diagnostic.find("rigel:missing") != std::string::npos);
}

TEST_CASE(WorldGenerator_DisabledStagesDoNotRequireMaterials) {
    BlockRegistry registry = makeRegistry();
    WorldGenConfig config = makeFlatConfig();
    config.solidBlock = "rigel:missing_solid";
    config.surfaceBlock = "rigel:missing_surface";
    config.stageEnabled["terrain_density"] = false;
    config.stageEnabled["surface_rules"] = false;

    CHECK_NO_THROW(WorldGenerator(registry, config));
}

TEST_CASE(WorldGenerator_RejectsInvalidDensityGraph) {
    BlockRegistry registry = makeRegistry();
    WorldGenConfig config = makeFlatConfig();
    config.densityGraph.outputs["base_density"] = "sum";
    config.densityGraph.nodes = {
        WorldGenConfig::DensityNodeConfig{
            .id = "sum",
            .type = "add",
            .inputs = {"missing"}
        }
    };

    std::string diagnostic;
    try {
        WorldGenerator generator(registry, config);
    } catch (const std::exception& e) {
        diagnostic = e.what();
    }

    CHECK(diagnostic.find("invalid density graph") != std::string::npos);
    CHECK(diagnostic.find("missing") != std::string::npos);
}
