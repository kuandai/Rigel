#include "TestFramework.h"

#include "Rigel/Voxel/WorldGenConfig.h"

#include <functional>
#include <sstream>
#include <stdexcept>

using namespace Rigel::Voxel;

namespace {

std::string exceptionMessage(const std::function<void()>& operation) {
    try {
        operation();
    } catch (const std::invalid_argument& error) {
        return error.what();
    }
    throw Rigel::Test::TestFailure("Expected invalid configuration");
}

} // namespace

TEST_CASE(WorldGenConfig_ApplyYaml) {
    WorldGenConfig config;
    std::string yaml = R"(
seed: 42
solid_block: base:stone_shale
surface_block: base:grass
world:
  min_y: -32
  max_y: 128
  sea_level: 8
  version: 7
flags:
  no_carvers: true
terrain:
  base_height: 5.0
  height_variation: 0.0
  surface_depth: 2
  noise:
    octaves: 2
    frequency: 0.1
    lacunarity: 2.5
    persistence: 0.4
    scale: 0.9
    offset: 0.1
  density_noise:
    octaves: 1
    frequency: 0.2
    lacunarity: 2.0
    persistence: 0.5
    scale: 1.0
    offset: 0.0
  density_strength: 3.0
  gradient_strength: 0.8
climate:
  latitude_scale: 0.001
  latitude_strength: 0.5
  local_blend: 0.25
  global:
    temperature:
      octaves: 1
      frequency: 0.0001
      lacunarity: 2.0
      persistence: 0.5
      scale: 1.0
      offset: 0.0
  local:
    temperature:
      octaves: 1
      frequency: 0.01
      lacunarity: 2.0
      persistence: 0.5
      scale: 1.0
      offset: 0.0
biomes:
  blend_power: 3.0
  epsilon: 0.001
  coast_band:
    biome: beach
    min_continentalness: -0.2
    max_continentalness: -0.05
  entries:
    - name: plains
      target:
        temperature: 0.1
        humidity: 0.2
        continentalness: 0.3
      surface:
        - block: base:grass
          depth: 1
density_graph:
  outputs:
    base_density: base_density
  nodes:
    - id: base_density
      type: constant
      value: 0.5
caves:
  density_output: cave_density
  threshold: 0.2
structures:
  features:
    - name: boulders
      block: base:stone_shale
      chance: 0.1
      min_height: 2
      max_height: 3
      biomes: [plains]
overlays:
  - path: assets/config/worldgen_overlays/no_carvers.yaml
    when: no_carvers
generation:
  stages:
    surface_rules: true
    terrain_density: false
)";

    config.applyYaml("test", yaml);

    CHECK_EQ(config.seed, static_cast<uint32_t>(42));
    CHECK_EQ(config.solidBlock, "base:stone_shale");
    CHECK_EQ(config.surfaceBlock, "base:grass");
    CHECK_EQ(config.world.minY, -32);
    CHECK_EQ(config.world.maxY, 128);
    CHECK_EQ(config.world.seaLevel, 8);
    CHECK_EQ(config.world.version, static_cast<uint32_t>(7));
    CHECK(config.isFlagEnabled("no_carvers"));
    CHECK_NEAR(config.terrain.baseHeight, 5.0f, 0.001f);
    CHECK_EQ(config.terrain.surfaceDepth, 2);
    CHECK_NEAR(config.terrain.densityStrength, 3.0f, 0.001f);
    CHECK_NEAR(config.climate.latitudeStrength, 0.5f, 0.001f);
    CHECK_EQ(config.biomes.entries.size(), static_cast<size_t>(1));
    CHECK_EQ(config.biomes.entries[0].name, "plains");
    CHECK(config.biomes.coastBand.enabled);
    CHECK_EQ(config.biomes.coastBand.biome, "beach");
    CHECK_EQ(config.densityGraph.nodes.size(), static_cast<size_t>(1));
    CHECK_EQ(config.caves.densityOutput, "cave_density");
    CHECK_EQ(config.structures.features.size(), static_cast<size_t>(1));
    CHECK_EQ(config.overlays.size(), static_cast<size_t>(1));
    CHECK(!config.isStageEnabled("terrain_density"));
    CHECK(config.isStageEnabled("surface_rules"));
    CHECK(config.isStageEnabled("caves"));
}

