#include "TestFramework.h"

#include "Rigel/Voxel/GeneratorDefinition.h"

#include <string>
#include <string_view>

namespace {

using Rigel::Voxel::GeneratorDefinition;
using Rigel::Voxel::parseGeneratorDefinition;
using Rigel::Voxel::parseGeneratorDefinitionSnapshot;
using Rigel::Voxel::serializeGeneratorDefinition;
using Rigel::Voxel::serializeGeneratorDefinitionSnapshot;

std::string validDefinitionYaml() {
    return R"yaml(generator:
  schema_version: 1
  id: test:continental
  source_revision: 7
  label: Continental
  description: A complete graph-only test definition.
  bounds:
    min_y: -64
    max_y: 319
  terrain:
    sea_level: 60
    solid_material: test:stone
    water_material: test:water
    density_output: terrain_density
  climate:
    latitude_scale: 0.0002
    latitude_strength: 0.3
    local_blend: 0.3
    global:
      temperature:
        octaves: 4
        frequency: 0.001
        lacunarity: 2
        persistence: 0.5
        scale: 1
        offset: 0
      humidity:
        octaves: 4
        frequency: 0.001
        lacunarity: 2
        persistence: 0.5
        scale: 1
        offset: 0
      continentalness:
        octaves: 4
        frequency: 0.001
        lacunarity: 2
        persistence: 0.5
        scale: 1
        offset: 0
    local:
      temperature:
        octaves: 3
        frequency: 0.01
        lacunarity: 2
        persistence: 0.5
        scale: 1
        offset: 0
      humidity:
        octaves: 3
        frequency: 0.01
        lacunarity: 2
        persistence: 0.5
        scale: 1
        offset: 0
      continentalness:
        octaves: 3
        frequency: 0.01
        lacunarity: 2
        persistence: 0.5
        scale: 1
        offset: 0
  biomes:
    blend_power: 2
    epsilon: 0.0001
    coast:
      biome: beach
      min_continentalness: -0.15
      max_continentalness: -0.05
    entries:
      - id: land
        target:
          temperature: 0.5
          humidity: 0.5
          continentalness: 0.5
        weight: 1
        water_fill: false
        surface:
          - material: test:grass
            depth: 1
          - material: test:dirt
            depth: 3
      - id: beach
        target:
          temperature: 0.5
          humidity: 0.5
          continentalness: -0.1
        weight: 1
        water_fill: false
        surface:
          - material: test:sand
            depth: 4
      - id: ocean
        target:
          temperature: 0.5
          humidity: 0.5
          continentalness: -0.8
        weight: 1
        water_fill: true
        surface:
          - material: test:sand
            depth: 3
  density_graph:
    outputs:
      terrain_density: terrain
      cave_density: cave
    nodes:
      - id: terrain
        type: constant
        value: 0.25
      - id: cave
        type: noise3d
        noise:
          octaves: 3
          frequency: 0.04
          lacunarity: 2
          persistence: 0.5
          scale: 1
          offset: 0
        scale: 1
        offset: 0
      - id: author_note
        type: constant
        value: 99
  caves:
    enabled: true
    density_output: cave_density
    threshold: 0.75
  structures:
    enabled: true
    features:
      - id: boulders
        material: test:stone
        chance: 0.02
        min_height: 1
        max_height: 3
        biomes: []
)yaml";
}

void replaceOnce(std::string& text,
                 std::string_view before,
                 std::string_view after) {
    const size_t position = text.find(before);
    if (position == std::string::npos) {
        throw Rigel::Test::TestFailure(
            "Generator definition fixture mutation target is missing");
    }
    text.replace(position, before.size(), after);
}

bool rejectsMutation(std::string_view before, std::string_view after) {
    std::string yaml = validDefinitionYaml();
    replaceOnce(yaml, before, after);
    try {
        static_cast<void>(parseGeneratorDefinition(yaml, "mutation.yaml"));
        return false;
    } catch (const std::invalid_argument&) {
        return true;
    }
}

} // namespace

