#include "TestFramework.h"

#include "Rigel/Asset/AssetLoader.h"
#include "Rigel/Asset/AssetManager.h"
#include "Rigel/Voxel/BlockLoader.h"
#include "Rigel/Voxel/GeneratorDefinition.h"
#include "Rigel/Voxel/GeneratorDefinitionLoader.h"
#include "Rigel/Voxel/BlockRegistry.h"
#include "Rigel/Voxel/BlockType.h"
#include "Rigel/Voxel/TextureAtlas.h"
#include "Rigel/Voxel/WorldGenerator.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <string_view>

namespace {

using Rigel::Voxel::GeneratorDefinition;
using Rigel::Voxel::GeneratorDefinitionData;
using Rigel::Voxel::parseGeneratorDefinition;
using Rigel::Voxel::parseGeneratorDefinitionSnapshot;
using Rigel::Voxel::prepareGeneratorDefinitionSnapshot;
using Rigel::Voxel::serializeGeneratorDefinition;

std::string validDefinitionYaml() {
    return R"yaml(generator:
  schema_version: 2
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

void replaceFirstScalar(std::string& text,
                        std::string_view field,
                        std::string_view replacement) {
    const size_t position = text.find(field);
    if (position == std::string::npos) {
        throw Rigel::Test::TestFailure(
            "Generator definition scalar fixture is missing");
    }
    const size_t valueStart = position + field.size();
    const size_t valueEnd = text.find('\n', valueStart);
    if (valueEnd == std::string::npos) {
        throw Rigel::Test::TestFailure(
            "Generator definition scalar fixture is unterminated");
    }
    text.replace(valueStart, valueEnd - valueStart, replacement);
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

void registerDefinitionMaterials(Rigel::Voxel::BlockRegistry& registry,
                                 bool includeWater = true) {
    for (const std::string identifier : {
             "test:stone", "test:water", "test:grass", "test:dirt",
             "test:sand"}) {
        if (!includeWater && identifier == "test:water") {
            continue;
        }
        Rigel::Voxel::BlockType block;
        block.identifier = identifier;
        registry.registerBlock(identifier, std::move(block));
    }
}

GeneratorDefinition definitionWithoutUnusedNodes() {
    GeneratorDefinition definition =
        parseGeneratorDefinition(validDefinitionYaml(), "installed.yaml");
    definition.data.densityGraph.nodes.erase(
        definition.data.densityGraph.nodes.begin() + 2);
    return definition;
}

Rigel::Voxel::PreparedGeneratorDefinitionSnapshot prepareSnapshot(
    const GeneratorDefinition& definition) {
    Rigel::Voxel::BlockRegistry registry;
    registerDefinitionMaterials(registry);
    return prepareGeneratorDefinitionSnapshot(definition, registry);
}

GeneratorDefinitionData parseSavedSnapshot(
    const GeneratorDefinitionData& data,
    std::string_view sourceName) {
    return parseGeneratorDefinitionSnapshot(
        Rigel::Voxel::serializeGeneratorDefinitionSnapshot(data),
        Rigel::Voxel::kGeneratorDefinitionSchemaVersion,
        sourceName);
}

void checkAuthorAndSavedSnapshotAccept(
    const GeneratorDefinition& definition,
    std::string_view sourceName) {
    const auto prepared = prepareSnapshot(definition);
    CHECK_EQ(
        parseGeneratorDefinitionSnapshot(
            prepared.canonicalSnapshot,
            prepared.definitionSchemaVersion,
            sourceName),
        prepared.data);
    CHECK_EQ(parseSavedSnapshot(definition.data, sourceName), prepared.data);
}

void checkAuthorAndSavedSnapshotReject(
    const GeneratorDefinition& definition,
    std::string_view sourceName) {
    CHECK_THROWS(prepareSnapshot(definition));
    CHECK_THROWS(parseSavedSnapshot(definition.data, sourceName));
}

uint64_t appendGoldenBytes(uint64_t hash, std::string_view value) {
    constexpr uint64_t Prime = 1099511628211ull;
    for (const unsigned char byte : value) {
        hash ^= byte;
        hash *= Prime;
    }
    hash ^= 0xffu;
    hash *= Prime;
    return hash;
}

} // namespace

TEST_CASE(GeneratorDefinition_parses_complete_graph_only_author_contract) {
    const GeneratorDefinition definition =
        parseGeneratorDefinition(validDefinitionYaml(), "complete.yaml");

    CHECK_EQ(definition.schemaVersion,
             Rigel::Voxel::kGeneratorDefinitionSchemaVersion);
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
        "  description: A complete graph-only test definition.\n",
        "  description: \"\"\n"));
    CHECK(rejectsMutation(
        "        type: constant\n        value: 0.25\n",
        "        type: constant\n        value: 0.25\n        scale: 2\n"));
    CHECK(rejectsMutation("        type: noise3d\n",
                          "        type: mystery\n"));
    CHECK(rejectsMutation("        type: noise3d\n        noise:\n",
                          "        type: noise3d\n        scale: 2\n"
                          "        noise:\n"));
    CHECK(rejectsMutation(
        "      - id: author_note\n        type: constant\n        value: 99\n",
        "      - id: author_note\n        type: climate\n"
        "        field: wind\n"));
    CHECK(rejectsMutation("    local_blend: 0.3\n",
                          "    local_blend: unrestricted\n"));
    CHECK(rejectsMutation("    latitude_strength: 0.3\n",
                          "    latitude_strength: .nan\n"));
    CHECK(rejectsMutation("  structures:\n    enabled: true\n",
                          "  structures:\n"));
}

