#include "Rigel/Voxel/GeneratorDefinition.h"

#include "Rigel/Util/Ryml.h"
#include "Rigel/Voxel/BlockRegistry.h"

#include <ryml.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <functional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace Rigel::Voxel {
namespace {

[[noreturn]] void fail(std::string_view sourceName,
                       std::string_view path,
                       std::string_view reason) {
    throw std::invalid_argument(
        "Invalid generator definition field '" + std::string(path) +
        "' in '" + std::string(sourceName) + "': " + std::string(reason));
}

bool contains(std::initializer_list<std::string_view> values,
              std::string_view value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

void requireMap(ryml::ConstNodeRef node,
                std::string_view sourceName,
                std::string_view path,
                std::initializer_list<std::string_view> fields) {
    if (!node.readable() || !node.is_map()) {
        fail(sourceName, path, "expected a mapping");
    }

    std::unordered_set<std::string> encountered;
    for (const ryml::ConstNodeRef child : node.children()) {
        const std::string key = Util::toStdString(child.key());
        if (!encountered.insert(key).second) {
            fail(sourceName, std::string(path) + "." + key,
                 "duplicate field");
        }
        if (!contains(fields, key)) {
            fail(sourceName, std::string(path) + "." + key,
                 "unknown field");
        }
    }
    for (const std::string_view field : fields) {
        if (!node.has_child(ryml::csubstr(field.data(), field.size()))) {
            fail(sourceName, std::string(path) + "." + std::string(field),
                 "missing required field");
        }
    }
}

ryml::ConstNodeRef child(ryml::ConstNodeRef node, std::string_view key) {
    return node[ryml::csubstr(key.data(), key.size())];
}

std::string scalarText(ryml::ConstNodeRef node,
                       std::string_view sourceName,
                       std::string_view path) {
    if (!node.readable() || !node.has_val() || node.has_children()) {
        fail(sourceName, path, "expected a scalar");
    }
    return Util::toStdString(node.val());
}

std::string readString(ryml::ConstNodeRef node,
                       std::string_view key,
                       std::string_view sourceName,
                       std::string_view path) {
    return scalarText(
        child(node, key), sourceName,
        std::string(path) + "." + std::string(key));
}

template <typename T>
T readInteger(ryml::ConstNodeRef node,
              std::string_view key,
              std::string_view sourceName,
              std::string_view path) {
    const std::string fullPath =
        std::string(path) + "." + std::string(key);
    const std::string value = scalarText(child(node, key), sourceName, fullPath);
    T parsed{};
    const auto result = std::from_chars(
        value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) {
        fail(sourceName, fullPath, "expected an integer");
    }
    return parsed;
}

float readFloatValue(ryml::ConstNodeRef node,
                     std::string_view sourceName,
                     std::string_view path) {
    const std::string value = scalarText(node, sourceName, path);
    float parsed = 0.0f;
    const auto result = std::from_chars(
        value.data(), value.data() + value.size(), parsed,
        std::chars_format::general);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
        !std::isfinite(parsed)) {
        fail(sourceName, path, "expected a finite number");
    }
    return parsed;
}

float readFloat(ryml::ConstNodeRef node,
                std::string_view key,
                std::string_view sourceName,
                std::string_view path) {
    return readFloatValue(
        child(node, key), sourceName,
        std::string(path) + "." + std::string(key));
}

bool readBool(ryml::ConstNodeRef node,
              std::string_view key,
              std::string_view sourceName,
              std::string_view path) {
    const std::string fullPath =
        std::string(path) + "." + std::string(key);
    const std::string value = scalarText(child(node, key), sourceName, fullPath);
    if (value == "true") {
        return true;
    }
    if (value == "false") {
        return false;
    }
    fail(sourceName, fullPath, "expected boolean 'true' or 'false'");
}

void requireSequence(ryml::ConstNodeRef node,
                     std::string_view sourceName,
                     std::string_view path) {
    if (!node.readable() || !node.is_seq()) {
        fail(sourceName, path, "expected a sequence");
    }
}

GeneratorDefinitionData::Noise parseNoise(ryml::ConstNodeRef node,
                                          std::string_view sourceName,
                                          std::string_view path) {
    requireMap(node, sourceName, path,
               {"octaves", "frequency", "lacunarity", "persistence",
                "scale", "offset"});
    GeneratorDefinitionData::Noise result;
    result.octaves = readInteger<int>(node, "octaves", sourceName, path);
    result.frequency = readFloat(node, "frequency", sourceName, path);
    result.lacunarity = readFloat(node, "lacunarity", sourceName, path);
    result.persistence = readFloat(node, "persistence", sourceName, path);
    result.scale = readFloat(node, "scale", sourceName, path);
    result.offset = readFloat(node, "offset", sourceName, path);
    return result;
}

GeneratorDefinitionData::ClimateLayer parseClimateLayer(
    ryml::ConstNodeRef node,
    std::string_view sourceName,
    std::string_view path) {
    requireMap(node, sourceName, path,
               {"temperature", "humidity", "continentalness"});
    GeneratorDefinitionData::ClimateLayer result;
    result.temperature = parseNoise(
        child(node, "temperature"), sourceName,
        std::string(path) + ".temperature");
    result.humidity = parseNoise(
        child(node, "humidity"), sourceName,
        std::string(path) + ".humidity");
    result.continentalness = parseNoise(
        child(node, "continentalness"), sourceName,
        std::string(path) + ".continentalness");
    return result;
}

std::vector<std::string> parseStringSequence(
    ryml::ConstNodeRef node,
    std::string_view sourceName,
    std::string_view path,
    size_t maximum) {
    requireSequence(node, sourceName, path);
    if (node.num_children() > maximum) {
        fail(sourceName, path,
             "contains more than " + std::to_string(maximum) + " entries");
    }
    std::vector<std::string> result;
    result.reserve(node.num_children());
    size_t index = 0;
    for (const ryml::ConstNodeRef entry : node.children()) {
        result.push_back(scalarText(
            entry, sourceName,
            std::string(path) + "[" + std::to_string(index) + "]"));
        ++index;
    }
    return result;
}

GeneratorDefinitionData::DensityNode parseDensityNode(
    ryml::ConstNodeRef node,
    std::string_view sourceName,
    std::string_view path) {
    if (!node.readable() || !node.is_map()) {
        fail(sourceName, path, "expected a mapping");
    }
    if (!node.has_child("id") || !node.has_child("type")) {
        fail(sourceName, path, "requires 'id' and 'type'");
    }

    const std::string type = readString(node, "type", sourceName, path);
    if (type == "constant") {
        requireMap(node, sourceName, path, {"id", "type", "value"});
    } else if (type == "noise2d" || type == "noise3d" ||
               type == "noise3d_xy") {
        requireMap(node, sourceName, path,
                   {"id", "type", "noise", "scale", "offset"});
    } else if (type == "add" || type == "mul" || type == "max" ||
               type == "min" || type == "abs" || type == "invert") {
        requireMap(node, sourceName, path, {"id", "type", "inputs"});
    } else if (type == "clamp") {
        requireMap(node, sourceName, path,
                   {"id", "type", "inputs", "min", "max"});
    } else if (type == "spline") {
        requireMap(node, sourceName, path,
                   {"id", "type", "inputs", "spline"});
    } else if (type == "climate") {
        requireMap(node, sourceName, path, {"id", "type", "field"});
    } else if (type == "y") {
        requireMap(node, sourceName, path,
                   {"id", "type", "scale", "offset"});
    } else {
        fail(sourceName, std::string(path) + ".type",
             "unknown density node type '" + type + "'");
    }

    GeneratorDefinitionData::DensityNode result;
    result.id = readString(node, "id", sourceName, path);
    result.type = type;
    if (type == "constant") {
        result.value = readFloat(node, "value", sourceName, path);
    } else if (type == "noise2d" || type == "noise3d" ||
               type == "noise3d_xy") {
        result.noise = parseNoise(
            child(node, "noise"), sourceName, std::string(path) + ".noise");
        result.scale = readFloat(node, "scale", sourceName, path);
        result.offset = readFloat(node, "offset", sourceName, path);
    } else if (type == "add" || type == "mul" || type == "max" ||
               type == "min" || type == "abs" || type == "invert" ||
               type == "clamp" || type == "spline") {
        result.inputs = parseStringSequence(
            child(node, "inputs"), sourceName, std::string(path) + ".inputs",
            GeneratorDefinitionData::MaxDensityNodeInputs);
    }
    if (type == "clamp") {
        result.minValue = readFloat(node, "min", sourceName, path);
        result.maxValue = readFloat(node, "max", sourceName, path);
    } else if (type == "spline") {
        const std::string splinePath = std::string(path) + ".spline";
        const ryml::ConstNodeRef spline = child(node, "spline");
        requireSequence(spline, sourceName, splinePath);
        if (spline.num_children() >
            GeneratorDefinitionData::MaxDensitySplinePoints) {
            fail(sourceName, splinePath, "contains too many points");
        }
        size_t pointIndex = 0;
        for (const ryml::ConstNodeRef point : spline.children()) {
            const std::string pointPath =
                splinePath + "[" + std::to_string(pointIndex) + "]";
            requireSequence(point, sourceName, pointPath);
            if (point.num_children() != 2) {
                fail(sourceName, pointPath, "expected exactly two coordinates");
            }
            result.splinePoints.emplace_back(
                readFloatValue(point[0], sourceName, pointPath + "[0]"),
                readFloatValue(point[1], sourceName, pointPath + "[1]"));
            ++pointIndex;
        }
    } else if (type == "climate") {
        result.field = readString(node, "field", sourceName, path);
    } else if (type == "y") {
        result.scale = readFloat(node, "scale", sourceName, path);
        result.offset = readFloat(node, "offset", sourceName, path);
    }
    return result;
}

GeneratorDefinitionData parseData(ryml::ConstNodeRef node,
                                  std::string_view sourceName,
                                  std::string_view path,
                                  bool exactRoot) {
    if (exactRoot) {
        requireMap(node, sourceName, path,
                   {"bounds", "terrain", "climate", "biomes",
                    "density_graph", "caves", "structures"});
    }
    GeneratorDefinitionData result;

    const ryml::ConstNodeRef bounds = child(node, "bounds");
    requireMap(bounds, sourceName, std::string(path) + ".bounds",
               {"min_y", "max_y"});
    result.bounds.minY = readInteger<int>(
        bounds, "min_y", sourceName, std::string(path) + ".bounds");
    result.bounds.maxY = readInteger<int>(
        bounds, "max_y", sourceName, std::string(path) + ".bounds");

    const ryml::ConstNodeRef terrain = child(node, "terrain");
    requireMap(terrain, sourceName, std::string(path) + ".terrain",
               {"sea_level", "solid_material", "water_material",
                "density_output"});
    result.terrain.seaLevel = readInteger<int>(
        terrain, "sea_level", sourceName, std::string(path) + ".terrain");
    result.terrain.solidMaterial = readString(
        terrain, "solid_material", sourceName, std::string(path) + ".terrain");
    result.terrain.waterMaterial = readString(
        terrain, "water_material", sourceName, std::string(path) + ".terrain");
    result.terrain.densityOutput = readString(
        terrain, "density_output", sourceName, std::string(path) + ".terrain");

    const ryml::ConstNodeRef climate = child(node, "climate");
    requireMap(climate, sourceName, std::string(path) + ".climate",
               {"latitude_scale", "latitude_strength", "local_blend",
                "global", "local"});
    result.climate.latitudeScale = readFloat(
        climate, "latitude_scale", sourceName, std::string(path) + ".climate");
    result.climate.latitudeStrength = readFloat(
        climate, "latitude_strength", sourceName,
        std::string(path) + ".climate");
    result.climate.localBlend = readFloat(
        climate, "local_blend", sourceName, std::string(path) + ".climate");
    result.climate.global = parseClimateLayer(
        child(climate, "global"), sourceName,
        std::string(path) + ".climate.global");
    result.climate.local = parseClimateLayer(
        child(climate, "local"), sourceName,
        std::string(path) + ".climate.local");

    const ryml::ConstNodeRef biomes = child(node, "biomes");
    requireMap(biomes, sourceName, std::string(path) + ".biomes",
               {"blend_power", "epsilon", "coast", "entries"});
    result.biomes.blendPower = readFloat(
        biomes, "blend_power", sourceName, std::string(path) + ".biomes");
    result.biomes.epsilon = readFloat(
        biomes, "epsilon", sourceName, std::string(path) + ".biomes");
    const ryml::ConstNodeRef coast = child(biomes, "coast");
    requireMap(coast, sourceName, std::string(path) + ".biomes.coast",
               {"biome", "min_continentalness", "max_continentalness"});
    result.biomes.coast.biome = readString(
        coast, "biome", sourceName, std::string(path) + ".biomes.coast");
    result.biomes.coast.minContinentalness = readFloat(
        coast, "min_continentalness", sourceName,
        std::string(path) + ".biomes.coast");
    result.biomes.coast.maxContinentalness = readFloat(
        coast, "max_continentalness", sourceName,
        std::string(path) + ".biomes.coast");

    const ryml::ConstNodeRef biomeEntries = child(biomes, "entries");
    const std::string biomeEntriesPath = std::string(path) + ".biomes.entries";
    requireSequence(biomeEntries, sourceName, biomeEntriesPath);
    if (biomeEntries.num_children() > GeneratorDefinitionData::MaxBiomeEntries) {
        fail(sourceName, biomeEntriesPath, "contains too many entries");
    }
    size_t biomeIndex = 0;
    for (const ryml::ConstNodeRef biome : biomeEntries.children()) {
        const std::string biomePath =
            biomeEntriesPath + "[" + std::to_string(biomeIndex) + "]";
        requireMap(biome, sourceName, biomePath,
                   {"id", "target", "weight", "water_fill", "surface"});
        GeneratorDefinitionData::Biome parsed;
        parsed.id = readString(biome, "id", sourceName, biomePath);
        parsed.weight = readFloat(biome, "weight", sourceName, biomePath);
        parsed.waterFill = readBool(
            biome, "water_fill", sourceName, biomePath);
        const ryml::ConstNodeRef target = child(biome, "target");
        requireMap(target, sourceName, biomePath + ".target",
                   {"temperature", "humidity", "continentalness"});
        parsed.target.temperature = readFloat(
            target, "temperature", sourceName, biomePath + ".target");
        parsed.target.humidity = readFloat(
            target, "humidity", sourceName, biomePath + ".target");
        parsed.target.continentalness = readFloat(
            target, "continentalness", sourceName, biomePath + ".target");
        const ryml::ConstNodeRef surface = child(biome, "surface");
        requireSequence(surface, sourceName, biomePath + ".surface");
        if (surface.num_children() > GeneratorDefinitionData::MaxSurfaceLayers) {
            fail(sourceName, biomePath + ".surface", "contains too many layers");
        }
        size_t layerIndex = 0;
        for (const ryml::ConstNodeRef layer : surface.children()) {
            const std::string layerPath = biomePath + ".surface[" +
                std::to_string(layerIndex) + "]";
            requireMap(layer, sourceName, layerPath, {"material", "depth"});
            parsed.surface.push_back({
                readString(layer, "material", sourceName, layerPath),
                readInteger<int>(layer, "depth", sourceName, layerPath)});
            ++layerIndex;
        }
        result.biomes.entries.push_back(std::move(parsed));
        ++biomeIndex;
    }

    const ryml::ConstNodeRef graph = child(node, "density_graph");
    requireMap(graph, sourceName, std::string(path) + ".density_graph",
               {"outputs", "nodes"});
    const ryml::ConstNodeRef outputs = child(graph, "outputs");
    const std::string outputsPath = std::string(path) + ".density_graph.outputs";
    if (!outputs.readable() || !outputs.is_map()) {
        fail(sourceName, outputsPath, "expected a mapping");
    }
    if (outputs.num_children() > GeneratorDefinitionData::MaxDensityGraphOutputs) {
        fail(sourceName, outputsPath, "contains too many outputs");
    }
    std::unordered_set<std::string> outputNames;
    for (const ryml::ConstNodeRef output : outputs.children()) {
        const std::string semantic = Util::toStdString(output.key());
        if (!outputNames.insert(semantic).second) {
            fail(sourceName, outputsPath + "." + semantic,
                 "duplicate output semantic");
        }
        result.densityGraph.outputs.push_back({
            semantic,
            scalarText(output, sourceName, outputsPath + "." + semantic)});
    }
    const ryml::ConstNodeRef nodes = child(graph, "nodes");
    const std::string nodesPath = std::string(path) + ".density_graph.nodes";
    requireSequence(nodes, sourceName, nodesPath);
    if (nodes.num_children() > GeneratorDefinitionData::MaxDensityGraphNodes) {
        fail(sourceName, nodesPath, "contains too many nodes");
    }
    size_t nodeIndex = 0;
    for (const ryml::ConstNodeRef densityNode : nodes.children()) {
        result.densityGraph.nodes.push_back(parseDensityNode(
            densityNode, sourceName,
            nodesPath + "[" + std::to_string(nodeIndex) + "]"));
        ++nodeIndex;
    }

    const ryml::ConstNodeRef caves = child(node, "caves");
    const std::string cavesPath = std::string(path) + ".caves";
    if (!caves.readable() || !caves.is_map() || !caves.has_child("enabled")) {
        fail(sourceName, cavesPath, "requires an 'enabled' field");
    }
    result.caves.enabled = readBool(caves, "enabled", sourceName, cavesPath);
    if (result.caves.enabled) {
        requireMap(caves, sourceName, cavesPath,
                   {"enabled", "density_output", "threshold"});
        result.caves.densityOutput = readString(
            caves, "density_output", sourceName, cavesPath);
        result.caves.threshold = readFloat(
            caves, "threshold", sourceName, cavesPath);
    } else {
        requireMap(caves, sourceName, cavesPath, {"enabled"});
    }

    const ryml::ConstNodeRef structures = child(node, "structures");
    const std::string structuresPath = std::string(path) + ".structures";
    if (!structures.readable() || !structures.is_map() ||
        !structures.has_child("enabled")) {
        fail(sourceName, structuresPath, "requires an 'enabled' field");
    }
    result.structures.enabled = readBool(
        structures, "enabled", sourceName, structuresPath);
    if (!result.structures.enabled) {
        requireMap(structures, sourceName, structuresPath, {"enabled"});
        return result;
    }

    requireMap(structures, sourceName, structuresPath, {"enabled", "features"});
    const ryml::ConstNodeRef features = child(structures, "features");
    const std::string featuresPath = structuresPath + ".features";
    requireSequence(features, sourceName, featuresPath);
    if (features.num_children() >
        GeneratorDefinitionData::MaxStructureFeatures) {
        fail(sourceName, featuresPath, "contains too many features");
    }
    size_t featureIndex = 0;
    for (const ryml::ConstNodeRef feature : features.children()) {
        const std::string featurePath =
            featuresPath + "[" + std::to_string(featureIndex) + "]";
        requireMap(feature, sourceName, featurePath,
                   {"id", "material", "chance", "min_height", "max_height",
                    "biomes"});
        GeneratorDefinitionData::StructureFeature parsed;
        parsed.id = readString(feature, "id", sourceName, featurePath);
        parsed.material = readString(
            feature, "material", sourceName, featurePath);
        parsed.chance = readFloat(feature, "chance", sourceName, featurePath);
        parsed.minHeight = readInteger<int>(
            feature, "min_height", sourceName, featurePath);
        parsed.maxHeight = readInteger<int>(
            feature, "max_height", sourceName, featurePath);
        parsed.biomes = parseStringSequence(
            child(feature, "biomes"), sourceName, featurePath + ".biomes",
            GeneratorDefinitionData::MaxFeatureBiomeFilters);
        result.structures.features.push_back(std::move(parsed));
        ++featureIndex;
    }
    return result;
}

bool validNamespacedId(std::string_view id) {
    const size_t separator = id.find(':');
    if (separator == std::string_view::npos || separator == 0 ||
        separator + 1 == id.size()) {
        return false;
    }
    return std::none_of(id.begin(), id.end(), [](const unsigned char byte) {
        return byte <= 0x20 || byte == 0x7f;
    });
}

void requireFinite(float value,
                   std::string_view sourceName,
                   std::string_view path) {
    if (!std::isfinite(value)) {
        fail(sourceName, path, "must be finite");
    }
}

void validateNoise(const GeneratorDefinitionData::Noise& noise,
                   std::string_view sourceName,
                   std::string_view path) {
    if (noise.octaves < 1 ||
        noise.octaves > GeneratorDefinitionData::MaxNoiseOctaves) {
        fail(sourceName, std::string(path) + ".octaves",
             "must be in [1, " +
                 std::to_string(GeneratorDefinitionData::MaxNoiseOctaves) +
                 "]");
    }
    requireFinite(noise.frequency, sourceName, std::string(path) + ".frequency");
    requireFinite(noise.lacunarity, sourceName,
                  std::string(path) + ".lacunarity");
    requireFinite(noise.persistence, sourceName,
                  std::string(path) + ".persistence");
    requireFinite(noise.scale, sourceName, std::string(path) + ".scale");
    requireFinite(noise.offset, sourceName, std::string(path) + ".offset");
    if (noise.frequency <= 0.0f) {
        fail(sourceName, std::string(path) + ".frequency", "must be positive");
    }
    if (noise.lacunarity <= 0.0f) {
        fail(sourceName, std::string(path) + ".lacunarity", "must be positive");
    }
    if (noise.persistence < 0.0f || noise.persistence > 1.0f) {
        fail(sourceName, std::string(path) + ".persistence", "must be in [0, 1]");
    }
}

const GeneratorDefinitionData::DensityOutput* findOutput(
    const GeneratorDefinitionData& data,
    std::string_view semantic) {
    const auto found = std::find_if(
        data.densityGraph.outputs.begin(), data.densityGraph.outputs.end(),
        [semantic](const auto& output) { return output.semantic == semantic; });
    return found == data.densityGraph.outputs.end() ? nullptr : &*found;
}

void validateData(const GeneratorDefinitionData& data,
                  std::string_view sourceName) {
    if (data.bounds.minY < GeneratorDefinitionData::MinWorldY ||
        data.bounds.maxY > GeneratorDefinitionData::MaxWorldY ||
        data.bounds.minY >= data.bounds.maxY ||
        data.bounds.maxY - data.bounds.minY + 1 >
            GeneratorDefinitionData::MaxWorldHeight) {
        fail(sourceName, "generator.bounds", "invalid finite world bounds");
    }
    if (data.terrain.seaLevel < data.bounds.minY ||
        data.terrain.seaLevel > data.bounds.maxY) {
        fail(sourceName, "generator.terrain.sea_level",
             "must be inside generator bounds");
    }
    for (const auto& [value, path] : {
             std::pair{data.climate.latitudeScale,
                       std::string_view("generator.climate.latitude_scale")},
             std::pair{data.climate.latitudeStrength,
                       std::string_view("generator.climate.latitude_strength")},
             std::pair{data.climate.localBlend,
                       std::string_view("generator.climate.local_blend")}}) {
        requireFinite(value, sourceName, path);
    }
    if (data.climate.latitudeScale < 0.0f) {
        fail(sourceName, "generator.climate.latitude_scale",
             "must be non-negative");
    }
    if (data.climate.localBlend < 0.0f || data.climate.localBlend > 1.0f) {
        fail(sourceName, "generator.climate.local_blend", "must be in [0, 1]");
    }
    auto validateLayer = [&](const auto& layer, std::string_view path) {
        validateNoise(layer.temperature, sourceName,
                      std::string(path) + ".temperature");
        validateNoise(layer.humidity, sourceName,
                      std::string(path) + ".humidity");
        validateNoise(layer.continentalness, sourceName,
                      std::string(path) + ".continentalness");
    };
    validateLayer(data.climate.global, "generator.climate.global");
    validateLayer(data.climate.local, "generator.climate.local");

    requireFinite(data.biomes.blendPower, sourceName,
                  "generator.biomes.blend_power");
    requireFinite(data.biomes.epsilon, sourceName, "generator.biomes.epsilon");
    if (data.biomes.blendPower <= 0.0f || data.biomes.epsilon <= 0.0f) {
        fail(sourceName, "generator.biomes",
             "blend_power and epsilon must be positive");
    }
    requireFinite(data.biomes.coast.minContinentalness, sourceName,
                  "generator.biomes.coast.min_continentalness");
    requireFinite(data.biomes.coast.maxContinentalness, sourceName,
                  "generator.biomes.coast.max_continentalness");
    if (data.biomes.coast.minContinentalness >
        data.biomes.coast.maxContinentalness) {
        fail(sourceName, "generator.biomes.coast",
             "min_continentalness must not exceed max_continentalness");
    }
    if (data.biomes.entries.empty()) {
        fail(sourceName, "generator.biomes.entries",
             "requires at least one biome");
    }
    if (data.biomes.entries.size() > GeneratorDefinitionData::MaxBiomeEntries) {
        fail(sourceName, "generator.biomes.entries", "contains too many biomes");
    }
    std::unordered_set<std::string> biomeIds;
    for (size_t index = 0; index < data.biomes.entries.size(); ++index) {
        const auto& biome = data.biomes.entries[index];
        const std::string path =
            "generator.biomes.entries[" + std::to_string(index) + "]";
        if (biome.id.empty() || !biomeIds.insert(biome.id).second) {
            fail(sourceName, path + ".id", "must be non-empty and unique");
        }
        requireFinite(biome.target.temperature, sourceName,
                      path + ".target.temperature");
        requireFinite(biome.target.humidity, sourceName,
                      path + ".target.humidity");
        requireFinite(biome.target.continentalness, sourceName,
                      path + ".target.continentalness");
        requireFinite(biome.weight, sourceName, path + ".weight");
        if (biome.weight <= 0.0f) {
            fail(sourceName, path + ".weight", "must be positive");
        }
        if (biome.surface.empty()) {
            fail(sourceName, path + ".surface",
                 "requires at least one authoritative surface layer");
        }
        for (size_t layer = 0; layer < biome.surface.size(); ++layer) {
            const auto& surface = biome.surface[layer];
            if (surface.material.empty()) {
                fail(sourceName,
                     path + ".surface[" + std::to_string(layer) + "].material",
                     "must not be empty");
            }
            if (surface.depth < 1 ||
                surface.depth > GeneratorDefinitionData::MaxSurfaceDepth) {
                fail(sourceName,
                     path + ".surface[" + std::to_string(layer) + "].depth",
                     "is outside the supported range");
            }
        }
    }
    if (!biomeIds.contains(data.biomes.coast.biome)) {
        fail(sourceName, "generator.biomes.coast.biome",
             "references an unknown biome '" + data.biomes.coast.biome + "'");
    }

    if (data.densityGraph.nodes.empty()) {
        fail(sourceName, "generator.density_graph.nodes",
             "requires at least one density node");
    }
    std::unordered_map<std::string, const GeneratorDefinitionData::DensityNode*>
        nodes;
    for (size_t index = 0; index < data.densityGraph.nodes.size(); ++index) {
        const auto& node = data.densityGraph.nodes[index];
        const std::string path =
            "generator.density_graph.nodes[" + std::to_string(index) + "]";
        if (node.id.empty() || !nodes.emplace(node.id, &node).second) {
            fail(sourceName, path + ".id", "must be non-empty and unique");
        }
        const size_t inputs = node.inputs.size();
        if ((node.type == "add" || node.type == "mul" || node.type == "max" ||
             node.type == "min") && inputs == 0) {
            fail(sourceName, path + ".inputs", "requires at least one input");
        }
        if ((node.type == "abs" || node.type == "invert" ||
             node.type == "clamp" || node.type == "spline") && inputs != 1) {
            fail(sourceName, path + ".inputs", "requires exactly one input");
        }
        if (inputs > GeneratorDefinitionData::MaxDensityNodeInputs) {
            fail(sourceName, path + ".inputs", "contains too many inputs");
        }
        if (node.type == "noise2d" || node.type == "noise3d" ||
            node.type == "noise3d_xy") {
            validateNoise(node.noise, sourceName, path + ".noise");
            requireFinite(node.scale, sourceName, path + ".scale");
            requireFinite(node.offset, sourceName, path + ".offset");
        } else if (node.type == "constant") {
            requireFinite(node.value, sourceName, path + ".value");
        } else if (node.type == "clamp") {
            requireFinite(node.minValue, sourceName, path + ".min");
            requireFinite(node.maxValue, sourceName, path + ".max");
            if (node.minValue > node.maxValue) {
                fail(sourceName, path, "clamp min must not exceed max");
            }
        } else if (node.type == "spline") {
            if (node.splinePoints.empty()) {
                fail(sourceName, path + ".spline", "requires at least one point");
            }
            std::unordered_set<float> coordinates;
            for (const auto& point : node.splinePoints) {
                requireFinite(point.first, sourceName, path + ".spline.x");
                requireFinite(point.second, sourceName, path + ".spline.y");
                if (!coordinates.insert(point.first).second) {
                    fail(sourceName, path + ".spline",
                         "contains duplicate x coordinates");
                }
            }
        } else if (node.type == "climate") {
            if (node.field != "temperature" && node.field != "humidity" &&
                node.field != "continentalness") {
                fail(sourceName, path + ".field",
                     "unknown climate field '" + node.field + "'");
            }
        } else if (node.type == "y") {
            requireFinite(node.scale, sourceName, path + ".scale");
            requireFinite(node.offset, sourceName, path + ".offset");
        } else if (node.type != "add" && node.type != "mul" &&
                   node.type != "max" && node.type != "min" &&
                   node.type != "abs" && node.type != "invert") {
            fail(sourceName, path + ".type",
                 "unknown density node type '" + node.type + "'");
        }
    }

    for (const auto& [id, node] : nodes) {
        for (const auto& input : node->inputs) {
            if (!nodes.contains(input)) {
                fail(sourceName, "generator.density_graph.nodes." + id +
                         ".inputs",
                     "references an unknown node '" + input + "'");
            }
        }
    }
    std::unordered_map<std::string, int> visitState;
    std::function<void(const std::string&)> visit = [&](const std::string& id) {
        if (visitState[id] == 1) {
            fail(sourceName, "generator.density_graph.nodes." + id,
                 "density graph contains a cycle");
        }
        if (visitState[id] == 2) {
            return;
        }
        visitState[id] = 1;
        for (const auto& input : nodes.at(id)->inputs) {
            visit(input);
        }
        visitState[id] = 2;
    };
    for (const auto& [id, node] : nodes) {
        static_cast<void>(node);
        visit(id);
    }

    if (data.densityGraph.outputs.empty()) {
        fail(sourceName, "generator.density_graph.outputs",
             "requires at least one semantic output");
    }
    std::unordered_set<std::string> outputNames;
    for (const auto& output : data.densityGraph.outputs) {
        if (output.semantic.empty() ||
            !outputNames.insert(output.semantic).second) {
            fail(sourceName, "generator.density_graph.outputs",
                 "output semantics must be non-empty and unique");
        }
        if (!nodes.contains(output.node)) {
            fail(sourceName,
                 "generator.density_graph.outputs." + output.semantic,
                 "references an unknown node '" + output.node + "'");
        }
    }
    if (data.terrain.densityOutput.empty() ||
        !findOutput(data, data.terrain.densityOutput)) {
        fail(sourceName, "generator.terrain.density_output",
             "references a missing semantic output");
    }
    if (data.terrain.solidMaterial.empty() ||
        data.terrain.waterMaterial.empty()) {
        fail(sourceName, "generator.terrain",
             "solid_material and water_material must not be empty");
    }

    std::unordered_set<std::string> consumedOutputs = {
        data.terrain.densityOutput};
    if (data.caves.enabled) {
        requireFinite(data.caves.threshold, sourceName,
                      "generator.caves.threshold");
        if (data.caves.densityOutput.empty() ||
            !findOutput(data, data.caves.densityOutput)) {
            fail(sourceName, "generator.caves.density_output",
                 "references a missing semantic output");
        }
        consumedOutputs.insert(data.caves.densityOutput);
    } else if (!data.caves.densityOutput.empty() ||
               data.caves.threshold != 0.0f) {
        fail(sourceName, "generator.caves",
             "disabled caves cannot retain cave-specific data");
    }
    for (const auto& output : data.densityGraph.outputs) {
        if (!consumedOutputs.contains(output.semantic)) {
            fail(sourceName,
                 "generator.density_graph.outputs." + output.semantic,
                 "semantic output is not consumed by terrain or caves");
        }
    }

    if (!data.structures.enabled) {
        if (!data.structures.features.empty()) {
            fail(sourceName, "generator.structures",
                 "disabled structures cannot retain feature data");
        }
        return;
    }
    if (data.structures.features.empty()) {
        fail(sourceName, "generator.structures.features",
             "enabled structures require at least one feature");
    }
    std::unordered_set<std::string> featureIds;
    for (size_t index = 0; index < data.structures.features.size(); ++index) {
        const auto& feature = data.structures.features[index];
        const std::string path =
            "generator.structures.features[" + std::to_string(index) + "]";
        if (feature.id.empty() || !featureIds.insert(feature.id).second) {
            fail(sourceName, path + ".id", "must be non-empty and unique");
        }
        if (feature.material.empty()) {
            fail(sourceName, path + ".material", "must not be empty");
        }
        requireFinite(feature.chance, sourceName, path + ".chance");
        if (feature.chance <= 0.0f || feature.chance > 1.0f) {
            fail(sourceName, path + ".chance", "must be in (0, 1]");
        }
        if (feature.minHeight < 1 ||
            feature.maxHeight > GeneratorDefinitionData::MaxStructureHeight ||
            feature.minHeight > feature.maxHeight) {
            fail(sourceName, path,
                 "feature height range is invalid or reversed");
        }
        std::unordered_set<std::string> filters;
        for (const auto& biome : feature.biomes) {
            if (!biomeIds.contains(biome)) {
                fail(sourceName, path + ".biomes",
                     "references an unknown biome '" + biome + "'");
            }
            if (!filters.insert(biome).second) {
                fail(sourceName, path + ".biomes",
                     "contains duplicate biome '" + biome + "'");
            }
        }
    }
}

} // namespace

