#include "Rigel/Voxel/GeneratorSnapshot.h"

#include "Rigel/Voxel/DensityFunction.h"
#include "Rigel/Voxel/WorldGenStages.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace Rigel::Voxel {
namespace {

constexpr std::array<std::string_view, 14> kDensityNodeTypes = {
    "constant", "noise2d", "noise3d", "noise3d_xy", "add", "mul",
    "clamp", "max", "min", "abs", "invert", "spline", "climate", "y"
};

bool isKnownDensityNodeType(std::string_view type) {
    return std::find(kDensityNodeTypes.begin(), kDensityNodeTypes.end(), type) !=
        kDensityNodeTypes.end();
}

bool isKnownClimateField(std::string_view field) {
    return field == "temperature" || field == "humidity" ||
        field == "continentalness";
}

std::string quoted(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('"');
    for (const unsigned char byte : value) {
        switch (byte) {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (byte < 0x20 || byte == 0x7f) {
                throw std::invalid_argument(
                    "Generator definition strings cannot contain control characters");
            }
            out.push_back(static_cast<char>(byte));
            break;
        }
    }
    out.push_back('"');
    return out;
}

std::string number(float value) {
    if (!std::isfinite(value)) {
        throw std::invalid_argument(
            "Generator definition numbers must be finite");
    }
    if (value == 0.0f) {
        return "0";
    }

    std::array<char, 64> buffer{};
    const auto result = std::to_chars(
        buffer.data(),
        buffer.data() + buffer.size(),
        value,
        std::chars_format::general,
        std::numeric_limits<float>::max_digits10);
    if (result.ec != std::errc{}) {
        throw std::runtime_error("Failed to serialize generator number");
    }
    return std::string(buffer.data(), result.ptr);
}

void appendNoise(std::string& out,
                 const WorldGenConfig::NoiseConfig& noise,
                 std::string_view indent) {
    out += indent;
    out += "octaves: " + std::to_string(noise.octaves) + "\n";
    out += indent;
    out += "frequency: " + number(noise.frequency) + "\n";
    out += indent;
    out += "lacunarity: " + number(noise.lacunarity) + "\n";
    out += indent;
    out += "persistence: " + number(noise.persistence) + "\n";
    out += indent;
    out += "scale: " + number(noise.scale) + "\n";
    out += indent;
    out += "offset: " + number(noise.offset) + "\n";
}

void appendClimateLayer(std::string& out,
                        const WorldGenConfig::ClimateLayerConfig& layer,
                        std::string_view indent) {
    out += indent;
    out += "temperature:\n";
    appendNoise(out, layer.temperature, std::string(indent) + "  ");
    out += indent;
    out += "humidity:\n";
    appendNoise(out, layer.humidity, std::string(indent) + "  ");
    out += indent;
    out += "continentalness:\n";
    appendNoise(out, layer.continentalness, std::string(indent) + "  ");
}

void requireUniqueNames(const WorldGenConfig& definition) {
    std::unordered_set<std::string> nodeNames;
    for (const auto& node : definition.densityGraph.nodes) {
        if (node.id.empty() || !nodeNames.insert(node.id).second) {
            throw std::invalid_argument(
                "Generator definition density node IDs must be non-empty and unique");
        }
    }

    std::unordered_set<std::string> biomeNames;
    for (const auto& biome : definition.biomes.entries) {
        if (biome.name.empty() || !biomeNames.insert(biome.name).second) {
            throw std::invalid_argument(
                "Generator definition biome names must be non-empty and unique");
        }
    }

    if (definition.biomes.coastBand.enabled &&
        !biomeNames.contains(definition.biomes.coastBand.biome)) {
        throw std::invalid_argument(
            "Generator definition coast biome is not defined: " +
            definition.biomes.coastBand.biome);
    }
    if (definition.isStageEnabled("structures")) {
        for (const auto& feature : definition.structures.features) {
            for (const auto& biome : feature.biomes) {
                if (!biomeNames.contains(biome)) {
                    throw std::invalid_argument(
                        "Generator definition structure biome is not defined: " +
                        biome);
                }
            }
        }
    }
}

void validateNodeContracts(const WorldGenConfig& definition) {
    for (const auto& node : definition.densityGraph.nodes) {
        if (!isKnownDensityNodeType(node.type)) {
            throw std::invalid_argument(
                "Generator definition has unknown density node type '" +
                node.type + "' for node '" + node.id + "'");
        }
        if (node.type == "climate" && !isKnownClimateField(node.field)) {
            throw std::invalid_argument(
                "Generator definition has unknown climate field '" +
                node.field + "' for node '" + node.id + "'");
        }
    }
}

void validateGraph(const WorldGenConfig& definition) {
    if (definition.densityGraph.nodes.empty()) {
        throw std::invalid_argument(
            "Generator definition requires a density graph");
    }
    const auto baseOutput = definition.densityGraph.outputs.find("base_density");
    if (baseOutput == definition.densityGraph.outputs.end()) {
        throw std::invalid_argument(
            "Generator definition requires the 'base_density' output");
    }
    if (definition.isStageEnabled("caves") &&
        !definition.densityGraph.outputs.contains(
            definition.caves.densityOutput)) {
        throw std::invalid_argument(
            "Generator definition cave output is not defined: " +
            definition.caves.densityOutput);
    }

    DensityGraph graph;
    std::string error;
    if (!buildDensityGraph(definition, graph, error)) {
        throw std::invalid_argument(
            "Generator definition has an invalid density graph: " + error);
    }
}

void validateDefinition(const WorldGenConfig& definition) {
    definition.validate("generator definition snapshot");
    requireUniqueNames(definition);
    validateNodeContracts(definition);
    validateGraph(definition);
}

void appendNodeInputs(std::string& out,
                      const std::vector<std::string>& inputs) {
    if (inputs.empty()) {
        return;
    }
    out += "    inputs:\n";
    for (const auto& input : inputs) {
        out += "      - " + quoted(input) + "\n";
    }
}

void appendDensityNode(std::string& out,
                       const WorldGenConfig::DensityNodeConfig& node) {
    out += "  - id: " + quoted(node.id) + "\n";
    out += "    type: " + quoted(node.type) + "\n";

    const bool hasInputs = node.type == "add" || node.type == "mul" ||
        node.type == "clamp" || node.type == "max" || node.type == "min" ||
        node.type == "abs" || node.type == "invert" || node.type == "spline";
    if (hasInputs) {
        appendNodeInputs(out, node.inputs);
    }

    if (node.type == "constant") {
        out += "    value: " + number(node.value) + "\n";
    } else if (node.type == "noise2d" || node.type == "noise3d" ||
               node.type == "noise3d_xy") {
        out += "    noise:\n";
        appendNoise(out, node.noise, "      ");
        out += "    scale: " + number(node.scale) + "\n";
        out += "    offset: " + number(node.offset) + "\n";
    } else if (node.type == "clamp") {
        out += "    min: " + number(node.minValue) + "\n";
        out += "    max: " + number(node.maxValue) + "\n";
    } else if (node.type == "spline") {
        std::vector<std::pair<float, float>> points = node.splinePoints;
        std::stable_sort(points.begin(), points.end(), [](const auto& left,
                                                         const auto& right) {
            return left.first < right.first;
        });
        out += "    spline:\n";
        for (const auto& [x, y] : points) {
            out += "      - [" + number(x) + ", " + number(y) + "]\n";
        }
    } else if (node.type == "climate") {
        out += "    field: " + quoted(node.field) + "\n";
    } else if (node.type == "y") {
        out += "    scale: " + number(node.scale) + "\n";
        out += "    offset: " + number(node.offset) + "\n";
    }
}

} // namespace

