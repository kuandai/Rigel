#include "TestFramework.h"
#include "GeneratorDefinitionTestRegistry.h"

#include "Rigel/Voxel/WorldGenerator.h"

#include <algorithm>

using namespace Rigel::Voxel;

namespace {

BlockRegistry makeRegistry() {
    BlockRegistry registry;
    for (const std::string& identifier : {
             "rigel:stone",
             "rigel:grass",
             "rigel:water",
             "rigel:coast_surface",
             "rigel:tree"}) {
        BlockType block;
        block.identifier = identifier;
        block.isSolid = identifier != "rigel:water";
        block.isOpaque = identifier != "rigel:water";
        registry.registerBlock(identifier, std::move(block));
    }
    return registry;
}

GeneratorDefinitionData flatDefinition() {
    GeneratorDefinitionData data = Rigel::Test::generatorDefinitionFixture(
        "rigel:stone", "rigel:grass", "rigel:water");
    data.bounds = {-31, 30};
    data.terrain.seaLevel = 0;
    data.terrain.densityOutput = "authored_terrain";
    auto& density = data.densityGraph.nodes.front();
    density.type = "y";
    density.value = 0.0f;
    density.scale = -1.0f;
    density.offset = 0.0f;
    data.densityGraph.outputs.front() = {"authored_terrain", "ground"};
    return data;
}

void checkLayerAir(const ChunkBuffer& buffer, int y, bool expectedAir) {
    for (int z = 0; z < Chunk::SIZE; ++z) {
        for (int x = 0; x < Chunk::SIZE; ++x) {
            CHECK_EQ(buffer.at(x, y, z).isAir(), expectedAir);
        }
    }
}

} // namespace

TEST_CASE(WorldGenerator_consumes_arbitrary_validated_terrain_output) {
    BlockRegistry registry = makeRegistry();
    const GeneratorDefinitionData definition = flatDefinition();
    WorldGenerator generator(registry, definition, 123u);

    ChunkBuffer buffer;
    generator.generate({0, 0, 0}, buffer);

    CHECK_EQ(generator.definition(), definition);
    CHECK_EQ(generator.seed(), 123u);
    CHECK_EQ(
        buffer.at(0, 0, 0).id.type,
        registry.findByIdentifier("rigel:grass")->type);
    CHECK(buffer.at(0, 1, 0).isAir());
}

TEST_CASE(WorldGenerator_uses_explicit_coast_water_and_surface_semantics) {
    BlockRegistry registry = makeRegistry();
    GeneratorDefinitionData definition = flatDefinition();
    definition.densityGraph.nodes.front().offset = -1.0f;
    definition.biomes.entries.front().id = "inland";
    definition.biomes.entries.front().surface.front().material = "rigel:grass";
    GeneratorDefinitionData::Biome coast;
    coast.id = "tidal_flats";
    coast.weight = 1.0f;
    coast.waterFill = true;
    coast.surface.push_back({"rigel:coast_surface", 1});
    definition.biomes.entries.push_back(std::move(coast));
    definition.biomes.coast = {"tidal_flats", -100.0f, 100.0f};

    WorldGenerator generator(registry, definition, 7u);
    ChunkBuffer buffer;
    generator.generate({0, 0, 0}, buffer);

    CHECK_EQ(
        buffer.at(0, 0, 0).id.type,
        registry.findByIdentifier("rigel:water")->type);

    definition.densityGraph.nodes.front().offset = 0.0f;
    WorldGenerator surfaced(registry, definition, 7u);
    surfaced.generate({0, 0, 0}, buffer);
    CHECK_EQ(
        buffer.at(0, 0, 0).id.type,
        registry.findByIdentifier("rigel:coast_surface")->type);
}

TEST_CASE(WorldGenerator_honors_enabled_cave_output) {
    BlockRegistry registry = makeRegistry();
    GeneratorDefinitionData definition = flatDefinition();
    definition.densityGraph.nodes.front().type = "constant";
    definition.densityGraph.nodes.front().scale = 0.0f;
    definition.densityGraph.nodes.front().value = 1.0f;
    GeneratorDefinitionData::DensityNode cave;
    cave.id = "authored_cavern";
    cave.type = "constant";
    cave.value = 0.8f;
    definition.densityGraph.nodes.push_back(std::move(cave));
    definition.densityGraph.outputs.push_back(
        {"authored_caves", "authored_cavern"});
    definition.caves = {true, "authored_caves", 0.5f};

    WorldGenerator generator(registry, definition, 31u);
    ChunkBuffer buffer;
    generator.generate({0, 0, 0}, buffer);

    CHECK(std::all_of(
        buffer.blocks.begin(),
        buffer.blocks.end(),
        [](BlockState state) { return state.isAir(); }));
}

TEST_CASE(WorldGenerator_honors_enabled_structure_data) {
    BlockRegistry registry = makeRegistry();
    GeneratorDefinitionData definition = flatDefinition();
    definition.structures.enabled = true;
    definition.structures.features.push_back({
        "authored_tree",
        "rigel:tree",
        1.0f,
        1,
        1,
        {"land"}});

    WorldGenerator generator(registry, definition, 99u);
    ChunkBuffer buffer;
    generator.generate({0, 0, 0}, buffer);

    CHECK_EQ(
        buffer.at(0, 1, 0).id.type,
        registry.findByIdentifier("rigel:tree")->type);
}

TEST_CASE(WorldGenerator_clips_exact_unaligned_bounds) {
    BlockRegistry registry = makeRegistry();
    GeneratorDefinitionData definition = flatDefinition();
    definition.densityGraph.nodes.front().type = "constant";
    definition.densityGraph.nodes.front().scale = 0.0f;
    definition.densityGraph.nodes.front().value = 1.0f;
    WorldGenerator generator(registry, definition, 5u);

    ChunkBuffer bottom;
    generator.generate({0, -1, 0}, bottom);
    checkLayerAir(bottom, 0, true);
    for (int y = 1; y < Chunk::SIZE; ++y) {
        checkLayerAir(bottom, y, false);
    }

    ChunkBuffer top;
    generator.generate({0, 0, 0}, top);
    for (int y = 0; y < Chunk::SIZE - 1; ++y) {
        checkLayerAir(top, y, false);
    }
    checkLayerAir(top, Chunk::SIZE - 1, true);
}

TEST_CASE(WorldGenerator_rejects_missing_material_without_fallback) {
    BlockRegistry registry = makeRegistry();
    GeneratorDefinitionData definition = flatDefinition();
    definition.terrain.solidMaterial = "rigel:missing";

    CHECK_THROWS(WorldGenerator(registry, definition, 1u));
}

TEST_CASE(WorldGenerator_rejects_missing_semantics_identity) {
    BlockRegistry registry = makeRegistry();
    const GeneratorDefinitionData definition = flatDefinition();

    CHECK_THROWS(WorldGenerator(
        registry,
        definition,
        1u,
        0u));
}