GeneratorDefinition parseGeneratorDefinition(std::string_view yaml,
                                             std::string_view sourceName) {
    if (yaml.empty()) {
        throw std::invalid_argument(
            "Generator definition '" + std::string(sourceName) + "' is empty");
    }
    const ryml::Tree tree = ryml::parse_in_arena(
        ryml::csubstr(sourceName.data(), sourceName.size()),
        ryml::csubstr(yaml.data(), yaml.size()));
    const ryml::ConstNodeRef root = tree.crootref();
    requireMap(root, sourceName, "document", {"generator"});
    const ryml::ConstNodeRef generator = child(root, "generator");
    requireMap(generator, sourceName, "generator",
               {"schema_version", "id", "source_revision", "label",
                "description", "bounds", "terrain", "climate", "biomes",
                "density_graph", "caves", "structures"});

    GeneratorDefinition result;
    result.schemaVersion = readInteger<uint32_t>(
        generator, "schema_version", sourceName, "generator");
    result.id = readString(generator, "id", sourceName, "generator");
    result.sourceRevision = readInteger<uint32_t>(
        generator, "source_revision", sourceName, "generator");
    result.label = readString(generator, "label", sourceName, "generator");
    result.description = readString(
        generator, "description", sourceName, "generator");
    result.data = parseData(generator, sourceName, "generator", false);
    validateGeneratorDefinition(result, sourceName);
    return result;
}

