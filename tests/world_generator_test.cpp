#include "TestFramework.h"

#include "Rigel/Config/ConfigSource.h"
#include "Rigel/Voxel/ChunkManager.h"
#include "Rigel/Voxel/ChunkStreamer.h"
#include "Rigel/Voxel/WorldConfigProvider.h"
#include "Rigel/Voxel/WorldGenerator.h"
#include "Rigel/Voxel/WorldMeshStore.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <set>

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

WorldConfiguration loadShippedConfiguration() {
    Rigel::Asset::AssetManager assets;
    assets.loadManifest("manifest.yaml");
    WorldConfigProvider provider;
    provider.addSource(
        std::make_unique<Rigel::Config::EmbeddedConfigSource>(
            assets, "raw/world_config"));
    return provider.loadConfig();
}

void registerShippedGenerationBlocks(BlockRegistry& registry,
                                     const WorldGenConfig& config) {
    std::set<std::string> identifiers{
        config.solidBlock,
        config.surfaceBlock,
        "base:water[type=source]",
        "base:sand"};
    for (const auto& biome : config.biomes.entries) {
        for (const auto& layer : biome.surface) {
            identifiers.insert(layer.block);
        }
    }
    for (const auto& feature : config.structures.features) {
        identifiers.insert(feature.block);
    }
    for (const std::string& identifier : identifiers) {
        BlockType block;
        block.identifier = identifier;
        registry.registerBlock(identifier, std::move(block));
    }
}

struct OutOfBoundsLifecycle {
    ChunkStreamer::DebugChunkState target;
    ChunkStreamer::WorkMetrics work;
    bool quiescent = false;
};

OutOfBoundsLifecycle runOutOfBoundsLifecycle(
    ChunkCoord target,
    StreamingConfig streaming,
    BlockRegistry& registry,
    const std::shared_ptr<const WorldGenerator>& generator) {
    ChunkManager manager;
    manager.setRegistry(&registry);
    WorldMeshStore meshStore;
    ChunkStreamer streamer(
        manager, meshStore, registry, nullptr, generator);
    streaming.viewDistanceChunks = 1;
    streaming.unloadDistanceChunks = 1;
    streaming.workerThreads = 0;
    streamer.setConfig(streaming);
    streamer.setChunkLoader([](ChunkLoadRequest) {
        return ChunkLoadRequestResult::Missing;
    });
    streamer.markSpawnDiscoveryComplete();

    OutOfBoundsLifecycle result;
    for (int update = 0; update < 16; ++update) {
        streamer.update(target.toWorldCenter());
        streamer.processCompletions();
        if (streamer.diagnostics().state ==
            StreamingLifecycleState::Quiescent) {
            result.quiescent = true;
            break;
        }
    }
    result.work = streamer.workMetrics();
    std::vector<ChunkStreamer::DebugChunkState> states;
    streamer.getDebugStates(states, target, 0);
    if (!states.empty()) {
        result.target = states.front();
    }
    return result;
}
}

