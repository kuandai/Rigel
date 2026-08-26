#include "TestFramework.h"

#include "Rigel/Voxel/GeneratorSnapshot.h"

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
}