std::string serializeGeneratorSnapshot(const WorldGenConfig& definition) {
    validateDefinition(definition);

    std::string out;
    out.reserve(8192);
    out += "solid_block: " + quoted(definition.solidBlock) + "\n";
    out += "surface_block: " + quoted(definition.surfaceBlock) + "\n";
    out += "world:\n";
    out += "  min_y: " + std::to_string(definition.world.minY) + "\n";
    out += "  max_y: " + std::to_string(definition.world.maxY) + "\n";
    out += "  sea_level: " + std::to_string(definition.world.seaLevel) + "\n";
    out += "terrain:\n";
    out += "  surface_depth: " +
        std::to_string(definition.terrain.surfaceDepth) + "\n";

    out += "climate:\n";
    out += "  latitude_scale: " + number(definition.climate.latitudeScale) + "\n";
    out += "  latitude_strength: " +
        number(definition.climate.latitudeStrength) + "\n";
    out += "  local_blend: " + number(definition.climate.localBlend) + "\n";
    out += "  global:\n";
    appendClimateLayer(out, definition.climate.global, "    ");
    out += "  local:\n";
    appendClimateLayer(out, definition.climate.local, "    ");

    out += "biomes:\n";
    out += "  blend_power: " + number(definition.biomes.blend.blendPower) + "\n";
    out += "  epsilon: " + number(definition.biomes.blend.epsilon) + "\n";
    if (definition.biomes.coastBand.enabled) {
        out += "  coast_band:\n";
        out += "    biome: " + quoted(definition.biomes.coastBand.biome) + "\n";
        const float coastMin = std::min(
            definition.biomes.coastBand.minContinentalness,
            definition.biomes.coastBand.maxContinentalness);
        const float coastMax = std::max(
            definition.biomes.coastBand.minContinentalness,
            definition.biomes.coastBand.maxContinentalness);
        out += "    min_continentalness: " + number(coastMin) + "\n";
        out += "    max_continentalness: " + number(coastMax) + "\n";
    }
    out += "  entries:\n";
    for (const auto& biome : definition.biomes.entries) {
        out += "    - name: " + quoted(biome.name) + "\n";
        out += "      target:\n";
        out += "        temperature: " + number(biome.target.temperature) + "\n";
        out += "        humidity: " + number(biome.target.humidity) + "\n";
        out += "        continentalness: " +
            number(biome.target.continentalness) + "\n";
        out += "      weight: " + number(biome.weight) + "\n";
        out += "      surface:\n";
        for (const auto& layer : biome.surface) {
            out += "        - block: " + quoted(layer.block) + "\n";
            out += "          depth: " + std::to_string(layer.depth) + "\n";
        }
    }

    out += "density_graph:\n";
    out += "  outputs:\n";
    std::vector<std::pair<std::string, std::string>> outputs(
        definition.densityGraph.outputs.begin(),
        definition.densityGraph.outputs.end());
    std::sort(outputs.begin(), outputs.end());
    for (const auto& [semantic, node] : outputs) {
        out += "    " + quoted(semantic) + ": " + quoted(node) + "\n";
    }
    out += "  nodes:\n";
    for (const auto& node : definition.densityGraph.nodes) {
        appendDensityNode(out, node);
    }

    if (definition.isStageEnabled("caves")) {
        out += "caves:\n";
        out += "  density_output: " + quoted(definition.caves.densityOutput) + "\n";
        out += "  threshold: " + number(definition.caves.threshold) + "\n";
    }
    if (definition.isStageEnabled("structures")) {
        out += "structures:\n";
        out += "  features:\n";
        for (const auto& feature : definition.structures.features) {
            out += "    - name: " + quoted(feature.name) + "\n";
            out += "      block: " + quoted(feature.block) + "\n";
            out += "      chance: " + number(feature.chance) + "\n";
            out += "      min_height: " + std::to_string(feature.minHeight) + "\n";
            out += "      max_height: " +
                std::to_string(std::max(feature.minHeight, feature.maxHeight)) +
                "\n";
            out += "      biomes:\n";
            for (const auto& biome : feature.biomes) {
                out += "        - " + quoted(biome) + "\n";
            }
        }
    }

    out += "generation:\n";
    out += "  stages:\n";
    for (const char* stage : kWorldGenPipelineStages) {
        out += "    " + std::string(stage) + ": ";
        out += definition.isStageEnabled(stage) ? "true\n" : "false\n";
    }
    return out;
}

WorldGenConfig parseGeneratorSnapshot(std::string_view snapshot,
                                      uint32_t definitionSchemaVersion,
                                      uint32_t seed,
                                      uint32_t sourceRevision) {
    if (definitionSchemaVersion != kGeneratorDefinitionSchemaVersion) {
        throw std::invalid_argument(
            "Unsupported generator definition schema version: " +
            std::to_string(definitionSchemaVersion));
    }
    if (snapshot.empty()) {
        throw std::invalid_argument("Generator definition snapshot is empty");
    }

    WorldGenConfig definition;
    definition.seed = seed;
    definition.world.version = sourceRevision;
    definition.applyYaml(
        "saved generator definition",
        std::string(snapshot));
    validateDefinition(definition);

    const std::string canonical = serializeGeneratorSnapshot(definition);
    if (canonical != snapshot) {
        throw std::invalid_argument(
            "Saved generator definition is not in canonical form");
    }
    return definition;
}

} // namespace Rigel::Voxel