TEST_CASE(WorldGenConfig_LayeredMergeSemantics) {
    WorldGenConfig config;
    const std::string base = R"(
seed: 7
terrain:
  base_height: 11.0
  noise:
    frequency: 0.2
biomes:
  entries:
    - name: plains
    - name: forest
density_graph:
  outputs:
    terrain: old_terrain
    retained: retained_node
  nodes:
    - id: terrain
      type: constant
      value: 1.0
      offset: 7.0
    - id: retained_node
      type: constant
      value: 2.0
structures:
  features:
    - name: boulders
      block: base:stone_shale
    - name: shrubs
      block: base:grass
generation:
  stages:
    terrain_density: false
flags:
  retained: true
  changed: false
overlays:
  - path: first.yaml
  - path: second.yaml
)";
    const std::string higherPrecedence = R"(
seed: 9
terrain:
  density_strength: 3.0
  noise:
    octaves: 2
biomes:
  entries:
    - name: tundra
density_graph:
  outputs:
    terrain: new_terrain
    added: added_node
  nodes:
    - id: terrain
      type: constant
      value: 10.0
structures:
  features:
    - name: crystals
      block: base:stone_shale
generation:
  stages:
    terrain_density: true
    caves: false
flags:
  changed: true
  added: true
overlays:
  - path: world.yaml
)";

    config.applyYaml("base", base);
    config.applyYaml("world", higherPrecedence);

    CHECK_EQ(config.seed, static_cast<uint32_t>(9));
    CHECK_NEAR(config.terrain.baseHeight, 11.0f, 0.001f);
    CHECK_NEAR(config.terrain.densityStrength, 3.0f, 0.001f);
    CHECK_EQ(config.terrain.heightNoise.octaves, 2);
    CHECK_NEAR(config.terrain.heightNoise.frequency, 0.2f, 0.001f);

    CHECK_EQ(config.biomes.entries.size(), static_cast<size_t>(1));
    CHECK_EQ(config.biomes.entries[0].name, "tundra");
    CHECK_EQ(config.structures.features.size(), static_cast<size_t>(1));
    CHECK_EQ(config.structures.features[0].name, "crystals");
    CHECK_EQ(config.overlays.size(), static_cast<size_t>(1));
    CHECK_EQ(config.overlays[0].path, "world.yaml");

    CHECK_EQ(config.densityGraph.outputs.size(), static_cast<size_t>(3));
    CHECK_EQ(config.densityGraph.outputs.at("terrain"), "new_terrain");
    CHECK_EQ(config.densityGraph.outputs.at("retained"), "retained_node");
    CHECK_EQ(config.densityGraph.outputs.at("added"), "added_node");
    CHECK_EQ(config.densityGraph.nodes.size(), static_cast<size_t>(2));
    CHECK_EQ(config.densityGraph.nodes[0].id, "terrain");
    CHECK_NEAR(config.densityGraph.nodes[0].value, 10.0f, 0.001f);
    CHECK_NEAR(config.densityGraph.nodes[0].offset, 0.0f, 0.001f);
    CHECK_EQ(config.densityGraph.nodes[1].id, "retained_node");

    CHECK(config.isFlagEnabled("retained"));
    CHECK(config.isFlagEnabled("changed"));
    CHECK(config.isFlagEnabled("added"));
    CHECK(config.isStageEnabled("terrain_density"));
    CHECK(!config.isStageEnabled("caves"));

    config.applyYaml(
        "clear",
        "biomes:\n"
        "  entries: []\n"
        "structures:\n"
        "  features: []\n"
        "overlays: []\n"
    );
    CHECK(config.biomes.entries.empty());
    CHECK(config.structures.features.empty());
    CHECK(config.overlays.empty());
}