TEST_CASE(WorldGenerator_ShippedBoundsDoNotImplyEmptyGeneratedChunks) {
    const WorldConfiguration shipped = loadShippedConfiguration();
    BlockRegistry registry;
    registerShippedGenerationBlocks(registry, shipped.generation);
    auto generator = std::make_shared<const WorldGenerator>(
        registry, shipped.generation);
    const int belowY = worldToChunk(
        0, shipped.generation.world.minY, 0).y - 1;
    const int aboveY = worldToChunk(
        0, shipped.generation.world.maxY, 0).y + 1;

    ChunkBuffer below;
    ChunkBuffer above;
    generator->generate({0, belowY, 0}, below);
    generator->generate({0, aboveY, 0}, above);
    const auto occupied = [](const ChunkBuffer& buffer) {
        return std::count_if(
            buffer.blocks.begin(), buffer.blocks.end(),
            [](const BlockState& block) { return !block.isAir(); });
    };

    CHECK(occupied(below) > 0);
    CHECK_EQ(occupied(above), static_cast<std::ptrdiff_t>(0));

    const auto belowLifecycle = runOutOfBoundsLifecycle(
        {0, belowY, 0}, shipped.streaming, registry, generator);
    CHECK(belowLifecycle.quiescent);
    CHECK_EQ(belowLifecycle.target.voxelOccupancy,
             ChunkStreamer::DebugVoxelOccupancy::Nonempty);
    CHECK(belowLifecycle.work.meshJobsStarted > 0);
    CHECK_EQ(belowLifecycle.work.meshJobsStarted,
             belowLifecycle.work.meshJobsAccepted);

    const auto aboveLifecycle = runOutOfBoundsLifecycle(
        {0, aboveY, 0}, shipped.streaming, registry, generator);
    CHECK(aboveLifecycle.quiescent);
    CHECK_EQ(aboveLifecycle.target.voxelOccupancy,
             ChunkStreamer::DebugVoxelOccupancy::Empty);
    CHECK_EQ(aboveLifecycle.work.meshJobsStarted, static_cast<uint64_t>(0));
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

TEST_CASE(WorldGenerator_RejectsProgrammaticDensityGraphFanout) {
    BlockRegistry registry = makeRegistry();

    WorldGenConfig nodesConfig = makeFlatConfig();
    nodesConfig.densityGraph.nodes.resize(
        WorldGenConfig::MaxDensityGraphNodes + 1);
    std::string nodesDiagnostic;
    try {
        WorldGenerator generator(registry, nodesConfig);
    } catch (const std::invalid_argument& error) {
        nodesDiagnostic = error.what();
    }
    CHECK_EQ(
        nodesDiagnostic,
        "Invalid configuration value 'density_graph.nodes' in "
        "'WorldGenerator configuration': must contain no more than 32 entries");

    WorldGenConfig inputsConfig = makeFlatConfig();
    inputsConfig.densityGraph.nodes.push_back({
        .id = "bounded",
        .type = "add",
        .inputs = std::vector<std::string>(
            WorldGenConfig::MaxDensityNodeInputs + 1, "bounded")
    });
    std::string inputsDiagnostic;
    try {
        WorldGenerator generator(registry, inputsConfig);
    } catch (const std::invalid_argument& error) {
        inputsDiagnostic = error.what();
    }
    CHECK_EQ(
        inputsDiagnostic,
        "Invalid configuration value 'density_graph.nodes[0].inputs' in "
        "'WorldGenerator configuration': must contain no more than 8 entries");

    WorldGenConfig splineConfig = makeFlatConfig();
    splineConfig.densityGraph.nodes.push_back({
        .id = "bounded",
        .type = "spline",
        .splinePoints = std::vector<std::pair<float, float>>(
            WorldGenConfig::MaxDensitySplinePoints + 1, {0.0f, 0.0f})
    });
    std::string splineDiagnostic;
    try {
        WorldGenerator generator(registry, splineConfig);
    } catch (const std::invalid_argument& error) {
        splineDiagnostic = error.what();
    }
    CHECK_EQ(
        splineDiagnostic,
        "Invalid configuration value 'density_graph.nodes[0].spline' in "
        "'WorldGenerator configuration': must contain no more than 16 entries");

    WorldGenConfig outputsConfig = makeFlatConfig();
    for (size_t output = 0; output <= WorldGenConfig::MaxDensityGraphOutputs;
         ++output) {
        outputsConfig.densityGraph.outputs[
            "output" + std::to_string(output)] = "node";
    }
    std::string outputsDiagnostic;
    try {
        WorldGenerator generator(registry, outputsConfig);
    } catch (const std::invalid_argument& error) {
        outputsDiagnostic = error.what();
    }
    CHECK_EQ(
        outputsDiagnostic,
        "Invalid configuration value 'density_graph.outputs' in "
        "'WorldGenerator configuration': must contain no more than 8 entries");
}

TEST_CASE(WorldGenerator_EvaluatesDensityGraphFanoutBoundary) {
    BlockRegistry registry = makeRegistry();
    WorldGenConfig config = makeFlatConfig();
    config.stageEnabled["climate_global"] = false;
    config.stageEnabled["climate_local"] = false;
    config.stageEnabled["biome_resolve"] = false;
    config.stageEnabled["caves"] = false;
    config.stageEnabled["surface_rules"] = false;
    config.stageEnabled["structures"] = false;

    for (size_t index = 0; index < WorldGenConfig::MaxDensityGraphNodes;
         ++index) {
        WorldGenConfig::DensityNodeConfig node;
        node.id = "node" + std::to_string(index);
        node.type = "constant";
        node.value = index == 7 ? 15.0f : 0.0f;
        config.densityGraph.nodes.push_back(std::move(node));
    }
    auto& aggregate = config.densityGraph.nodes[30];
    aggregate.type = "add";
    aggregate.inputs.clear();
    for (size_t input = 0; input < WorldGenConfig::MaxDensityNodeInputs;
         ++input) {
        aggregate.inputs.push_back("node" + std::to_string(input));
    }
    auto& spline = config.densityGraph.nodes[31];
    spline.type = "spline";
    spline.inputs = {"node30"};
    spline.splinePoints.clear();
    for (size_t point = 0; point < WorldGenConfig::MaxDensitySplinePoints;
         ++point) {
        spline.splinePoints.emplace_back(
            static_cast<float>(point),
            point + 1 == WorldGenConfig::MaxDensitySplinePoints ? 1.0f : -1.0f);
    }
    config.densityGraph.outputs["base_density"] = "node31";

    WorldGenerator generator(registry, config);
    ChunkBuffer buffer;
    generator.generate({0, 0, 0}, buffer);
    CHECK_EQ(
        buffer.at(0, 0, 0).id.type,
        registry.findByIdentifier("rigel:stone")->type);
}

TEST_CASE(WorldGenerator_RejectsCyclicDensityGraphs) {
    BlockRegistry registry = makeRegistry();

    WorldGenConfig selfCycle = makeFlatConfig();
    selfCycle.densityGraph.nodes.push_back({
        .id = "self",
        .type = "add",
        .inputs = {"self"}
    });
    std::string selfDiagnostic;
    try {
        WorldGenerator generator(registry, selfCycle);
    } catch (const std::runtime_error& error) {
        selfDiagnostic = error.what();
    }
    CHECK_EQ(
        selfDiagnostic,
        "WorldGenerator: invalid density graph: "
        "Density graph cycle detected at node: self");

    WorldGenConfig twoNodeCycle = makeFlatConfig();
    twoNodeCycle.densityGraph.nodes = {
        WorldGenConfig::DensityNodeConfig{
            .id = "first", .type = "add", .inputs = {"second"}},
        WorldGenConfig::DensityNodeConfig{
            .id = "second", .type = "add", .inputs = {"first"}}
    };
    std::string twoNodeDiagnostic;
    try {
        WorldGenerator generator(registry, twoNodeCycle);
    } catch (const std::runtime_error& error) {
        twoNodeDiagnostic = error.what();
    }
    CHECK_EQ(
        twoNodeDiagnostic,
        "WorldGenerator: invalid density graph: "
        "Density graph cycle detected at node: first");
}

TEST_CASE(WorldGenerator_AcceptsMaximumDepthAcyclicDensityGraph) {
    BlockRegistry registry = makeRegistry();
    WorldGenConfig config = makeFlatConfig();
    config.stageEnabled["climate_global"] = false;
    config.stageEnabled["climate_local"] = false;
    config.stageEnabled["biome_resolve"] = false;
    config.stageEnabled["caves"] = false;
    config.stageEnabled["surface_rules"] = false;
    config.stageEnabled["structures"] = false;

    for (size_t index = 0; index < WorldGenConfig::MaxDensityGraphNodes;
         ++index) {
        WorldGenConfig::DensityNodeConfig node;
        node.id = "chain" + std::to_string(index);
        node.type = index == 0 ? "constant" : "add";
        node.value = 1.0f;
        if (index != 0) {
            node.inputs.push_back("chain" + std::to_string(index - 1));
        }
        config.densityGraph.nodes.push_back(std::move(node));
    }
    config.densityGraph.outputs["base_density"] =
        "chain" + std::to_string(WorldGenConfig::MaxDensityGraphNodes - 1);

    WorldGenerator generator(registry, config);
    ChunkBuffer buffer;
    generator.generate({0, 0, 0}, buffer);
    CHECK_EQ(
        buffer.at(0, 0, 0).id.type,
        registry.findByIdentifier("rigel:stone")->type);
}
