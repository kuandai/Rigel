#include "TestFramework.h"

#include "Rigel/Voxel/GeneratorSnapshot.h"
#include "Rigel/Voxel/BlockRegistry.h"
#include "Rigel/Voxel/BlockType.h"

#include <string>

using namespace Rigel::Voxel;

namespace {

WorldGenConfig snapshotDefinition() {
    WorldGenConfig definition;
    definition.seed = 123456789u;
    definition.solidBlock = "base:stone_shale";
    definition.surfaceBlock = "base:grass";
    definition.world.version = 27;
    definition.world.seaLevel = 42;
    definition.terrain.baseHeight = 999.0f;
    definition.flags["author_only"] = true;
    definition.overlays.push_back({"ignored.yaml", "author_only"});

    WorldGenConfig::BiomeConfig biome;
    biome.name = "land";
    biome.surface.push_back({"base:grass", 1});
    definition.biomes.entries.push_back(std::move(biome));

    WorldGenConfig::DensityNodeConfig baseDensity;
    baseDensity.id = "base";
    baseDensity.type = "constant";
    baseDensity.value = 0.25f;
    definition.densityGraph.nodes.push_back(std::move(baseDensity));
    definition.densityGraph.outputs["base_density"] = "base";
    WorldGenConfig::DensityNodeConfig dead;
    dead.id = "dead_authoring_node";
    dead.type = "constant";
    dead.value = -100.0f;
    definition.densityGraph.nodes.push_back(std::move(dead));
    definition.stageEnabled["caves"] = false;
    definition.stageEnabled["structures"] = false;
    return definition;
}

} // namespace

TEST_CASE(GeneratorSnapshot_round_trips_normalized_runtime_definition) {
    const WorldGenConfig installed = snapshotDefinition();
    const std::string snapshot = serializeGeneratorSnapshot(installed);

    CHECK(snapshot.find("seed:") == std::string::npos);
    CHECK(snapshot.find("version:") == std::string::npos);
    CHECK(snapshot.find("base_height") == std::string::npos);
    CHECK(snapshot.find("flags:") == std::string::npos);
    CHECK(snapshot.find("overlays:") == std::string::npos);
    CHECK(snapshot.find("\nstructures:\n") == std::string::npos);
    CHECK(snapshot.find("dead_authoring_node") == std::string::npos);

    const WorldGenConfig loaded = parseGeneratorSnapshot(
        snapshot,
        kGeneratorDefinitionSchemaVersion,
        998877u,
        51u);
    CHECK_EQ(loaded.seed, 998877u);
    CHECK_EQ(loaded.world.version, 51u);
    CHECK_EQ(loaded.world.seaLevel, installed.world.seaLevel);
    CHECK_EQ(loaded.solidBlock, installed.solidBlock);
    CHECK_EQ(loaded.densityGraph.nodes.size(), static_cast<size_t>(1));
    CHECK_EQ(loaded.densityGraph.outputs.size(), static_cast<size_t>(1));
    CHECK_EQ(serializeGeneratorSnapshot(loaded), snapshot);
}

TEST_CASE(GeneratorSnapshot_rejects_noncanonical_or_unknown_content) {
    const std::string canonical = serializeGeneratorSnapshot(snapshotDefinition());

    std::string unknownField = canonical;
    unknownField += "unknown_runtime_field: true\n";
    CHECK_THROWS(parseGeneratorSnapshot(
        unknownField,
        kGeneratorDefinitionSchemaVersion,
        1u,
        1u));

    std::string unknownType = canonical;
    const auto typePosition = unknownType.find("type: \"constant\"");
    CHECK(typePosition != std::string::npos);
    unknownType.replace(
        typePosition,
        std::string("type: \"constant\"").size(),
        "type: \"mystery\"");
    CHECK_THROWS(parseGeneratorSnapshot(
        unknownType,
        kGeneratorDefinitionSchemaVersion,
        1u,
        1u));

    CHECK_THROWS(parseGeneratorSnapshot(canonical, 2u, 1u, 1u));
}

TEST_CASE(GeneratorSnapshot_rejects_dangling_graph_and_content_references) {
    WorldGenConfig danglingGraph = snapshotDefinition();
    danglingGraph.densityGraph.outputs["base_density"] = "missing";
    CHECK_THROWS(serializeGeneratorSnapshot(danglingGraph));

    WorldGenConfig danglingBiome = snapshotDefinition();
    danglingBiome.biomes.coastBand.enabled = true;
    danglingBiome.biomes.coastBand.biome = "missing";
    CHECK_THROWS(serializeGeneratorSnapshot(danglingBiome));

    WorldGenConfig unusedOutput = snapshotDefinition();
    unusedOutput.densityGraph.outputs["unused_semantic"] =
        "dead_authoring_node";
    CHECK_THROWS(serializeGeneratorSnapshot(unusedOutput));
}

TEST_CASE(GeneratorSnapshot_validates_referenced_runtime_content) {
    WorldGenConfig definition = snapshotDefinition();
    BlockRegistry registry;
    for (const std::string identifier : {
             "base:stone_shale", "base:grass", "base:water[type=source]",
             "base:sand"}) {
        BlockType block;
        block.identifier = identifier;
        registry.registerBlock(identifier, std::move(block));
    }

    CHECK_NO_THROW(validateGeneratorSnapshotContent(definition, registry));
    definition.biomes.entries.front().surface.front().block = "base:missing";
    CHECK_THROWS(validateGeneratorSnapshotContent(definition, registry));
}

TEST_CASE(GeneratorSnapshot_requires_every_runtime_material_dependency) {
    WorldGenConfig definition = snapshotDefinition();
    BlockRegistry registry;
    auto registerBlock = [&](const std::string& identifier) {
        BlockType block;
        block.identifier = identifier;
        registry.registerBlock(identifier, std::move(block));
    };
    registerBlock(definition.solidBlock);
    registerBlock(definition.surfaceBlock);

    CHECK_THROWS(validateGeneratorSnapshotContent(definition, registry));
    registerBlock(definition.waterBlock);
    CHECK_THROWS(validateGeneratorSnapshotContent(definition, registry));
    registerBlock(definition.shoreBlock);
    CHECK_NO_THROW(validateGeneratorSnapshotContent(definition, registry));
}

TEST_CASE(GeneratorSnapshot_rejects_incoherent_node_and_pipeline_contracts) {
    WorldGenConfig definition = snapshotDefinition();
    definition.densityGraph.nodes.front().type = "abs";
    CHECK_THROWS(serializeGeneratorSnapshot(definition));

    definition = snapshotDefinition();
    definition.stageEnabled["terrain_density"] = false;
    CHECK_THROWS(serializeGeneratorSnapshot(definition));

    definition = snapshotDefinition();
    definition.climate.global.temperature.octaves = -1;
    CHECK_THROWS(serializeGeneratorSnapshot(definition));
}