TEST_CASE(WorldGenConfig_AcceptsWorldAndOctaveMaxima) {
    WorldGenConfig config;
    config.applyYaml(
        "limits.yaml",
        "world:\n"
        "  min_y: 3073\n"
        "  max_y: 4096\n"
        "  sea_level: 4096\n"
        "terrain:\n"
        "  noise:\n"
        "    octaves: 16\n"
        "density_graph:\n"
        "  nodes:\n"
        "    - id: bounded_noise\n"
        "      type: noise3d\n"
        "      noise:\n"
        "        octaves: 16\n"
    );
    config.validate("merged test configuration");

    CHECK_EQ(config.world.maxY, WorldGenConfig::MaxWorldY);
    CHECK_EQ(config.world.maxY - config.world.minY + 1,
             WorldGenConfig::MaxWorldHeight);
    CHECK_EQ(config.terrain.heightNoise.octaves,
             WorldGenConfig::MaxNoiseOctaves);
    CHECK_EQ(config.densityGraph.nodes[0].noise.octaves,
             WorldGenConfig::MaxNoiseOctaves);
}

TEST_CASE(WorldGenConfig_RejectsWorldAndOctaveValuesAboveMaxima) {
    const std::string worldError = exceptionMessage([] {
        WorldGenConfig config;
        config.applyYaml(
            "limits.yaml",
            "world:\n"
            "  min_y: 4096\n"
            "  max_y: 4097\n"
            "  sea_level: 4096\n"
        );
    });
    CHECK_EQ(
        worldError,
        "Invalid configuration value 'world.max_y' in 'limits.yaml': "
        "expected integer in [-4096, 4096], got '4097'"
    );

    const std::string octaveError = exceptionMessage([] {
        WorldGenConfig config;
        config.applyYaml(
            "limits.yaml",
            "terrain:\n  noise:\n    octaves: 17\n"
        );
    });
    CHECK_EQ(
        octaveError,
        "Invalid configuration value 'terrain.noise.octaves' in "
        "'limits.yaml': expected integer no greater than 16, got '17'"
    );

    const std::string integerError = exceptionMessage([] {
        WorldGenConfig config;
        config.applyYaml(
            "limits.yaml",
            "world:\n  max_y: 2147483647\n"
        );
    });
    CHECK_EQ(
        integerError,
        "Invalid configuration value 'world.max_y' in 'limits.yaml': "
        "expected integer in [-4096, 4096], got '2147483647'"
    );
}

TEST_CASE(WorldGenConfig_RejectsWorldCrossFieldViolations) {
    const std::string orderingError = exceptionMessage([] {
        WorldGenConfig config;
        config.applyYaml(
            "constraints.yaml",
            "world:\n"
            "  min_y: 10\n"
            "  max_y: 9\n"
            "  sea_level: 9\n"
        );
        config.validate("merged test configuration");
    });
    CHECK_EQ(
        orderingError,
        "Invalid configuration value 'world.max_y' in "
        "'merged test configuration': "
        "must be greater than or equal to 'world.min_y'"
    );

    const std::string heightError = exceptionMessage([] {
        WorldGenConfig config;
        config.applyYaml(
            "constraints.yaml",
            "world:\n"
            "  min_y: 0\n"
            "  max_y: 1024\n"
            "  sea_level: 0\n"
        );
        config.validate("merged test configuration");
    });
    CHECK_EQ(
        heightError,
        "Invalid configuration value 'world.max_y' in "
        "'merged test configuration': "
        "inclusive world height must not exceed 1024"
    );
}

TEST_CASE(WorldGenConfig_ValidatesBoundsAfterLayeredMerge) {
    WorldGenConfig config;
    config.applyYaml("base.yaml", "world:\n  min_y: 400\n");
    config.applyYaml("override.yaml", "world:\n  max_y: 500\n");
    config.validate("merged test configuration");

    CHECK_EQ(config.world.minY, 400);
    CHECK_EQ(config.world.maxY, 500);
}

TEST_CASE(WorldGenConfig_AcceptsSeaLevelOutsideWorldBounds) {
    WorldGenConfig config;
    config.applyYaml(
        "above.yaml",
        "world:\n  min_y: 0\n  max_y: 10\n  sea_level: 11\n"
    );
    config.validate("merged test configuration");
    CHECK_EQ(config.world.seaLevel, 11);

    config.applyYaml("below.yaml", "world:\n  sea_level: -1\n");
    config.validate("merged test configuration");
    CHECK_EQ(config.world.seaLevel, -1);
}

