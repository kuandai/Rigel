#pragma once

#include "Rigel/Voxel/BlockRegistry.h"
#include "Rigel/Voxel/GeneratorDefinition.h"
#include "Rigel/Voxel/WorldGenerator.h"

#include <algorithm>
#include <memory>
#include <utility>

namespace Rigel::Test {

inline void registerGeneratorDefinitionBlocks(
    const Voxel::GeneratorDefinitionData& definition,
    Voxel::BlockRegistry& registry) {
    auto registerBlock = [&registry](const std::string& identifier) {
        if (identifier.empty() || registry.hasIdentifier(identifier)) {
            return;
        }
        Voxel::BlockType block;
        block.identifier = identifier;
        registry.registerBlock(identifier, std::move(block));
    };

    registerBlock(definition.terrain.solidMaterial);
    registerBlock(definition.terrain.waterMaterial);
    for (const auto& biome : definition.biomes.entries) {
        for (const auto& layer : biome.surface) {
            registerBlock(layer.material);
        }
    }
    for (const auto& feature : definition.structures.features) {
        registerBlock(feature.material);
    }
}

inline Voxel::GeneratorDefinitionData::Noise generatorTestNoise() {
    return {
        .octaves = 1,
        .frequency = 0.01f,
        .lacunarity = 2.0f,
        .persistence = 0.5f,
        .scale = 1.0f,
        .offset = 0.0f};
}

inline Voxel::GeneratorDefinitionData generatorDefinitionFixture(
    std::string solidMaterial = "base:air",
    std::string surfaceMaterial = "base:air",
    std::string waterMaterial = "base:air") {
    Voxel::GeneratorDefinitionData data;
    data.bounds = {-64, 320};
    data.terrain = {
        .seaLevel = 60,
        .solidMaterial = std::move(solidMaterial),
        .waterMaterial = std::move(waterMaterial),
        .densityOutput = "terrain"};
    const auto noise = generatorTestNoise();
    data.climate.global = {noise, noise, noise};
    data.climate.local = {noise, noise, noise};
    data.climate.localBlend = 0.0f;
    data.biomes.blendPower = 2.0f;
    data.biomes.epsilon = 0.0001f;
    data.biomes.coast = {"coast", -100.0f, -99.0f};
    Voxel::GeneratorDefinitionData::Biome biome;
    biome.id = "land";
    biome.weight = 1.0f;
    biome.surface.push_back({data.terrain.solidMaterial, 1});
    if (!surfaceMaterial.empty()) {
        biome.surface.front().material = std::move(surfaceMaterial);
    }
    data.biomes.entries.push_back(std::move(biome));
    Voxel::GeneratorDefinitionData::Biome coast;
    coast.id = "coast";
    coast.weight = 1.0f;
    coast.surface.push_back(
        {data.biomes.entries.front().surface.front().material, 1});
    data.biomes.entries.push_back(std::move(coast));
    Voxel::GeneratorDefinitionData::DensityNode density;
    density.id = "ground";
    density.type = "constant";
    density.value = 0.75f;
    data.densityGraph.nodes.push_back(std::move(density));
    data.densityGraph.outputs.push_back({"terrain", "ground"});
    data.caves.enabled = false;
    data.structures.enabled = false;
    return data;
}

inline Voxel::PreparedGeneratorDefinitionSnapshot preparedGeneratorFixture(
    const Voxel::GeneratorDefinitionData& data,
    Voxel::BlockRegistry& registry,
    std::string id = "rigel:test_generator",
    uint32_t revision = 1) {
    registerGeneratorDefinitionBlocks(data, registry);
    Voxel::GeneratorDefinition definition;
    definition.schemaVersion = Voxel::kGeneratorDefinitionSchemaVersion;
    definition.id = std::move(id);
    definition.sourceRevision = revision;
    definition.label = "Test Generator";
    definition.description = "Generator fixture for focused tests.";
    definition.data = data;
    return Voxel::prepareGeneratorDefinitionSnapshot(
        definition,
        registry,
        Voxel::GeneratorDefinitionOrigin::Shipped);
}

inline std::shared_ptr<Voxel::WorldGenerator> makeWorldGeneratorFixture(
    Voxel::BlockRegistry& registry,
    Voxel::GeneratorDefinitionData definition,
    uint32_t seed,
    uint32_t semanticsVersion = Voxel::kGeneratorSemanticsVersion) {
    registerGeneratorDefinitionBlocks(definition, registry);
    return std::make_shared<Voxel::WorldGenerator>(
        registry,
        std::move(definition),
        seed,
        semanticsVersion);
}

} // namespace Rigel::Test
