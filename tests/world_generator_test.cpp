#include "TestFramework.h"
#include "GeneratorDefinitionTestRegistry.h"

#include "Rigel/Voxel/Noise.h"
#include "Rigel/Voxel/WorldGenerator.h"

#include <algorithm>
#include <limits>

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

TEST_CASE(WorldGenerator_continues_noise_driven_surface_layers_across_vertical_chunks) {
    BlockRegistry registry = makeRegistry();
    GeneratorDefinitionData definition = flatDefinition();
    definition.bounds.maxY = Chunk::SIZE + 4;

    auto& ground = definition.densityGraph.nodes.front();
    ground.scale = -10.0f;
    GeneratorDefinitionData::DensityNode variation;
    variation.id = "vertical_variation";
    variation.type = "noise3d";
    variation.noise = {
        .octaves = 1,
        .frequency = 0.1f,
        .lacunarity = 2.0f,
        .persistence = 0.5f,
        .scale = 32.0f,
        .offset = 0.0f};
    const uint32_t variationSeed =
        Noise::seedForChannel(7u, variation.id);
    const float lowerBoundaryNoise = Noise::fbm3D(
        0.0f,
        static_cast<float>(Chunk::SIZE),
        0.0f,
        variationSeed,
        variation.noise);
    const float nextGridNoise = Noise::fbm3D(
        0.0f,
        static_cast<float>(Chunk::SIZE + 4),
        0.0f,
        variationSeed,
        variation.noise);
    const float upperNoise =
        lowerBoundaryNoise + (nextGridNoise - lowerBoundaryNoise) * 0.25f;
    ground.offset = 10.0f * static_cast<float>(Chunk::SIZE + 1) -
        (lowerBoundaryNoise + upperNoise) * 0.5f;

    GeneratorDefinitionData::DensityNode terrain;
    terrain.id = "noise_driven_terrain";
    terrain.type = "add";
    terrain.inputs = {ground.id, variation.id};
    definition.densityGraph.nodes.push_back(std::move(variation));
    definition.densityGraph.nodes.push_back(std::move(terrain));
    definition.densityGraph.outputs.front().node = "noise_driven_terrain";
    definition.biomes.entries.front().surface = {
        {"rigel:grass", 1},
        {"rigel:coast_surface", 2}};

    WorldGenerator generator(registry, definition, 7u);
    ChunkBuffer upper;
    generator.generate({0, 1, 0}, upper);
    ChunkBuffer lower;
    generator.generate({0, 0, 0}, lower);

    const BlockID grass = *registry.findByIdentifier("rigel:grass");
    const BlockID lowerSurface =
        *registry.findByIdentifier("rigel:coast_surface");
    const BlockID base = *registry.findByIdentifier("rigel:stone");
    CHECK_EQ(upper.at(0, 1, 0).id.type, grass.type);
    CHECK_EQ(upper.at(0, 0, 0).id.type, lowerSurface.type);
    CHECK_EQ(lower.at(0, Chunk::SIZE - 1, 0).id.type, lowerSurface.type);
    CHECK_EQ(lower.at(0, Chunk::SIZE - 2, 0).id.type, base.type);
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

    const int lastWorldChunk =
        std::numeric_limits<int>::max() / Chunk::SIZE;
    CHECK_THROWS(generator.generate({lastWorldChunk, 0, 0}, buffer));
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

TEST_CASE(WorldGenerator_generates_at_supported_noise_frequency_boundary) {
    BlockRegistry registry = makeRegistry();
    GeneratorDefinitionData definition = flatDefinition();
    const GeneratorDefinitionData::Noise boundaryNoise{
        .octaves = 2,
        .frequency = GeneratorDefinitionData::MaxNoiseFrequency * 0.5f,
        .lacunarity = 2.0f,
        .persistence = 0.5f,
        .scale = 0.0f,
        .offset = 0.0f};
    definition.climate.global = {
        boundaryNoise, boundaryNoise, boundaryNoise};
    definition.climate.local = {
        boundaryNoise, boundaryNoise, boundaryNoise};
    auto& density = definition.densityGraph.nodes.front();
    density.type = "noise3d";
    density.noise = boundaryNoise;
    density.scale = 0.0f;
    density.offset = 0.0f;

    WorldGenerator generator(registry, definition, 19u);
    ChunkBuffer buffer;
    CHECK_NO_THROW(generator.generate({0, 0, 0}, buffer));
    const int lastWorldChunk =
        std::numeric_limits<int>::max() / Chunk::SIZE;
    CHECK_NO_THROW(generator.generate({lastWorldChunk, 0, 0}, buffer));
    CHECK_THROWS(generator.generate({lastWorldChunk + 1, 0, 0}, buffer));
}

TEST_CASE(Noise_checks_lattice_coordinate_range_before_integer_conversion) {
    const float safe = static_cast<float>(std::numeric_limits<int>::max()) *
        GeneratorDefinitionData::MaxNoiseFrequency;
    CHECK_NO_THROW(Noise::noise2D(safe, -safe, 3u));
    CHECK_NO_THROW(Noise::noise3D(safe, 0.0f, -safe, 3u));

    const float outside = std::numeric_limits<float>::max();
    CHECK_THROWS(Noise::noise2D(outside, 0.0f, 3u));
    CHECK_THROWS(Noise::noise3D(0.0f, outside, 0.0f, 3u));
}