TEST_CASE(WorldGenConfig_AcceptsGenerationLoopMaxima) {
    WorldGenConfig config;
    config.applyYaml(
        "loop-limits.yaml",
        "terrain:\n"
        "  surface_depth: 32\n"
        "biomes:\n"
        "  entries:\n"
        "    - name: bounded\n"
        "      surface:\n"
        "        - block: base:stone_shale\n"
        "          depth: 32\n"
        "structures:\n"
        "  features:\n"
        "    - name: bounded\n"
        "      block: base:stone_shale\n"
        "      min_height: 1024\n"
        "      max_height: 1024\n"
    );

    CHECK_EQ(config.terrain.surfaceDepth, WorldGenConfig::MaxSurfaceDepth);
    CHECK_EQ(config.biomes.entries[0].surface[0].depth,
             WorldGenConfig::MaxSurfaceDepth);
    CHECK_EQ(config.structures.features[0].minHeight,
             WorldGenConfig::MaxStructureHeight);
    CHECK_EQ(config.structures.features[0].maxHeight,
             WorldGenConfig::MaxStructureHeight);
}

TEST_CASE(WorldGenConfig_RejectsGenerationLoopValuesAboveMaxima) {
    const std::string terrainError = exceptionMessage([] {
        WorldGenConfig config;
        config.applyYaml(
            "loop-limits.yaml",
            "terrain:\n  surface_depth: 33\n"
        );
    });
    CHECK_EQ(
        terrainError,
        "Invalid configuration value 'terrain.surface_depth' in "
        "'loop-limits.yaml': expected integer no greater than 32, got '33'"
    );

    const std::string terrainIntegerError = exceptionMessage([] {
        WorldGenConfig config;
        config.applyYaml(
            "loop-limits.yaml",
            "terrain:\n  surface_depth: 2147483647\n"
        );
    });
    CHECK_EQ(
        terrainIntegerError,
        "Invalid configuration value 'terrain.surface_depth' in "
        "'loop-limits.yaml': expected integer no greater than 32, got "
        "'2147483647'"
    );

    const std::string terrainUnsignedError = exceptionMessage([] {
        WorldGenConfig config;
        config.applyYaml(
            "loop-limits.yaml",
            "terrain:\n  surface_depth: 4294967295\n"
        );
    });
    CHECK_EQ(
        terrainUnsignedError,
        "Invalid configuration value 'terrain.surface_depth' in "
        "'loop-limits.yaml': expected integer no greater than 32, got "
        "'4294967295'"
    );

    const std::string biomeMaxError = exceptionMessage([] {
        WorldGenConfig config;
        config.applyYaml(
            "loop-limits.yaml",
            "biomes:\n"
            "  entries:\n"
            "    - name: bounded\n"
            "      surface:\n"
            "        - block: base:stone_shale\n"
            "          depth: 33\n"
        );
    });
    CHECK_EQ(
        biomeMaxError,
        "Invalid configuration value 'biomes.entries[0].surface[0].depth' "
        "in 'loop-limits.yaml': expected integer no greater than 32, got '33'"
    );

    const std::string biomeError = exceptionMessage([] {
        WorldGenConfig config;
        config.applyYaml(
            "loop-limits.yaml",
            "biomes:\n"
            "  entries:\n"
            "    - name: bounded\n"
            "      surface:\n"
            "        - block: base:stone_shale\n"
            "          depth: 2147483647\n"
        );
    });
    CHECK_EQ(
        biomeError,
        "Invalid configuration value 'biomes.entries[0].surface[0].depth' "
        "in 'loop-limits.yaml': expected integer no greater than 32, got "
        "'2147483647'"
    );

    const std::string biomeUnsignedError = exceptionMessage([] {
        WorldGenConfig config;
        config.applyYaml(
            "loop-limits.yaml",
            "biomes:\n"
            "  entries:\n"
            "    - name: bounded\n"
            "      surface:\n"
            "        - block: base:stone_shale\n"
            "          depth: 4294967295\n"
        );
    });
    CHECK_EQ(
        biomeUnsignedError,
        "Invalid configuration value 'biomes.entries[0].surface[0].depth' "
        "in 'loop-limits.yaml': expected integer no greater than 32, got "
        "'4294967295'"
    );

    const std::string structureMaxError = exceptionMessage([] {
        WorldGenConfig config;
        config.applyYaml(
            "loop-limits.yaml",
            "structures:\n"
            "  features:\n"
            "    - name: bounded\n"
            "      block: base:stone_shale\n"
            "      min_height: 1025\n"
        );
    });
    CHECK_EQ(
        structureMaxError,
        "Invalid configuration value 'structures.features[0].min_height' "
        "in 'loop-limits.yaml': expected integer no greater than 1024, got "
        "'1025'"
    );

    const std::string structureIntegerError = exceptionMessage([] {
        WorldGenConfig config;
        config.applyYaml(
            "loop-limits.yaml",
            "structures:\n"
            "  features:\n"
            "    - name: bounded\n"
            "      block: base:stone_shale\n"
            "      min_height: 2147483647\n"
        );
    });
    CHECK_EQ(
        structureIntegerError,
        "Invalid configuration value 'structures.features[0].min_height' "
        "in 'loop-limits.yaml': expected integer no greater than 1024, got "
        "'2147483647'"
    );

    const std::string structureError = exceptionMessage([] {
        WorldGenConfig config;
        config.applyYaml(
            "loop-limits.yaml",
            "structures:\n"
            "  features:\n"
            "    - name: bounded\n"
            "      block: base:stone_shale\n"
            "      min_height: 0\n"
            "      max_height: 4294967295\n"
        );
    });
    CHECK_EQ(
        structureError,
        "Invalid configuration value 'structures.features[0].max_height' "
        "in 'loop-limits.yaml': expected integer no greater than 1024, got "
        "'4294967295'"
    );
}

