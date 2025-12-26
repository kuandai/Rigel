#include "TestFramework.h"

#include "Rigel/Voxel/WorldGenConfig.h"

using namespace Rigel::Voxel;

TEST_CASE(WorldGenConfig_ApplyYaml) {
    WorldGenConfig config;
    std::string yaml = R"(
seed: 42
solid_block: rigel:stone
surface_block: rigel:grass
terrain:
  base_height: 5.0
  height_variation: 0.0
  surface_depth: 2
  noise:
    octaves: 2
    frequency: 0.1
    lacunarity: 2.5
    persistence: 0.4
streaming:
  view_distance_chunks: 3
  unload_distance_chunks: 5
  max_generate_per_frame: 0
  max_resident_chunks: 100
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
    CHECK_EQ(config.solidBlock, "rigel:stone");
    CHECK_EQ(config.surfaceBlock, "rigel:grass");
    CHECK_NEAR(config.terrain.baseHeight, 5.0f, 0.001f);
    CHECK_EQ(config.terrain.surfaceDepth, 2);
    CHECK_EQ(config.stream.viewDistanceChunks, 3);
    CHECK_EQ(config.stream.maxGeneratePerFrame, 0);
    CHECK_EQ(config.stream.maxResidentChunks, static_cast<size_t>(100));
    CHECK(!config.isStageEnabled("terrain_density"));
}