TEST_CASE(GeneratorDefinition_rejects_duplicates_and_dangling_references) {
    CHECK(rejectsMutation("      - id: beach\n", "      - id: land\n"));
    CHECK(rejectsMutation("      - id: cave\n", "      - id: terrain\n"));
    CHECK(rejectsMutation("      terrain_density: terrain\n",
                          "      terrain_density: missing\n"));
    CHECK(rejectsMutation(
        "      terrain_density: terrain\n",
        "      terrain_density: terrain\n      terrain_density: terrain\n"));
    CHECK(rejectsMutation("      biome: beach\n", "      biome: missing\n"));
    CHECK(rejectsMutation("        biomes: []\n",
                          "        biomes: [missing]\n"));
    CHECK(rejectsMutation("    density_output: cave_density\n",
                          "    density_output: missing\n"));

    GeneratorDefinition duplicateFeature = definitionWithoutUnusedNodes();
    duplicateFeature.data.structures.features.push_back(
        duplicateFeature.data.structures.features.front());
    CHECK_THROWS(prepareSnapshot(duplicateFeature));
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
    CHECK(rejectsMutation(
        "          - material: test:grass\n"
        "            depth: 1\n"
        "          - material: test:dirt\n"
        "            depth: 3\n",
        "          - material: test:grass\n"
        "            depth: 20\n"
        "          - material: test:dirt\n"
        "            depth: 20\n"));
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

TEST_CASE(GeneratorDefinition_accepts_exact_engine_safety_boundaries) {
    GeneratorDefinition minimumY = definitionWithoutUnusedNodes();
    minimumY.data.bounds = {
        GeneratorDefinitionData::MinWorldY,
        GeneratorDefinitionData::MinWorldY + 1};
    minimumY.data.terrain.seaLevel = GeneratorDefinitionData::MinWorldY;
    checkAuthorAndSavedSnapshotAccept(minimumY, "minimum-y-snapshot.yaml");
    CHECK_EQ(minimumY.data.bounds.minY, GeneratorDefinitionData::MinWorldY);

    GeneratorDefinition maximumY = definitionWithoutUnusedNodes();
    maximumY.data.bounds = {
        GeneratorDefinitionData::MaxWorldY - 1,
        GeneratorDefinitionData::MaxWorldY};
    maximumY.data.terrain.seaLevel = GeneratorDefinitionData::MaxWorldY;
    checkAuthorAndSavedSnapshotAccept(maximumY, "maximum-y-snapshot.yaml");
    CHECK_EQ(maximumY.data.bounds.maxY, GeneratorDefinitionData::MaxWorldY);

    GeneratorDefinition maximumHeight = definitionWithoutUnusedNodes();
    maximumHeight.data.bounds = {
        0,
        GeneratorDefinitionData::MaxWorldHeight - 1};
    checkAuthorAndSavedSnapshotAccept(
        maximumHeight, "maximum-height-snapshot.yaml");
    CHECK_EQ(
        maximumHeight.data.bounds.maxY - maximumHeight.data.bounds.minY + 1,
        GeneratorDefinitionData::MaxWorldHeight);

    GeneratorDefinition maximumOctaves = definitionWithoutUnusedNodes();
    auto& noise = maximumOctaves.data.climate.global.temperature;
    noise.octaves = GeneratorDefinitionData::MaxNoiseOctaves;
    noise.lacunarity = 1.0f;
    checkAuthorAndSavedSnapshotAccept(
        maximumOctaves, "maximum-octaves-snapshot.yaml");
    CHECK_EQ(noise.octaves, GeneratorDefinitionData::MaxNoiseOctaves);
}

TEST_CASE(GeneratorDefinition_rejects_values_beyond_engine_safety_boundaries) {
    GeneratorDefinition belowMinimumY = definitionWithoutUnusedNodes();
    belowMinimumY.data.bounds = {
        GeneratorDefinitionData::MinWorldY - 1,
        GeneratorDefinitionData::MinWorldY};
    belowMinimumY.data.terrain.seaLevel = GeneratorDefinitionData::MinWorldY;
    checkAuthorAndSavedSnapshotReject(
        belowMinimumY, "below-minimum-y-snapshot.yaml");

    GeneratorDefinition aboveMaximumY = definitionWithoutUnusedNodes();
    aboveMaximumY.data.bounds = {
        GeneratorDefinitionData::MaxWorldY,
        GeneratorDefinitionData::MaxWorldY + 1};
    aboveMaximumY.data.terrain.seaLevel = GeneratorDefinitionData::MaxWorldY;
    checkAuthorAndSavedSnapshotReject(
        aboveMaximumY, "above-maximum-y-snapshot.yaml");

    GeneratorDefinition excessiveHeight = definitionWithoutUnusedNodes();
    excessiveHeight.data.bounds = {
        0,
        GeneratorDefinitionData::MaxWorldHeight};
    checkAuthorAndSavedSnapshotReject(
        excessiveHeight, "excessive-height-snapshot.yaml");
    CHECK_EQ(
        excessiveHeight.data.bounds.maxY - excessiveHeight.data.bounds.minY + 1,
        GeneratorDefinitionData::MaxWorldHeight + 1);

    GeneratorDefinition seaBelowBounds = definitionWithoutUnusedNodes();
    seaBelowBounds.data.terrain.seaLevel = seaBelowBounds.data.bounds.minY - 1;
    checkAuthorAndSavedSnapshotReject(
        seaBelowBounds, "sea-below-bounds-snapshot.yaml");

    GeneratorDefinition seaAboveBounds = definitionWithoutUnusedNodes();
    seaAboveBounds.data.terrain.seaLevel = seaAboveBounds.data.bounds.maxY + 1;
    checkAuthorAndSavedSnapshotReject(
        seaAboveBounds, "sea-above-bounds-snapshot.yaml");

    GeneratorDefinition excessiveOctaves = definitionWithoutUnusedNodes();
    excessiveOctaves.data.climate.global.temperature.octaves =
        GeneratorDefinitionData::MaxNoiseOctaves + 1;
    checkAuthorAndSavedSnapshotReject(
        excessiveOctaves, "excessive-octaves-snapshot.yaml");
}

TEST_CASE(GeneratorDefinition_rejects_unsafe_evaluator_arithmetic) {
    CHECK(rejectsMutation(
        "        frequency: 0.001\n",
        "        frequency: 3.40282347e+38\n"));
    CHECK(rejectsMutation(
        "        octaves: 4\n"
        "        frequency: 0.001\n"
        "        lacunarity: 2\n",
        "        octaves: 3\n"
        "        frequency: 0.5\n"
        "        lacunarity: 2\n"));
    CHECK(rejectsMutation(
        "        scale: 1\n"
        "        offset: 0\n",
        "        scale: 3.40282347e+38\n"
        "        offset: 3.40282347e+38\n"));

    GeneratorDefinition derived =
        definitionWithoutUnusedNodes();
    derived.data.climate.global.temperature.scale = 0.0f;
    derived.data.climate.global.temperature.offset = 2.9e38f;
    derived.data.climate.local.temperature.scale = 0.0f;
    derived.data.climate.local.temperature.offset = 2.9e38f;
    CHECK_THROWS(prepareSnapshot(derived));

    GeneratorDefinition composition = definitionWithoutUnusedNodes();
    auto& terrain = composition.data.densityGraph.nodes.front();
    terrain.type = "add";
    terrain.value = 0.0f;
    terrain.inputs = {"extreme_a", "extreme_b"};
    GeneratorDefinitionData::DensityNode extremeA;
    extremeA.id = "extreme_a";
    extremeA.type = "constant";
    extremeA.value = std::numeric_limits<float>::max();
    GeneratorDefinitionData::DensityNode extremeB = extremeA;
    extremeB.id = "extreme_b";
    composition.data.densityGraph.nodes.push_back(std::move(extremeA));
    composition.data.densityGraph.nodes.push_back(std::move(extremeB));
    CHECK_THROWS(prepareSnapshot(composition));
    composition.data.densityGraph.nodes.front().type = "mul";
    CHECK_THROWS(prepareSnapshot(composition));

    GeneratorDefinition yTransform = definitionWithoutUnusedNodes();
    auto& y = yTransform.data.densityGraph.nodes.front();
    y.type = "y";
    y.value = 0.0f;
    y.scale = std::numeric_limits<float>::max();
    y.offset = std::numeric_limits<float>::max();
    CHECK_THROWS(prepareSnapshot(yTransform));
}

TEST_CASE(GeneratorDefinition_snapshot_rejects_unsafe_evaluator_arithmetic) {
    const std::string canonical =
        prepareSnapshot(definitionWithoutUnusedNodes()).canonicalSnapshot;

    std::string frequency = canonical;
    replaceFirstScalar(
        frequency, "frequency: ", "3.40282347e+38");
    CHECK_THROWS(parseGeneratorDefinitionSnapshot(
        frequency,
        Rigel::Voxel::kGeneratorDefinitionSchemaVersion,
        "extreme-frequency-snapshot.yaml"));

    std::string transform = canonical;
    replaceFirstScalar(transform, "scale: ", "3.40282347e+38");
    replaceFirstScalar(transform, "offset: ", "3.40282347e+38");
    CHECK_THROWS(parseGeneratorDefinitionSnapshot(
        transform,
        Rigel::Voxel::kGeneratorDefinitionSchemaVersion,
        "extreme-transform-snapshot.yaml"));

    GeneratorDefinition composition = definitionWithoutUnusedNodes();
    auto& terrain = composition.data.densityGraph.nodes.front();
    terrain.type = "add";
    terrain.value = 0.0f;
    terrain.inputs = {"snapshot_a", "snapshot_b"};
    GeneratorDefinitionData::DensityNode snapshotA;
    snapshotA.id = "snapshot_a";
    snapshotA.type = "constant";
    snapshotA.value = 1.0f;
    GeneratorDefinitionData::DensityNode snapshotB = snapshotA;
    snapshotB.id = "snapshot_b";
    composition.data.densityGraph.nodes.push_back(std::move(snapshotA));
    composition.data.densityGraph.nodes.push_back(std::move(snapshotB));
    std::string densityComposition =
        prepareSnapshot(composition).canonicalSnapshot;
    replaceOnce(
        densityComposition, "value: 1\n", "value: 3.40282347e+38\n");
    replaceOnce(
        densityComposition, "value: 1\n", "value: 3.40282347e+38\n");
    CHECK_THROWS(parseGeneratorDefinitionSnapshot(
        densityComposition,
        Rigel::Voxel::kGeneratorDefinitionSchemaVersion,
        "extreme-density-snapshot.yaml"));
}

TEST_CASE(GeneratorDefinition_rejects_coast_only_biome_selection) {
    GeneratorDefinition definition =
        parseGeneratorDefinition(validDefinitionYaml(), "coast-only.yaml");
    const auto coast = std::find_if(
        definition.data.biomes.entries.begin(),
        definition.data.biomes.entries.end(),
        [&](const auto& biome) {
            return biome.id == definition.data.biomes.coast.biome;
        });
    CHECK(coast != definition.data.biomes.entries.end());
    definition.data.biomes.entries = {*coast};

    std::string diagnostic;
    try {
        static_cast<void>(prepareSnapshot(definition));
    } catch (const std::invalid_argument& error) {
        diagnostic = error.what();
    }
    CHECK(diagnostic.find("generator.biomes.entries") != std::string::npos);
    CHECK(diagnostic.find("outside the coast band") != std::string::npos);
}

TEST_CASE(GeneratorDefinition_validates_unreachable_nodes_before_pruning) {
    CHECK(rejectsMutation(
        "      - id: author_note\n        type: constant\n        value: 99\n",
        "      - id: author_note\n        type: add\n"
        "        inputs: [missing]\n"));
    CHECK(rejectsMutation(
        "      - id: author_note\n        type: constant\n        value: 99\n",
        "      - id: author_note\n        type: add\n"
        "        inputs: [author_note]\n"));
}

TEST_CASE(GeneratorDefinition_enforces_programmatic_collection_limits) {
    GeneratorDefinition tooManyNodes =
        parseGeneratorDefinition(validDefinitionYaml(), "node-limit.yaml");
    while (tooManyNodes.data.densityGraph.nodes.size() <=
           Rigel::Voxel::GeneratorDefinitionData::MaxDensityGraphNodes) {
        auto node = tooManyNodes.data.densityGraph.nodes.front();
        node.id = "extra_node_" +
            std::to_string(tooManyNodes.data.densityGraph.nodes.size());
        tooManyNodes.data.densityGraph.nodes.push_back(std::move(node));
    }
    CHECK_THROWS(prepareSnapshot(tooManyNodes));

    GeneratorDefinition tooManyOutputs =
        parseGeneratorDefinition(validDefinitionYaml(), "output-limit.yaml");
    while (tooManyOutputs.data.densityGraph.outputs.size() <=
           Rigel::Voxel::GeneratorDefinitionData::MaxDensityGraphOutputs) {
        tooManyOutputs.data.densityGraph.outputs.push_back(
            {"extra_output_" +
                 std::to_string(tooManyOutputs.data.densityGraph.outputs.size()),
             "terrain"});
    }
    CHECK_THROWS(prepareSnapshot(tooManyOutputs));

    GeneratorDefinition tooManySplinePoints =
        parseGeneratorDefinition(validDefinitionYaml(), "spline-limit.yaml");
    Rigel::Voxel::GeneratorDefinitionData::DensityNode spline;
    spline.id = "oversized_spline";
    spline.type = "spline";
    spline.inputs = {"terrain"};
    for (size_t point = 0;
         point <=
         Rigel::Voxel::GeneratorDefinitionData::MaxDensitySplinePoints;
         ++point) {
        spline.splinePoints.emplace_back(
            static_cast<float>(point), static_cast<float>(point));
    }
    tooManySplinePoints.data.densityGraph.nodes.push_back(std::move(spline));
    tooManySplinePoints.data.densityGraph.outputs.front().node =
        "oversized_spline";
    CHECK_THROWS(prepareSnapshot(tooManySplinePoints));

    GeneratorDefinition tooManyFeatures =
        parseGeneratorDefinition(validDefinitionYaml(), "feature-limit.yaml");
    while (tooManyFeatures.data.structures.features.size() <=
           Rigel::Voxel::GeneratorDefinitionData::MaxStructureFeatures) {
        auto feature = tooManyFeatures.data.structures.features.front();
        feature.id = "extra_feature_" +
            std::to_string(tooManyFeatures.data.structures.features.size());
        tooManyFeatures.data.structures.features.push_back(std::move(feature));
    }
    CHECK_THROWS(prepareSnapshot(tooManyFeatures));

    GeneratorDefinition tooManyBiomes =
        parseGeneratorDefinition(validDefinitionYaml(), "biome-limit.yaml");
    while (tooManyBiomes.data.biomes.entries.size() <=
           Rigel::Voxel::GeneratorDefinitionData::MaxBiomeEntries) {
        auto biome = tooManyBiomes.data.biomes.entries.front();
        biome.id = "extra_biome_" +
            std::to_string(tooManyBiomes.data.biomes.entries.size());
        tooManyBiomes.data.biomes.entries.push_back(std::move(biome));
    }
    CHECK_THROWS(prepareSnapshot(tooManyBiomes));
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
    const GeneratorDefinition authoring =
        parseGeneratorDefinition(validDefinitionYaml(), "snapshot-source.yaml");
    CHECK(Rigel::Voxel::serializeGeneratorDefinitionSnapshot(authoring.data)
              .find("author_note") == std::string::npos);

    GeneratorDefinition definition = definitionWithoutUnusedNodes();
    definition.data.structures.features.front().biomes = {"ocean", "land"};
    const auto prepared = prepareSnapshot(definition);
    const std::string& snapshot = prepared.canonicalSnapshot;

    CHECK_EQ(prepared.sourceId, definition.id);
    CHECK_EQ(prepared.sourceRevision, definition.sourceRevision);
    CHECK_EQ(prepared.definitionSchemaVersion, definition.schemaVersion);

    CHECK(snapshot.find("schema_version") == std::string::npos);
    CHECK(snapshot.find("source_revision") == std::string::npos);
    CHECK(snapshot.find("label:") == std::string::npos);
    CHECK(snapshot.find("description:") == std::string::npos);
    CHECK(snapshot.find("density_output: \"terrain_density\"") !=
          std::string::npos);
    CHECK(snapshot.find("water_fill: true") != std::string::npos);
    CHECK(snapshot.find(
              "      biomes:\n        - \"land\"\n        - \"ocean\"\n") !=
          std::string::npos);

    const auto loaded = parseGeneratorDefinitionSnapshot(
        snapshot, Rigel::Voxel::kGeneratorDefinitionSchemaVersion,
        "generator-definition.yaml");
    GeneratorDefinition loadedDefinition = definition;
    loadedDefinition.data = loaded;
    CHECK_EQ(prepareSnapshot(loadedDefinition).canonicalSnapshot, snapshot);
}

TEST_CASE(GeneratorDefinition_snapshot_parser_requires_canonical_content) {
    const GeneratorDefinition definition = definitionWithoutUnusedNodes();
    const std::string canonical =
        prepareSnapshot(definition).canonicalSnapshot;

    std::string unknown = canonical;
    unknown += "legacy_mode: simple\n";
    CHECK_THROWS(parseGeneratorDefinitionSnapshot(
        unknown, Rigel::Voxel::kGeneratorDefinitionSchemaVersion,
        "unknown-snapshot.yaml"));

    std::string unused = canonical;
    const size_t caves = unused.find("caves:\n");
    CHECK(caves != std::string::npos);
    unused.insert(
        caves,
        "    - id: \"unused\"\n"
        "      type: \"constant\"\n"
        "      value: 4\n");
    CHECK_THROWS(parseGeneratorDefinitionSnapshot(
        unused, Rigel::Voxel::kGeneratorDefinitionSchemaVersion,
        "noncanonical-snapshot.yaml"));
    CHECK_THROWS(parseGeneratorDefinitionSnapshot(
        canonical, Rigel::Voxel::kGeneratorDefinitionSchemaVersion + 1u,
        "newer-snapshot.yaml"));

    GeneratorDefinition programmatic = definition;
    programmatic.data.densityGraph.nodes.front().inputs = {"cave"};
    CHECK_THROWS(prepareSnapshot(programmatic));
}

TEST_CASE(GeneratorDefinition_snapshot_normalizes_splines_and_disabled_features) {
    GeneratorDefinition definition = definitionWithoutUnusedNodes();
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
    std::erase_if(
        definition.data.densityGraph.nodes,
        [](const auto& node) { return node.id == "cave"; });
    definition.data.structures = {};

    const std::string snapshot =
        prepareSnapshot(definition).canonicalSnapshot;
    CHECK(snapshot.find("density_output: cave_density") == std::string::npos);
    CHECK(snapshot.find("features:") == std::string::npos);
    CHECK(snapshot.find("caves:\n  enabled: false\n") != std::string::npos);
    CHECK(snapshot.find("structures:\n  enabled: false\n") !=
          std::string::npos);
    CHECK(snapshot.find("- [-1, -2]") < snapshot.find("- [0, 0.5]"));
    CHECK(snapshot.find("- [0, 0.5]") < snapshot.find("- [1, 2]"));
    CHECK_NO_THROW(parseGeneratorDefinitionSnapshot(
        snapshot, Rigel::Voxel::kGeneratorDefinitionSchemaVersion,
        "normalized-snapshot.yaml"));
}

TEST_CASE(GeneratorDefinition_preparation_requires_every_material) {
    const GeneratorDefinition definition = definitionWithoutUnusedNodes();
    Rigel::Voxel::BlockRegistry incomplete;
    registerDefinitionMaterials(incomplete, false);
    CHECK_THROWS(prepareGeneratorDefinitionSnapshot(definition, incomplete));

    Rigel::Voxel::BlockRegistry complete;
    registerDefinitionMaterials(complete);
    CHECK_NO_THROW(prepareGeneratorDefinitionSnapshot(definition, complete));

    GeneratorDefinition missingSurface = definition;
    missingSurface.data.biomes.entries.front().surface.front().material =
        "test:missing";
    CHECK_THROWS(prepareGeneratorDefinitionSnapshot(missingSurface, complete));

    GeneratorDefinition missingFeature = definition;
    missingFeature.data.structures.features.front().material = "test:missing";
    CHECK_THROWS(prepareGeneratorDefinitionSnapshot(missingFeature, complete));
}

TEST_CASE(GeneratorDefinition_preparation_rejects_unreachable_nodes) {
    const GeneratorDefinition definition =
        parseGeneratorDefinition(validDefinitionYaml(), "dead-node.yaml");
    Rigel::Voxel::BlockRegistry registry;
    registerDefinitionMaterials(registry);

    CHECK_THROWS(prepareGeneratorDefinitionSnapshot(definition, registry));
    CHECK_NO_THROW(prepareGeneratorDefinitionSnapshot(
        definitionWithoutUnusedNodes(), registry));
}

TEST_CASE(GeneratorDefinition_shipped_default_bootstraps_strict_runtime) {
    Rigel::Asset::AssetManager assets;
    assets.loadManifest("manifest.yaml");
    Rigel::Voxel::BlockRegistry registry;
    Rigel::Voxel::TextureAtlas atlas;
    Rigel::Voxel::BlockLoader blocks;
    const Rigel::Voxel::BlockLoadReport report =
        blocks.loadFromManifest(assets, registry, atlas);
    CHECK_EQ(report.failed, size_t{0});

    const auto prepared =
        Rigel::Voxel::loadPreparedGeneratorDefinitionSnapshot(
            assets,
            registry,
            "rigel:default");
    CHECK_EQ(prepared.sourceId, std::string("rigel:default"));
    CHECK_EQ(prepared.sourceRevision, uint32_t{1});
    CHECK_EQ(
        prepared.definitionSchemaVersion,
        Rigel::Voxel::kGeneratorDefinitionSchemaVersion);
    CHECK_EQ(
        prepared.canonicalSnapshot,
        Rigel::Voxel::serializeGeneratorDefinitionSnapshot(prepared.data));
    CHECK_EQ(prepared.data.terrain.densityOutput,
             std::string("terrain_density"));
    CHECK_EQ(prepared.data.caves.densityOutput,
             std::string("cavern_field"));
    CHECK(prepared.data.caves.enabled);
    CHECK(!prepared.data.structures.enabled);

    CHECK_NO_THROW(Rigel::Voxel::WorldGenerator(
        registry, prepared.data, 1337u));
    bool assetBoundary = false;
    try {
        static_cast<void>(
            Rigel::Voxel::loadPreparedGeneratorDefinitionSnapshot(
                assets,
                registry,
                "rigel:missing"));
    } catch (const Rigel::Asset::AssetLoadError&) {
        assetBoundary = true;
    }
    CHECK(assetBoundary);
}

TEST_CASE(GeneratorDefinition_shipped_default_generation_is_repeatable_and_golden) {
    Rigel::Asset::AssetManager assets;
    assets.loadManifest("manifest.yaml");
    Rigel::Voxel::BlockRegistry registry;
    Rigel::Voxel::TextureAtlas atlas;
    Rigel::Voxel::BlockLoader blocks;
    CHECK_EQ(blocks.loadFromManifest(assets, registry, atlas).failed,
             size_t{0});
    const auto prepared =
        Rigel::Voxel::loadPreparedGeneratorDefinitionSnapshot(
            assets,
            registry,
            "rigel:default");
    Rigel::Voxel::WorldGenerator first(registry, prepared.data, 1337u);
    Rigel::Voxel::WorldGenerator second(registry, prepared.data, 1337u);
    constexpr std::array<Rigel::Voxel::ChunkCoord, 4> Coordinates = {{
        {0, 3, 0},
        {4, 3, -3},
        {-5, 4, 6},
        {9, 2, 7},
    }};

    uint64_t signature = 1469598103934665603ull;
    std::set<std::string> sampledMaterials;
    for (const auto coord : Coordinates) {
        Rigel::Voxel::ChunkBuffer firstBuffer;
        Rigel::Voxel::ChunkBuffer secondBuffer;
        first.generate(coord, firstBuffer);
        second.generate(coord, secondBuffer);
        CHECK(firstBuffer.blocks == secondBuffer.blocks);
        for (const auto state : firstBuffer.blocks) {
            const std::string& identifier =
                registry.getType(state.id).identifier;
            sampledMaterials.insert(identifier);
            signature = appendGoldenBytes(signature, identifier);
        }
    }

    CHECK(sampledMaterials.size() >= size_t{3});
    CHECK_EQ(signature, uint64_t{11943161365773689889ull});
}
