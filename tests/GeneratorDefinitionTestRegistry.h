#pragma once

#include "Rigel/Voxel/BlockRegistry.h"
#include "Rigel/Voxel/GeneratorDefinition.h"
#include "Rigel/Voxel/WorldGenConfig.h"
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
    data.biomes.coast = {"land", -100.0f, 100.0f};
    Voxel::GeneratorDefinitionData::Biome biome;
    biome.id = "land";
    biome.weight = 1.0f;
    biome.surface.push_back({data.terrain.solidMaterial, 1});
    if (!surfaceMaterial.empty()) {
        biome.surface.front().material = std::move(surfaceMaterial);
    }
    data.biomes.entries.push_back(std::move(biome));
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

inline Voxel::GeneratorDefinitionData strictGeneratorDefinitionFixture(
    const Voxel::WorldGenConfig& config) {
    Voxel::GeneratorDefinitionData data;
    data.bounds = {config.world.minY, config.world.maxY};
    data.terrain = {
        .seaLevel = std::clamp(
            config.world.seaLevel, config.world.minY, config.world.maxY),
        .solidMaterial = config.solidBlock,
        .waterMaterial = config.waterBlock,
        .densityOutput = "terrain"};

    const auto convertNoise = [](const Voxel::WorldGenConfig::NoiseConfig& in) {
        return Voxel::GeneratorDefinitionData::Noise{
            .octaves = in.octaves,
            .frequency = in.frequency,
            .lacunarity = in.lacunarity,
            .persistence = in.persistence,
            .scale = in.scale,
            .offset = in.offset};
    };
    const auto convertLayer = [&convertNoise](
                                  const Voxel::WorldGenConfig::ClimateLayerConfig& in) {
        return Voxel::GeneratorDefinitionData::ClimateLayer{
            .temperature = convertNoise(in.temperature),
            .humidity = convertNoise(in.humidity),
            .continentalness = convertNoise(in.continentalness)};
    };
    data.climate = {
        .latitudeScale = std::max(config.climate.latitudeScale, 0.0f),
        .latitudeStrength = config.climate.latitudeStrength,
        .localBlend = std::clamp(config.climate.localBlend, 0.0f, 1.0f),
        .global = convertLayer(config.climate.global),
        .local = convertLayer(config.climate.local)};
    data.biomes.blendPower = config.biomes.blend.blendPower;
    data.biomes.epsilon = config.biomes.blend.epsilon;
    for (const auto& legacyBiome : config.biomes.entries) {
        Voxel::GeneratorDefinitionData::Biome biome;
        biome.id = legacyBiome.name;
        biome.target = {
            legacyBiome.target.temperature,
            legacyBiome.target.humidity,
            legacyBiome.target.continentalness};
        biome.weight = legacyBiome.weight;
        for (const auto& layer : legacyBiome.surface) {
            biome.surface.push_back({layer.block, layer.depth});
        }
        if (biome.surface.empty()) {
            biome.surface.push_back({config.surfaceBlock, 1});
        }
        data.biomes.entries.push_back(std::move(biome));
    }
    if (data.biomes.entries.empty()) {
        Voxel::GeneratorDefinitionData::Biome biome;
        biome.id = "test_default";
        biome.weight = 1.0f;
        biome.surface.push_back({config.surfaceBlock, 1});
        data.biomes.entries.push_back(std::move(biome));
    }
    data.biomes.coast = {
        config.biomes.coastBand.enabled
            ? config.biomes.coastBand.biome
            : data.biomes.entries.front().id,
        config.biomes.coastBand.enabled
            ? config.biomes.coastBand.minContinentalness
            : -100.0f,
        config.biomes.coastBand.enabled
            ? config.biomes.coastBand.maxContinentalness
            : 100.0f};

    for (const auto& legacyNode : config.densityGraph.nodes) {
        Voxel::GeneratorDefinitionData::DensityNode node;
        node.id = legacyNode.id;
        node.type = legacyNode.type;
        if (node.type == "constant") {
            node.value = legacyNode.value;
        } else if (node.type == "noise2d" || node.type == "noise3d" ||
                   node.type == "noise3d_xy") {
            node.noise = convertNoise(legacyNode.noise);
        } else if (node.type == "climate") {
            node.field = legacyNode.field;
        } else if (node.type == "y") {
            node.scale = legacyNode.scale;
            node.offset = legacyNode.offset;
        } else {
            node.inputs = legacyNode.inputs;
            if (node.type == "clamp") {
                node.minValue = legacyNode.minValue;
                node.maxValue = legacyNode.maxValue;
            } else if (node.type == "spline") {
                node.splinePoints = legacyNode.splinePoints;
            }
        }
        data.densityGraph.nodes.push_back(std::move(node));
    }
    if (data.densityGraph.nodes.empty()) {
        Voxel::GeneratorDefinitionData::DensityNode node;
        node.id = "test_height";
        node.type = "y";
        node.scale = -1.0f;
        node.offset = config.terrain.baseHeight;
        data.densityGraph.nodes.push_back(std::move(node));
    }
    for (const auto& [semantic, node] : config.densityGraph.outputs) {
        data.densityGraph.outputs.push_back({semantic, node});
    }
    const auto terrainOutput = config.densityGraph.outputs.find("base_density");
    if (terrainOutput != config.densityGraph.outputs.end()) {
        data.terrain.densityOutput = "base_density";
    } else if (!data.densityGraph.outputs.empty()) {
        data.terrain.densityOutput = data.densityGraph.outputs.front().semantic;
    } else {
        data.densityGraph.outputs.push_back({"terrain", "test_height"});
    }

    const auto cavesEnabled = config.stageEnabled.find("caves");
    const bool wantsCaves = cavesEnabled == config.stageEnabled.end() ||
        cavesEnabled->second;
    if (wantsCaves &&
        config.densityGraph.outputs.contains(config.caves.densityOutput)) {
        data.caves = {true, config.caves.densityOutput, config.caves.threshold};
    }

    const auto structuresEnabled = config.stageEnabled.find("structures");
    const bool wantsStructures = structuresEnabled == config.stageEnabled.end() ||
        structuresEnabled->second;
    data.structures.enabled = wantsStructures &&
        !config.structures.features.empty();
    if (data.structures.enabled) {
        for (const auto& legacyFeature : config.structures.features) {
            data.structures.features.push_back({
                legacyFeature.name,
                legacyFeature.block,
                legacyFeature.chance,
                legacyFeature.minHeight,
                legacyFeature.maxHeight,
                legacyFeature.biomes});
        }
    }
    return data;
}