TEST_CASE(WorldGenConfig_AcceptsGeneratorListMaxima) {
    std::ostringstream yaml;
    yaml << "biomes:\n  entries:\n";
    for (size_t biome = 0; biome < WorldGenConfig::MaxBiomeEntries; ++biome) {
        yaml << "    - name: biome" << biome << "\n";
        if (biome == 0) {
            yaml << "      surface:\n";
            for (size_t layer = 0; layer < WorldGenConfig::MaxSurfaceLayers;
                 ++layer) {
                yaml << "        - block: base:stone_shale\n"
                     << "          depth: 1\n";
            }
        }
    }
    yaml << "structures:\n  features:\n";
    for (size_t feature = 0; feature < WorldGenConfig::MaxStructureFeatures;
         ++feature) {
        yaml << "    - name: feature" << feature << "\n"
             << "      block: base:stone_shale\n";
        if (feature == 0) {
            yaml << "      biomes:\n";
            for (size_t filter = 0;
                 filter < WorldGenConfig::MaxFeatureBiomeFilters; ++filter) {
                yaml << "        - biome" << filter << "\n";
            }
        }
    }

    WorldGenConfig config;
    config.applyYaml("list-limits.yaml", yaml.str());
    CHECK_EQ(config.biomes.entries.size(), WorldGenConfig::MaxBiomeEntries);
    CHECK_EQ(config.biomes.entries[0].surface.size(),
             WorldGenConfig::MaxSurfaceLayers);
    CHECK_EQ(config.structures.features.size(),
             WorldGenConfig::MaxStructureFeatures);
    CHECK_EQ(config.structures.features[0].biomes.size(),
             WorldGenConfig::MaxFeatureBiomeFilters);
}