void validateGeneratorDefinition(const GeneratorDefinition& definition,
                                 std::string_view sourceName) {
    if (definition.schemaVersion !=
        kGeneratorDefinitionAuthoringSchemaVersion) {
        fail(sourceName, "generator.schema_version",
             "unsupported schema version " +
                 std::to_string(definition.schemaVersion));
    }
    if (!validNamespacedId(definition.id)) {
        fail(sourceName, "generator.id", "must be a non-empty namespaced ID");
    }
    if (definition.sourceRevision == 0) {
        fail(sourceName, "generator.source_revision",
             "must be greater than zero");
    }
    if (definition.label.empty()) {
        fail(sourceName, "generator.label", "must not be empty");
    }
    validateData(definition.data, sourceName);
}

void validateGeneratorDefinitionContent(const GeneratorDefinitionData& data,
                                        const BlockRegistry& registry,
                                        std::string_view sourceName) {
    auto requireMaterial = [&](const std::string& identifier,
                               const std::string& path) {
        if (!registry.findByIdentifier(identifier)) {
            fail(sourceName, path,
                 "references an unavailable block '" + identifier + "'");
        }
    };
    requireMaterial(data.terrain.solidMaterial,
                    "generator.terrain.solid_material");
    requireMaterial(data.terrain.waterMaterial,
                    "generator.terrain.water_material");
    for (size_t biome = 0; biome < data.biomes.entries.size(); ++biome) {
        for (size_t layer = 0;
             layer < data.biomes.entries[biome].surface.size(); ++layer) {
            requireMaterial(
                data.biomes.entries[biome].surface[layer].material,
                "generator.biomes.entries[" + std::to_string(biome) +
                    "].surface[" + std::to_string(layer) + "].material");
        }
    }
    for (size_t feature = 0; feature < data.structures.features.size();
         ++feature) {
        requireMaterial(
            data.structures.features[feature].material,
            "generator.structures.features[" + std::to_string(feature) +
                "].material");
    }
}

} // namespace Rigel::Voxel