TEST_CASE(GeneratorDefinition_parses_complete_graph_only_author_contract) {
    const GeneratorDefinition definition =
        parseGeneratorDefinition(validDefinitionYaml(), "complete.yaml");

    CHECK_EQ(definition.schemaVersion, uint32_t{1});
    CHECK_EQ(definition.id, std::string("test:continental"));
    CHECK_EQ(definition.sourceRevision, uint32_t{7});
    CHECK_EQ(definition.label, std::string("Continental"));
    CHECK_EQ(definition.data.bounds.minY, -64);
    CHECK_EQ(definition.data.terrain.densityOutput,
             std::string("terrain_density"));
    CHECK_EQ(definition.data.biomes.entries.size(), size_t{3});
    CHECK(definition.data.biomes.entries.back().waterFill);
    CHECK(definition.data.caves.enabled);
    CHECK(definition.data.structures.enabled);
    CHECK(definition.data.structures.features.front().biomes.empty());
}

TEST_CASE(GeneratorDefinition_rejects_unknown_missing_and_inapplicable_fields) {
    CHECK(rejectsMutation(
        "  bounds:\n", "  legacy_mode: simple\n  bounds:\n"));
    CHECK(rejectsMutation(
        "    water_material: test:water\n", ""));
    CHECK(rejectsMutation(
        "        type: constant\n        value: 0.25\n",
        "        type: constant\n        value: 0.25\n        scale: 2\n"));
    CHECK(rejectsMutation("        type: noise3d\n",
                          "        type: mystery\n"));
    CHECK(rejectsMutation("    local_blend: 0.3\n",
                          "    local_blend: unrestricted\n"));
    CHECK(rejectsMutation("  structures:\n    enabled: true\n",
                          "  structures:\n"));
}

TEST_CASE(GeneratorDefinition_rejects_duplicates_and_dangling_references) {
    CHECK(rejectsMutation("      - id: beach\n", "      - id: land\n"));
    CHECK(rejectsMutation("      - id: cave\n", "      - id: terrain\n"));
    CHECK(rejectsMutation("      terrain_density: terrain\n",
                          "      terrain_density: missing\n"));
    CHECK(rejectsMutation("      biome: beach\n", "      biome: missing\n"));
    CHECK(rejectsMutation("        biomes: []\n",
                          "        biomes: [missing]\n"));
    CHECK(rejectsMutation("    density_output: cave_density\n",
                          "    density_output: missing\n"));
}

TEST_CASE(GeneratorDefinition_rejects_invalid_ranges_and_dependencies) {
    CHECK(rejectsMutation("    min_y: -64\n    max_y: 319\n",
                          "    min_y: 319\n    max_y: -64\n"));
    CHECK(rejectsMutation(
        "      min_continentalness: -0.15\n"
        "      max_continentalness: -0.05\n",
        "      min_continentalness: 0.1\n"
        "      max_continentalness: -0.1\n"));
    CHECK(rejectsMutation("        min_height: 1\n        max_height: 3\n",
                          "        min_height: 4\n        max_height: 3\n"));
    CHECK(rejectsMutation("        surface:\n",
                          "        surface: []\n        ignored:\n"));
    CHECK(rejectsMutation(
        "  caves:\n    enabled: true\n    density_output: cave_density\n"
        "    threshold: 0.75\n",
        "  caves:\n    enabled: false\n    density_output: cave_density\n"
        "    threshold: 0.75\n"));
    CHECK(rejectsMutation(
        "  structures:\n    enabled: true\n    features:\n",
        "  structures:\n    enabled: false\n    features:\n"));
}

TEST_CASE(GeneratorDefinition_validates_unreachable_nodes_before_pruning) {
    CHECK(rejectsMutation(
        "      - id: author_note\n        type: constant\n        value: 99\n",
        "      - id: author_note\n        type: add\n"
        "        inputs: [missing]\n"));
}