TEST_CASE(WorldGenConfig_RejectsGeneratorListsAboveMaxima) {
    const std::string biomeError = exceptionMessage([] {
        std::ostringstream yaml;
        yaml << "biomes:\n  entries:\n";
        for (size_t i = 0; i <= WorldGenConfig::MaxBiomeEntries; ++i) {
            yaml << "    - name: biome" << i << "\n";
        }
        WorldGenConfig config;
        config.applyYaml("list-limits.yaml", yaml.str());
    });
    CHECK_EQ(
        biomeError,
        "Invalid configuration value 'biomes.entries' in "
        "'list-limits.yaml': must contain no more than 32 entries"
    );

    const std::string layerError = exceptionMessage([] {
        std::ostringstream yaml;
        yaml << "biomes:\n  entries:\n    - name: bounded\n"
             << "      surface:\n";
        for (size_t i = 0; i <= WorldGenConfig::MaxSurfaceLayers; ++i) {
            yaml << "        - block: base:stone_shale\n"
                 << "          depth: 0\n";
        }
        WorldGenConfig config;
        config.applyYaml("list-limits.yaml", yaml.str());
    });
    CHECK_EQ(
        layerError,
        "Invalid configuration value 'biomes.entries[0].surface' in "
        "'list-limits.yaml': must contain no more than 32 entries"
    );

    const std::string featureError = exceptionMessage([] {
        std::ostringstream yaml;
        yaml << "structures:\n  features:\n";
        for (size_t i = 0; i <= WorldGenConfig::MaxStructureFeatures; ++i) {
            yaml << "    - name: feature" << i << "\n"
                 << "      block: base:stone_shale\n";
        }
        WorldGenConfig config;
        config.applyYaml("list-limits.yaml", yaml.str());
    });
    CHECK_EQ(
        featureError,
        "Invalid configuration value 'structures.features' in "
        "'list-limits.yaml': must contain no more than 16 entries"
    );

    const std::string filterError = exceptionMessage([] {
        std::ostringstream yaml;
        yaml << "structures:\n  features:\n"
             << "    - name: bounded\n"
             << "      block: base:stone_shale\n"
             << "      biomes:\n";
        for (size_t i = 0; i <= WorldGenConfig::MaxFeatureBiomeFilters; ++i) {
            yaml << "        - biome" << i << "\n";
        }
        WorldGenConfig config;
        config.applyYaml("list-limits.yaml", yaml.str());
    });
    CHECK_EQ(
        filterError,
        "Invalid configuration value 'structures.features[0].biomes' in "
        "'list-limits.yaml': must contain no more than 32 entries"
    );
}

TEST_CASE(WorldGenConfig_RejectsExcessCumulativeBiomeDepth) {
    const std::string error = exceptionMessage([] {
        WorldGenConfig config;
        config.applyYaml(
            "loop-limits.yaml",
            "biomes:\n"
            "  entries:\n"
            "    - name: bounded\n"
            "      surface:\n"
            "        - block: base:stone_shale\n"
            "          depth: 17\n"
            "        - block: base:stone_shale\n"
            "          depth: 16\n"
        );
    });
    CHECK_EQ(
        error,
        "Invalid configuration value 'biomes.entries[0].surface[1].depth' "
        "in 'loop-limits.yaml': cumulative biome surface depth must not exceed "
        "32"
    );
}

TEST_CASE(WorldGenConfig_NegativeBiomeDepthCannotHidePositiveWork) {
    const std::string error = exceptionMessage([] {
        WorldGenConfig config;
        config.applyYaml(
            "loop-limits.yaml",
            "biomes:\n"
            "  entries:\n"
            "    - name: bounded\n"
            "      surface:\n"
            "        - block: base:stone_shale\n"
            "          depth: -2147483648\n"
            "        - block: base:stone_shale\n"
            "          depth: 32\n"
            "        - block: base:stone_shale\n"
            "          depth: 1\n"
        );
    });
    CHECK_EQ(
        error,
        "Invalid configuration value 'biomes.entries[0].surface[2].depth' "
        "in 'loop-limits.yaml': cumulative biome surface depth must not exceed "
        "32"
    );
}

TEST_CASE(WorldGenConfig_PreservesNegativeLoopSemantics) {
    WorldGenConfig config;
    config.applyYaml(
        "negative.yaml",
        "terrain:\n"
        "  surface_depth: -1\n"
        "biomes:\n"
        "  entries:\n"
        "    - name: bounded\n"
        "      surface:\n"
        "        - block: base:stone_shale\n"
        "          depth: -2\n"
        "structures:\n"
        "  features:\n"
        "    - name: bounded\n"
        "      block: base:stone_shale\n"
        "      min_height: -3\n"
        "      max_height: 5\n"
    );

    CHECK_EQ(config.terrain.surfaceDepth, -1);
    CHECK_EQ(config.biomes.entries[0].surface[0].depth, -2);
    CHECK_EQ(config.structures.features[0].minHeight, -3);
    CHECK_EQ(config.structures.features[0].maxHeight, 5);
}

