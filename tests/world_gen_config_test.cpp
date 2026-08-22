#include "TestFramework.h"

#include "Rigel/Voxel/WorldGenConfig.h"

#include <functional>
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
    });
    CHECK_EQ(
        orderingError,
        "Invalid configuration value 'world.max_y' in 'constraints.yaml': "
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
    });
    CHECK_EQ(
        heightError,
        "Invalid configuration value 'world.max_y' in 'constraints.yaml': "
        "inclusive world height must not exceed 1024"
    );

    const std::string seaError = exceptionMessage([] {
        WorldGenConfig config;
        config.applyYaml(
            "constraints.yaml",
            "world:\n"
            "  min_y: 0\n"
            "  max_y: 10\n"
            "  sea_level: 11\n"
        );
    });
    CHECK_EQ(
        seaError,
        "Invalid configuration value 'world.sea_level' in "
        "'constraints.yaml': must be between 'world.min_y' and 'world.max_y'"
    );
}