TEST_CASE(GeneratorDefinition_authoring_serialization_is_normalized_and_stable) {
    const GeneratorDefinition parsed =
        parseGeneratorDefinition(validDefinitionYaml(), "author.yaml");
    const std::string canonical = serializeGeneratorDefinition(parsed);
    const GeneratorDefinition reparsed =
        parseGeneratorDefinition(canonical, "canonical-author.yaml");

    CHECK_EQ(serializeGeneratorDefinition(reparsed), canonical);
    CHECK(canonical.find("mode:") == std::string::npos);
    CHECK(canonical.find("stages:") == std::string::npos);
    CHECK(canonical.find("author_note") != std::string::npos);
}

TEST_CASE(GeneratorDefinition_snapshot_is_metadata_free_effective_data) {
    const GeneratorDefinition definition =
        parseGeneratorDefinition(validDefinitionYaml(), "snapshot-source.yaml");
    const std::string snapshot =
        serializeGeneratorDefinitionSnapshot(definition.data);

    CHECK(snapshot.find("schema_version") == std::string::npos);
    CHECK(snapshot.find("source_revision") == std::string::npos);
    CHECK(snapshot.find("label:") == std::string::npos);
    CHECK(snapshot.find("description:") == std::string::npos);
    CHECK(snapshot.find("author_note") == std::string::npos);
    CHECK(snapshot.find("density_output: \"terrain_density\"") !=
          std::string::npos);
    CHECK(snapshot.find("water_fill: true") != std::string::npos);

    const auto loaded = parseGeneratorDefinitionSnapshot(
        snapshot, 1u, "generator-definition.yaml");
    CHECK_EQ(serializeGeneratorDefinitionSnapshot(loaded), snapshot);
}

TEST_CASE(GeneratorDefinition_snapshot_parser_requires_canonical_content) {
    const GeneratorDefinition definition =
        parseGeneratorDefinition(validDefinitionYaml(), "snapshot-source.yaml");
    const std::string canonical =
        serializeGeneratorDefinitionSnapshot(definition.data);

    std::string unknown = canonical;
    unknown += "legacy_mode: simple\n";
    CHECK_THROWS(parseGeneratorDefinitionSnapshot(
        unknown, 1u, "unknown-snapshot.yaml"));

    std::string unused = canonical;
    const size_t caves = unused.find("caves:\n");
    CHECK(caves != std::string::npos);
    unused.insert(
        caves,
        "    - id: \"unused\"\n"
        "      type: \"constant\"\n"
        "      value: 4\n");
    CHECK_THROWS(parseGeneratorDefinitionSnapshot(
        unused, 1u, "noncanonical-snapshot.yaml"));
    CHECK_THROWS(parseGeneratorDefinitionSnapshot(
        canonical, 2u, "newer-snapshot.yaml"));
}

TEST_CASE(GeneratorDefinition_snapshot_normalizes_splines_and_disabled_features) {
    GeneratorDefinition definition =
        parseGeneratorDefinition(validDefinitionYaml(), "normalization.yaml");
    Rigel::Voxel::GeneratorDefinitionData::DensityNode spline;
    spline.id = "shaped";
    spline.type = "spline";
    spline.inputs = {"terrain"};
    spline.splinePoints = {{1.0f, 2.0f}, {-1.0f, -2.0f}, {0.0f, 0.5f}};
    definition.data.densityGraph.nodes.push_back(std::move(spline));
    definition.data.densityGraph.outputs.front().node = "shaped";
    definition.data.caves = {};
    definition.data.densityGraph.outputs.erase(
        definition.data.densityGraph.outputs.begin() + 1);
    definition.data.structures = {};

    const std::string snapshot =
        serializeGeneratorDefinitionSnapshot(definition.data);
    CHECK(snapshot.find("density_output: cave_density") == std::string::npos);
    CHECK(snapshot.find("features:") == std::string::npos);
    CHECK(snapshot.find("caves:\n  enabled: false\n") != std::string::npos);
    CHECK(snapshot.find("structures:\n  enabled: false\n") !=
          std::string::npos);
    CHECK(snapshot.find("- [-1, -2]") < snapshot.find("- [0, 0.5]"));
    CHECK(snapshot.find("- [0, 0.5]") < snapshot.find("- [1, 2]"));
    CHECK_NO_THROW(parseGeneratorDefinitionSnapshot(
        snapshot, 1u, "normalized-snapshot.yaml"));
}