TEST_CASE(WorldGenConfig_IgnoresUnretainedGeneratorListEntries) {
    std::ostringstream yaml;
    yaml << "biomes:\n  entries:\n";
    for (size_t i = 0; i <= WorldGenConfig::MaxBiomeEntries; ++i) {
        yaml << "    - name: ''\n"
             << "      surface:\n"
             << "        - block: ''\n"
             << "          depth: 4294967295\n";
    }
    yaml << "structures:\n  features:\n";
    for (size_t i = 0; i <= WorldGenConfig::MaxStructureFeatures; ++i) {
        yaml << "    - name: inert" << i << "\n"
             << "      block: ''\n"
             << "      max_height: 4294967295\n";
    }

    WorldGenConfig config;
    config.applyYaml("inert.yaml", yaml.str());
    CHECK(config.biomes.entries.empty());
    CHECK(config.structures.features.empty());
}

TEST_CASE(WorldGenConfig_AcceptsDensityGraphFanoutMaximaAndReplacement) {
    std::ostringstream yaml;
    yaml << "density_graph:\n  nodes:\n";
    for (size_t node = 0; node < WorldGenConfig::MaxDensityGraphNodes; ++node) {
        yaml << "    - id: node" << node << "\n"
             << "      type: " << (node == 0 ? "spline" : "constant") << "\n";
        if (node == 0) {
            yaml << "      inputs:\n";
            for (size_t input = 0; input < WorldGenConfig::MaxDensityNodeInputs;
                 ++input) {
                yaml << "        - node" << input + 1 << "\n";
            }
            yaml << "        - ''\n";
            yaml << "      spline:\n";
            for (size_t point = 0; point < WorldGenConfig::MaxDensitySplinePoints;
                 ++point) {
                yaml << "        - [" << point << ", " << point << "]\n";
            }
        }
    }

    WorldGenConfig config;
    config.applyYaml("density-limits.yaml", yaml.str());
    CHECK_EQ(config.densityGraph.nodes.size(),
             WorldGenConfig::MaxDensityGraphNodes);
    CHECK_EQ(config.densityGraph.nodes[0].inputs.size(),
             WorldGenConfig::MaxDensityNodeInputs);
    CHECK_EQ(config.densityGraph.nodes[0].splinePoints.size(),
             WorldGenConfig::MaxDensitySplinePoints);

    config.applyYaml(
        "density-overlay.yaml",
        "density_graph:\n"
        "  nodes:\n"
        "    - id: node0\n"
        "      type: constant\n"
        "      value: 7\n");
    CHECK_EQ(config.densityGraph.nodes.size(),
             WorldGenConfig::MaxDensityGraphNodes);
    CHECK_EQ(config.densityGraph.nodes[0].type, "constant");
    CHECK_NEAR(config.densityGraph.nodes[0].value, 7.0f, 0.001f);
    CHECK(config.densityGraph.nodes[0].inputs.empty());
    CHECK(config.densityGraph.nodes[0].splinePoints.empty());
}

