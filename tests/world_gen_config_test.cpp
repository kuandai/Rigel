#include "TestFramework.h"

#include "Rigel/Voxel/WorldGenConfig.h"

using namespace Rigel::Voxel;

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
  lava_level: -16
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
  elevation_lapse: 0.02
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
  enabled: true
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
streaming:
  view_distance_chunks: 3
  unload_distance_chunks: 5
  gen_queue_limit: 4
  mesh_queue_limit: 6
  apply_budget_per_frame: 9
  worker_threads: 0
  max_resident_chunks: 100
persistence:
  cr:
    lz4: true
generation:
  pipeline:
    - stage: climate_global
      enabled: true
    - stage: climate_local
      enabled: true
    - stage: biome_resolve
      enabled: true
    - stage: terrain_density
      enabled: false
    - stage: caves
      enabled: true
    - stage: surface_rules
      enabled: true
    - stage: structures
      enabled: true
    - stage: post_process
      enabled: true
)";

    config.applyYaml("test", yaml);

    CHECK_EQ(config.seed, static_cast<uint32_t>(42));
    CHECK_EQ(config.solidBlock, "base:stone_shale");
    CHECK_EQ(config.surfaceBlock, "base:grass");
    CHECK_EQ(config.world.minY, -32);
    CHECK_EQ(config.world.maxY, 128);
    CHECK_EQ(config.world.seaLevel, 8);
    CHECK_EQ(config.world.lavaLevel, -16);
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
    CHECK_EQ(config.stream.viewDistanceChunks, 3);
    CHECK_EQ(config.stream.genQueueLimit, static_cast<size_t>(4));
    CHECK_EQ(config.stream.meshQueueLimit, static_cast<size_t>(6));
    CHECK_EQ(config.stream.applyBudgetPerFrame, 9);
    CHECK_EQ(config.stream.workerThreads, 0);
    CHECK_EQ(config.stream.maxResidentChunks, static_cast<size_t>(100));
    CHECK(config.persistence.cr.lz4);
    CHECK(!config.isStageEnabled("terrain_density"));
}