inline std::shared_ptr<Voxel::WorldGenerator> makeWorldGeneratorFixture(
    Voxel::BlockRegistry& registry,
    const Voxel::WorldGenConfig& config) {
    Voxel::GeneratorDefinitionData definition =
        strictGeneratorDefinitionFixture(config);
    registerGeneratorDefinitionBlocks(definition, registry);
    return std::make_shared<Voxel::WorldGenerator>(
        registry,
        std::move(definition),
        config.seed,
        config.world.version);
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

inline Voxel::WorldGenConfig legacyWorldGeneratorConfigFixture(
    const Voxel::WorldGenerator& generator) {
    const auto& data = generator.definition();
    Voxel::WorldGenConfig config;
    config.seed = generator.seed();
    config.world = {
        data.bounds.minY,
        data.bounds.maxY,
        data.terrain.seaLevel,
        generator.semanticsVersion()};
    config.solidBlock = data.terrain.solidMaterial;
    config.waterBlock = data.terrain.waterMaterial;
    config.surfaceBlock = data.biomes.entries.front().surface.front().material;
    config.shoreBlock = config.surfaceBlock;
    config.biomes.entries.clear();
    for (const auto& strictBiome : data.biomes.entries) {
        Voxel::WorldGenConfig::BiomeConfig biome;
        biome.name = strictBiome.id;
        biome.target = {
            strictBiome.target.temperature,
            strictBiome.target.humidity,
            strictBiome.target.continentalness};
        biome.weight = strictBiome.weight;
        for (const auto& layer : strictBiome.surface) {
            biome.surface.push_back({layer.material, layer.depth});
        }
        config.biomes.entries.push_back(std::move(biome));
    }
    config.biomes.blend = {data.biomes.blendPower, data.biomes.epsilon};
    config.biomes.coastBand = {
        data.biomes.coast.biome,
        data.biomes.coast.minContinentalness,
        data.biomes.coast.maxContinentalness,
        true};
    for (const auto& strictNode : data.densityGraph.nodes) {
        Voxel::WorldGenConfig::DensityNodeConfig node;
        node.id = strictNode.id;
        node.type = strictNode.type;
        node.inputs = strictNode.inputs;
        node.field = strictNode.field;
        node.noise = {
            strictNode.noise.octaves,
            strictNode.noise.frequency,
            strictNode.noise.lacunarity,
            strictNode.noise.persistence,
            strictNode.noise.scale,
            strictNode.noise.offset};
        node.value = strictNode.value;
        node.minValue = strictNode.minValue;
        node.maxValue = strictNode.maxValue;
        node.scale = strictNode.scale;
        node.offset = strictNode.offset;
        node.splinePoints = strictNode.splinePoints;
        config.densityGraph.nodes.push_back(std::move(node));
    }
    for (const auto& output : data.densityGraph.outputs) {
        config.densityGraph.outputs.emplace(output.semantic, output.node);
    }
    config.caves = {data.caves.densityOutput, data.caves.threshold};
    config.stageEnabled["caves"] = data.caves.enabled;
    config.stageEnabled["structures"] = data.structures.enabled;
    config.stageEnabled["surface_rules"] = true;
    for (const auto& strictFeature : data.structures.features) {
        config.structures.features.push_back({
            strictFeature.id,
            strictFeature.material,
            strictFeature.chance,
            strictFeature.minHeight,
            strictFeature.maxHeight,
            strictFeature.biomes});
    }
    return config;
}

} // namespace Rigel::Test