TEST_CASE(WorldGenConfig_RejectsDensityGraphFanoutAboveMaxima) {
    const std::string nodeError = exceptionMessage([] {
        std::ostringstream yaml;
        yaml << "density_graph:\n  nodes:\n";
        for (size_t node = 0; node <= WorldGenConfig::MaxDensityGraphNodes;
             ++node) {
            yaml << "    - id: node" << node << "\n"
                 << "      type: constant\n";
        }
        WorldGenConfig config;
        config.applyYaml("density-limits.yaml", yaml.str());
    });
    CHECK_EQ(
        nodeError,
        "Invalid configuration value 'density_graph.nodes[32]' in "
        "'density-limits.yaml': must contain no more than 32 entries");

    const std::string inputError = exceptionMessage([] {
        std::ostringstream yaml;
        yaml << "density_graph:\n  nodes:\n"
             << "    - id: bounded\n"
             << "      type: add\n"
             << "      inputs:\n";
        for (size_t input = 0; input <= WorldGenConfig::MaxDensityNodeInputs;
             ++input) {
            yaml << "        - input" << input << "\n";
        }
        WorldGenConfig config;
        config.applyYaml("density-limits.yaml", yaml.str());
    });
    CHECK_EQ(
        inputError,
        "Invalid configuration value 'density_graph.nodes[0].inputs' in "
        "'density-limits.yaml': must contain no more than 8 entries");

    const std::string splineError = exceptionMessage([] {
        std::ostringstream yaml;
        yaml << "density_graph:\n  nodes:\n"
             << "    - id: bounded\n"
             << "      type: spline\n"
             << "      spline:\n";
        for (size_t point = 0; point <= WorldGenConfig::MaxDensitySplinePoints;
             ++point) {
            yaml << "        - [" << point << ", " << point << "]\n";
        }
        WorldGenConfig config;
        config.applyYaml("density-limits.yaml", yaml.str());
    });
    CHECK_EQ(
        splineError,
        "Invalid configuration value 'density_graph.nodes[0].spline' in "
        "'density-limits.yaml': must contain no more than 16 entries");
}

TEST_CASE(WorldGenConfig_IgnoresUnretainedDensityGraphFanout) {
    std::ostringstream yaml;
    yaml << "density_graph:\n  nodes:\n";
    for (size_t node = 0; node <= WorldGenConfig::MaxDensityGraphNodes; ++node) {
        yaml << "    - id: ''\n"
             << "      type: spline\n"
             << "      inputs:\n";
        for (size_t input = 0; input <= WorldGenConfig::MaxDensityNodeInputs;
             ++input) {
            yaml << "        - input" << input << "\n";
        }
        yaml << "      spline:\n";
        for (size_t point = 0; point <= WorldGenConfig::MaxDensitySplinePoints;
             ++point) {
            yaml << "        - [" << point << ", " << point << "]\n";
        }
    }

    WorldGenConfig config;
    config.applyYaml("inert-density.yaml", yaml.str());
    CHECK(config.densityGraph.nodes.empty());
}

TEST_CASE(WorldGenConfig_RejectsHostileDensityGraphFanoutAtFirstExcess) {
    constexpr size_t hostileCount = 1024;

    const std::string nodeError = exceptionMessage([] {
        std::ostringstream yaml;
        yaml << "density_graph:\n  nodes:\n";
        for (size_t node = 0; node < hostileCount; ++node) {
            yaml << "    - id: hostile" << node << "\n"
                 << "      type: constant\n";
        }
        WorldGenConfig config;
        config.applyYaml("hostile-density.yaml", yaml.str());
    });
    CHECK_EQ(
        nodeError,
        "Invalid configuration value 'density_graph.nodes[32]' in "
        "'hostile-density.yaml': must contain no more than 32 entries");

    const std::string inputError = exceptionMessage([] {
        std::ostringstream yaml;
        yaml << "density_graph:\n  nodes:\n"
             << "    - id: hostile\n"
             << "      type: add\n"
             << "      inputs:\n";
        for (size_t input = 0; input < hostileCount; ++input) {
            yaml << "        - input" << input << "\n";
        }
        WorldGenConfig config;
        config.applyYaml("hostile-density.yaml", yaml.str());
    });
    CHECK_EQ(
        inputError,
        "Invalid configuration value 'density_graph.nodes[0].inputs' in "
        "'hostile-density.yaml': must contain no more than 8 entries");

    const std::string splineError = exceptionMessage([] {
        std::ostringstream yaml;
        yaml << "density_graph:\n  nodes:\n"
             << "    - id: hostile\n"
             << "      type: spline\n"
             << "      spline:\n";
        for (size_t point = 0; point < hostileCount; ++point) {
            yaml << "        - [" << point << ", " << point << "]\n";
        }
        WorldGenConfig config;
        config.applyYaml("hostile-density.yaml", yaml.str());
    });
    CHECK_EQ(
        splineError,
        "Invalid configuration value 'density_graph.nodes[0].spline' in "
        "'hostile-density.yaml': must contain no more than 16 entries");
}
