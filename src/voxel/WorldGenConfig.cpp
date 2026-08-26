#include "Rigel/Voxel/WorldGenConfig.h"

#include <ryml.hpp>
#include <ryml_std.hpp>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <unordered_set>

#include "Rigel/Util/Yaml.h"
#include "Rigel/Util/Ryml.h"
#include "Rigel/Voxel/WorldGenStages.h"

namespace Rigel::Voxel {

namespace {
void validateRetainedCount(size_t count,
                           size_t maximum,
                           const char* sourceName,
                           const std::string& path) {
    if (count >= maximum) {
        Util::throwConfigurationConstraint(
            sourceName,
            path,
            "must contain no more than " + std::to_string(maximum) + " entries"
        );
    }
}

bool isKnownGenerationStage(std::string_view name) {
    return std::any_of(
        kWorldGenPipelineStages.begin(),
        kWorldGenPipelineStages.end(),
        [name](const char* stage) { return name == stage; }
    );
}

void validateKeys(ryml::ConstNodeRef node,
                  const char* sourceName,
                  std::string_view path,
                  std::initializer_list<std::string_view> knownKeys,
                  bool strict) {
    if (!strict) {
        Util::warnUnknownKeys(node, sourceName, path, knownKeys);
        return;
    }
    if (!node.readable() || !node.is_map()) {
        throw std::invalid_argument(
            "Generator definition field '" +
            std::string(path.empty() ? "document" : path) + "' in '" +
            sourceName + "' must be a mapping");
    }
    std::unordered_set<std::string> encountered;
    for (const ryml::ConstNodeRef child : node.children()) {
        const std::string key = Util::toStdString(child.key());
        const std::string fullPath = path.empty()
            ? key
            : std::string(path) + "." + key;
        if (!encountered.insert(key).second) {
            throw std::invalid_argument(
                "Duplicate generator definition field '" + fullPath +
                "' in '" + sourceName + "'");
        }
        if (std::find(knownKeys.begin(), knownKeys.end(), key) !=
            knownKeys.end()) {
            continue;
        }
        throw std::invalid_argument(
            "Unknown generator definition field '" + fullPath +
            "' in '" + sourceName + "'");
    }
}

void validateDynamicMap(ryml::ConstNodeRef node,
                        const char* sourceName,
                        std::string_view path,
                        bool strict) {
    if (!strict) {
        return;
    }
    if (!node.readable() || !node.is_map()) {
        throw std::invalid_argument(
            "Generator definition field '" + std::string(path) + "' in '" +
            sourceName + "' must be a mapping");
    }
    std::unordered_set<std::string> encountered;
    for (const ryml::ConstNodeRef child : node.children()) {
        const std::string key = Util::toStdString(child.key());
        if (!encountered.insert(key).second) {
            throw std::invalid_argument(
                "Duplicate generator definition field '" +
                std::string(path) + "." + key + "' in '" + sourceName +
                "'");
        }
    }
}

void requireSequence(ryml::ConstNodeRef node,
                     const char* sourceName,
                     std::string_view path,
                     bool strict) {
    if (strict && (!node.readable() || !node.is_seq())) {
        throw std::invalid_argument(
            "Generator definition field '" + std::string(path) + "' in '" +
            sourceName + "' must be a sequence");
    }
}

std::string requireNonEmptyString(ryml::ConstNodeRef node,
                                  const char* key,
                                  const char* sourceName,
                                  std::string_view path) {
    const std::string value = Util::readString(node, key, "");
    if (value.empty()) {
        throw std::invalid_argument(
            "Generator definition field '" + std::string(path) + "." + key +
            "' in '" + sourceName + "' must be a non-empty string");
    }
    return value;
}

std::vector<WorldGenConfig::OverlayConfig> applyOverlayRouting(
    WorldGenConfig& config,
    ryml::ConstNodeRef root,
    const char* sourceName) {
    std::vector<WorldGenConfig::OverlayConfig> declaredOverlays;
    if (root.has_child("flags")) {
        const ryml::ConstNodeRef flagsNode = root["flags"];
        if (flagsNode.is_map()) {
            for (ryml::ConstNodeRef flagNode : flagsNode.children()) {
                const std::string key = Util::toStdString(flagNode.key());
                config.flags[key] = Util::readBool(
                    flagsNode, key.c_str(), false, sourceName, "flags");
            }
        }
    }

    if (root.has_child("overlays")) {
        const ryml::ConstNodeRef overlaysNode = root["overlays"];
        if (overlaysNode.is_seq()) {
            config.overlays.clear();
            for (ryml::ConstNodeRef overlayNode : overlaysNode.children()) {
                WorldGenConfig::OverlayConfig overlay;
                overlay.path = Util::readString(overlayNode, "path", "");
                overlay.when = Util::readString(overlayNode, "when", "");
                if (!overlay.path.empty()) {
                    declaredOverlays.push_back(overlay);
                    config.overlays.push_back(std::move(overlay));
                }
            }
        }
    }
    return declaredOverlays;
}

void validateGenerationStages(ryml::ConstNodeRef stages,
                              const char* sourceName,
                              bool strict) {
    if (strict) {
        validateDynamicMap(
            stages, sourceName, "generation.stages", true);
    } else if (!stages.readable() || !stages.is_map()) {
        return;
    }

    for (ryml::ConstNodeRef stage : stages.children()) {
        const std::string name = Util::toStdString(stage.key());
        if (isKnownGenerationStage(name)) {
            continue;
        }
        if (strict) {
            throw std::invalid_argument(
                "Unknown generator definition field 'generation.stages." +
                name + "' in '" + sourceName + "'");
        }
        spdlog::warn(
            "Unknown configuration key 'generation.stages.{}' in '{}'",
            name,
            sourceName
        );
    }
}

void validateNoiseKeys(ryml::ConstNodeRef node,
                       const char* sourceName,
                       std::string_view path,
                       bool strict) {
    validateKeys(
        node,
        sourceName,
        path,
        {"octaves", "frequency", "lacunarity", "persistence", "scale", "offset"},
        strict
    );
}

void validateClimateLayerKeys(ryml::ConstNodeRef node,
                              const char* sourceName,
                              std::string_view path,
                              bool strict) {
    validateKeys(
        node,
        sourceName,
        path,
        {"temperature", "humidity", "continentalness"},
        strict
    );
    for (const char* key : {"temperature", "humidity", "continentalness"}) {
        if (node.has_child(key)) {
            validateNoiseKeys(
                node[key], sourceName, std::string(path) + "." + key,
                strict);
        }
    }
}

void validateDensityNodeKeys(ryml::ConstNodeRef node,
                             const char* sourceName,
                             bool strict) {
    const std::string type = Util::readString(node, "type", "");
    if (!strict) {
        validateKeys(
            node,
            sourceName,
            "density_graph.nodes",
            {
                "id", "type", "inputs", "field", "noise", "value", "min",
                "max", "scale", "offset", "spline", "octaves", "frequency",
                "lacunarity", "persistence"
            },
            false);
    } else if (type == "constant") {
        validateKeys(
            node, sourceName, "density_graph.nodes",
            {"id", "type", "value"}, true);
    } else if (type == "noise2d" || type == "noise3d" ||
               type == "noise3d_xy") {
        validateKeys(
            node, sourceName, "density_graph.nodes",
            {"id", "type", "noise", "scale", "offset"}, true);
    } else if (type == "add" || type == "mul" || type == "max" ||
               type == "min" || type == "abs" || type == "invert") {
        validateKeys(
            node, sourceName, "density_graph.nodes",
            {"id", "type", "inputs"}, true);
    } else if (type == "clamp") {
        validateKeys(
            node, sourceName, "density_graph.nodes",
            {"id", "type", "inputs", "min", "max"}, true);
    } else if (type == "spline") {
        validateKeys(
            node, sourceName, "density_graph.nodes",
            {"id", "type", "inputs", "spline"}, true);
    } else if (type == "climate") {
        validateKeys(
            node, sourceName, "density_graph.nodes",
            {"id", "type", "field"}, true);
    } else if (type == "y") {
        validateKeys(
            node, sourceName, "density_graph.nodes",
            {"id", "type", "scale", "offset"}, true);
    } else {
        validateKeys(
            node,
            sourceName,
            "density_graph.nodes",
            {
                "id", "type", "inputs", "field", "noise", "value", "min",
                "max", "scale", "offset", "spline"
            },
            true);
    }

    if (strict) {
        static_cast<void>(requireNonEmptyString(
            node, "id", sourceName, "density_graph.nodes"));
        static_cast<void>(requireNonEmptyString(
            node, "type", sourceName, "density_graph.nodes"));
    }

    if (node.has_child("inputs")) {
        requireSequence(
            node["inputs"], sourceName, "density_graph.nodes.inputs",
            strict);
    }

    if (node.has_child("noise")) {
        validateNoiseKeys(
            node["noise"], sourceName, "density_graph.nodes.noise", strict);
    }
    if (node.has_child("spline")) {
        requireSequence(
            node["spline"], sourceName, "density_graph.nodes.spline",
            strict);
        for (const ryml::ConstNodeRef point : node["spline"].children()) {
            if (point.is_seq()) {
                if (strict && point.num_children() != 2) {
                    throw std::invalid_argument(
                        "Generator definition field "
                        "'density_graph.nodes.spline' in '" +
                        std::string(sourceName) +
                        "' requires two-value points");
                }
            } else {
                validateKeys(
                    point,
                    sourceName,
                    "density_graph.nodes.spline",
                    {"x", "y"},
                    strict);
                if (strict &&
                    (!point.has_child("x") || !point["x"].has_val() ||
                     point["x"].val().empty() || !point.has_child("y") ||
                     !point["y"].has_val() || point["y"].val().empty())) {
                    throw std::invalid_argument(
                        "Generator definition field "
                        "'density_graph.nodes.spline' in '" +
                        std::string(sourceName) +
                        "' requires mapping points with x and y values");
                }
            }
        }
    }
}

void validateWorldConfigKeys(ryml::ConstNodeRef root,
                             const char* sourceName,
                             bool strict) {
    validateKeys(
        root,
        sourceName,
        "",
        {
            "seed", "solid_block", "surface_block", "water_block", "shore_block",
            "world", "terrain", "climate", "biomes", "density_graph", "caves",
            "structures", "streaming", "generation", "flags", "overlays",
            "generator"
        },
        strict
    );

    if (root.has_child("flags")) {
        validateDynamicMap(root["flags"], sourceName, "flags", strict);
    }

    if (root.has_child("world")) {
        if (strict) {
            validateKeys(
                root["world"], sourceName, "world",
                {"min_y", "max_y", "sea_level"}, true);
        } else {
            validateKeys(
                root["world"], sourceName, "world",
                {"min_y", "max_y", "sea_level", "version"}, false);
        }
    }

    if (root.has_child("terrain")) {
        const ryml::ConstNodeRef terrain = root["terrain"];
        validateKeys(
            terrain,
            sourceName,
            "terrain",
            {
                "base_height", "height_variation", "surface_depth", "noise",
                "density_noise", "density_strength", "gradient_strength"
            },
            strict
        );
        if (terrain.has_child("noise")) {
            validateNoiseKeys(
                terrain["noise"], sourceName, "terrain.noise", strict);
        }
        if (terrain.has_child("density_noise")) {
            validateNoiseKeys(
                terrain["density_noise"], sourceName,
                "terrain.density_noise", strict);
        }
    }

    if (root.has_child("climate")) {
        const ryml::ConstNodeRef climate = root["climate"];
        validateKeys(
            climate,
            sourceName,
            "climate",
            {
                "global", "local", "local_blend", "latitude_scale",
                "latitude_strength"
            },
            strict
        );
        if (climate.has_child("global")) {
            validateClimateLayerKeys(
                climate["global"], sourceName, "climate.global", strict);
        }
        if (climate.has_child("local")) {
            validateClimateLayerKeys(
                climate["local"], sourceName, "climate.local", strict);
        }
    }

    if (root.has_child("biomes")) {
        const ryml::ConstNodeRef biomes = root["biomes"];
        validateKeys(
            biomes,
            sourceName,
            "biomes",
            {"blend_power", "epsilon", "entries", "coast_band"},
            strict
        );
        if (biomes.has_child("coast_band")) {
            validateKeys(
                biomes["coast_band"],
                sourceName,
                "biomes.coast_band",
                {"biome", "min_continentalness", "max_continentalness", "min", "max"},
                strict
            );
        }
        if (biomes.has_child("entries")) {
            requireSequence(
                biomes["entries"], sourceName, "biomes.entries", strict);
            for (ryml::ConstNodeRef entry : biomes["entries"].children()) {
                validateKeys(
                    entry,
                    sourceName,
                    "biomes.entries",
                    {"name", "target", "weight", "surface"},
                    strict
                );
                if (strict) {
                    static_cast<void>(requireNonEmptyString(
                        entry, "name", sourceName, "biomes.entries"));
                }
                if (entry.has_child("target")) {
                    validateKeys(
                        entry["target"],
                        sourceName,
                        "biomes.entries.target",
                        {"temperature", "humidity", "continentalness"},
                        strict
                    );
                }
                if (entry.has_child("surface")) {
                    requireSequence(
                        entry["surface"], sourceName,
                        "biomes.entries.surface", strict);
                    for (ryml::ConstNodeRef layer : entry["surface"].children()) {
                        validateKeys(
                            layer,
                            sourceName,
                            "biomes.entries.surface",
                            {"block", "depth"},
                            strict
                        );
                        if (strict) {
                            static_cast<void>(requireNonEmptyString(
                                layer, "block", sourceName,
                                "biomes.entries.surface"));
                        }
                    }
                }
            }
        }
    }

    if (root.has_child("density_graph")) {
        const ryml::ConstNodeRef graph = root["density_graph"];
        validateKeys(
            graph, sourceName, "density_graph", {"outputs", "nodes"},
            strict);
        if (graph.has_child("outputs")) {
            const ryml::ConstNodeRef outputs = graph["outputs"];
            validateDynamicMap(
                outputs, sourceName, "density_graph.outputs", strict);
            if (strict) {
                for (const ryml::ConstNodeRef output : outputs.children()) {
                    const std::string semantic =
                        Util::toStdString(output.key());
                    std::string node;
                    if (output.has_val()) {
                        output >> node;
                    }
                    if (semantic.empty() || node.empty()) {
                        throw std::invalid_argument(
                            "Generator definition outputs in '" +
                            std::string(sourceName) +
                            "' require non-empty semantics and node IDs");
                    }
                }
            }
        }
        if (graph.has_child("nodes")) {
            requireSequence(
                graph["nodes"], sourceName, "density_graph.nodes", strict);
            std::unordered_set<std::string> nodeIds;
            for (ryml::ConstNodeRef node : graph["nodes"].children()) {
                validateDensityNodeKeys(node, sourceName, strict);
                if (strict) {
                    const std::string id =
                        Util::readString(node, "id", "");
                    if (!nodeIds.insert(id).second) {
                        throw std::invalid_argument(
                            "Duplicate generator definition density node ID '" +
                            id + "' in '" + sourceName + "'");
                    }
                }
            }
        }
    }

    if (root.has_child("caves")) {
        validateKeys(
            root["caves"],
            sourceName,
            "caves",
            {"density_output", "threshold"},
            strict
        );
    }

    if (root.has_child("structures")) {
        const ryml::ConstNodeRef structures = root["structures"];
        validateKeys(
            structures, sourceName, "structures", {"features"}, strict);
        if (structures.has_child("features")) {
            requireSequence(
                structures["features"], sourceName, "structures.features",
                strict);
            for (ryml::ConstNodeRef feature : structures["features"].children()) {
                validateKeys(
                    feature,
                    sourceName,
                    "structures.features",
                    {"name", "block", "chance", "min_height", "max_height", "biomes"},
                    strict
                );
                if (strict) {
                    static_cast<void>(requireNonEmptyString(
                        feature, "name", sourceName,
                        "structures.features"));
                    static_cast<void>(requireNonEmptyString(
                        feature, "block", sourceName,
                        "structures.features"));
                }
                if (feature.has_child("biomes")) {
                    requireSequence(
                        feature["biomes"], sourceName,
                        "structures.features.biomes", strict);
                }
            }
        }
    }

    if (root.has_child("generation")) {
        const ryml::ConstNodeRef generation = root["generation"];
        validateKeys(
            generation, sourceName, "generation", {"stages"}, strict);
        if (generation.has_child("stages")) {
            validateGenerationStages(
                generation["stages"], sourceName, strict);
        }
    }

    if (root.has_child("overlays")) {
        requireSequence(
            root["overlays"], sourceName, "overlays", strict);
        for (ryml::ConstNodeRef overlay : root["overlays"].children()) {
            validateKeys(
                overlay, sourceName, "overlays", {"path", "when"}, strict);
            if (strict) {
                static_cast<void>(requireNonEmptyString(
                    overlay, "path", sourceName, "overlays"));
            }
        }
    }
}

void applyNoise(ryml::ConstNodeRef node,
                WorldGenConfig::NoiseConfig& noise,
                const char* sourceName,
                std::string_view path) {
    noise.octaves = Util::readIntWithMaximum(
        node, "octaves", noise.octaves, 0,
        WorldGenConfig::MaxNoiseOctaves, sourceName, path);
    noise.frequency = Util::readFloat(node, "frequency", noise.frequency);
    noise.lacunarity = Util::readFloat(node, "lacunarity", noise.lacunarity);
    noise.persistence = Util::readFloat(node, "persistence", noise.persistence);
    noise.scale = Util::readFloat(node, "scale", noise.scale);
    noise.offset = Util::readFloat(node, "offset", noise.offset);
}

void applyClimateLayer(ryml::ConstNodeRef node,
                       WorldGenConfig::ClimateLayerConfig& layer,
                       const char* sourceName,
                       std::string_view path) {
    if (!node.readable()) {
        return;
    }
    if (node.has_child("temperature")) {
        applyNoise(
            node["temperature"], layer.temperature, sourceName,
            std::string(path) + ".temperature");
    }
    if (node.has_child("humidity")) {
        applyNoise(
            node["humidity"], layer.humidity, sourceName,
            std::string(path) + ".humidity");
    }
    if (node.has_child("continentalness")) {
        applyNoise(
            node["continentalness"], layer.continentalness, sourceName,
            std::string(path) + ".continentalness");
    }
}

void validateWorldBounds(const WorldGenConfig::WorldConfig& world,
                         const char* sourceName) {
    if (world.maxY < world.minY) {
        Util::throwConfigurationConstraint(
            sourceName,
            "world.max_y",
            "must be greater than or equal to 'world.min_y'"
        );
    }
    const int64_t height =
        static_cast<int64_t>(world.maxY) - world.minY + 1;
    if (height > WorldGenConfig::MaxWorldHeight) {
        Util::throwConfigurationConstraint(
            sourceName,
            "world.max_y",
            "inclusive world height must not exceed " +
                std::to_string(WorldGenConfig::MaxWorldHeight)
        );
    }
}

WorldGenConfig::BiomeTarget readBiomeTarget(ryml::ConstNodeRef node,
                                            const WorldGenConfig::BiomeTarget& fallback) {
    WorldGenConfig::BiomeTarget target = fallback;
    if (!node.readable()) {
        return target;
    }
    target.temperature = Util::readFloat(node, "temperature", target.temperature);
    target.humidity = Util::readFloat(node, "humidity", target.humidity);
    target.continentalness = Util::readFloat(node, "continentalness", target.continentalness);
    return target;
}
} // namespace

void WorldGenConfig::applyYaml(const char* sourceName, const std::string& yaml) {
    applyYamlWithOverlays(sourceName, yaml);
}

std::vector<WorldGenConfig::OverlayConfig>
WorldGenConfig::applyCreationYamlWithOverlays(
    const char* sourceName,
    const std::string& yaml) {
    WorldGenConfig candidate = *this;
    auto overlays = candidate.applyYamlUnchecked(sourceName, yaml, true);
    *this = std::move(candidate);
    return overlays;
}

std::vector<WorldGenConfig::OverlayConfig> WorldGenConfig::applyYamlWithOverlays(
    const char* sourceName,
    const std::string& yaml) {
    WorldGenConfig candidate = *this;
    auto overlays = candidate.applyYamlUnchecked(sourceName, yaml, false);
    *this = std::move(candidate);
    return overlays;
}

std::vector<WorldGenConfig::OverlayConfig> WorldGenConfig::applyYamlRouting(
    const char* sourceName,
    const std::string& yaml) {
    if (yaml.empty()) {
        return {};
    }

    WorldGenConfig candidate = *this;
    const ryml::Tree tree = ryml::parse_in_arena(
        ryml::to_csubstr(sourceName),
        ryml::to_csubstr(yaml));
    const ryml::ConstNodeRef root = tree.rootref();
    if (root.has_child("overlays")) {
        for (const ryml::ConstNodeRef overlay :
             root["overlays"].children()) {
            Util::warnUnknownKeys(
                overlay, sourceName, "overlays", {"path", "when"});
        }
    }
    auto declaredOverlays = applyOverlayRouting(
        candidate, root, sourceName);
    *this = std::move(candidate);
    return declaredOverlays;
}

std::vector<WorldGenConfig::OverlayConfig> WorldGenConfig::applyYamlUnchecked(
    const char* sourceName,
    const std::string& yaml,
    bool strictDefinitionFields) {
    std::vector<OverlayConfig> declaredOverlays;
    if (yaml.empty()) {
        return declaredOverlays;
    }

    ryml::Tree tree = ryml::parse_in_arena(
        ryml::to_csubstr(sourceName),
        ryml::to_csubstr(yaml)
    );
    ryml::ConstNodeRef root = tree.rootref();
    validateWorldConfigKeys(root, sourceName, strictDefinitionFields);

    seed = static_cast<uint32_t>(Util::readInt(root, "seed", static_cast<int>(seed)));
    solidBlock = Util::readString(root, "solid_block", solidBlock);
    surfaceBlock = Util::readString(root, "surface_block", surfaceBlock);
    waterBlock = Util::readString(root, "water_block", waterBlock);
    shoreBlock = Util::readString(root, "shore_block", shoreBlock);

    if (root.has_child("world")) {
        ryml::ConstNodeRef worldNode = root["world"];
        world.minY = Util::readIntInRange(
            worldNode, "min_y", world.minY, MinWorldY, MaxWorldY,
            sourceName, "world");
        world.maxY = Util::readIntInRange(
            worldNode, "max_y", world.maxY, MinWorldY, MaxWorldY,
            sourceName, "world");
        world.seaLevel = Util::readIntInRange(
            worldNode, "sea_level", world.seaLevel, MinWorldY, MaxWorldY,
            sourceName, "world");
        world.version = static_cast<uint32_t>(Util::readInt(worldNode, "version",
                                                      static_cast<int>(world.version)));
    }

    if (root.has_child("terrain")) {
        ryml::ConstNodeRef terrainNode = root["terrain"];
        terrain.baseHeight = Util::readFloat(terrainNode, "base_height", terrain.baseHeight);
        terrain.heightVariation = Util::readFloat(terrainNode, "height_variation", terrain.heightVariation);
        terrain.surfaceDepth = Util::readIntWithMaximum(
            terrainNode, "surface_depth", terrain.surfaceDepth,
            std::numeric_limits<int>::min(),
            MaxSurfaceDepth, sourceName, "terrain");
        terrain.densityStrength = Util::readFloat(terrainNode, "density_strength", terrain.densityStrength);
        terrain.gradientStrength = Util::readFloat(terrainNode, "gradient_strength", terrain.gradientStrength);
        if (terrainNode.has_child("noise")) {
            applyNoise(
                terrainNode["noise"], terrain.heightNoise,
                sourceName, "terrain.noise");
        }
        if (terrainNode.has_child("density_noise")) {
            applyNoise(
                terrainNode["density_noise"], terrain.densityNoise,
                sourceName, "terrain.density_noise");
        }
    }

    if (root.has_child("climate")) {
        ryml::ConstNodeRef climateNode = root["climate"];
        climate.localBlend = Util::readFloat(climateNode, "local_blend", climate.localBlend);
        climate.latitudeScale = Util::readFloat(climateNode, "latitude_scale", climate.latitudeScale);
        climate.latitudeStrength = Util::readFloat(climateNode, "latitude_strength", climate.latitudeStrength);
        if (climateNode.has_child("global")) {
            applyClimateLayer(
                climateNode["global"], climate.global,
                sourceName, "climate.global");
        }
        if (climateNode.has_child("local")) {
            applyClimateLayer(
                climateNode["local"], climate.local,
                sourceName, "climate.local");
        }
    }

    if (root.has_child("biomes")) {
        ryml::ConstNodeRef biomesNode = root["biomes"];
        biomes.blend.blendPower = Util::readFloat(biomesNode, "blend_power", biomes.blend.blendPower);
        biomes.blend.epsilon = Util::readFloat(biomesNode, "epsilon", biomes.blend.epsilon);
        if (biomesNode.has_child("entries")) {
            ryml::ConstNodeRef entries = biomesNode["entries"];
            if (entries.is_seq()) {
                biomes.entries.clear();
                size_t biomeIndex = 0;
                for (ryml::ConstNodeRef entry : entries.children()) {
                    BiomeConfig biome;
                    biome.name = Util::readString(entry, "name", "");
                    if (!biome.name.empty() && entry.has_child("target")) {
                        biome.target = readBiomeTarget(entry["target"], biome.target);
                    }
                    if (!biome.name.empty()) {
                        biome.weight = Util::readFloat(entry, "weight", biome.weight);
                    }
                    if (!biome.name.empty() && entry.has_child("surface")) {
                        ryml::ConstNodeRef surface = entry["surface"];
                        if (surface.is_seq()) {
                            const std::string surfacePath =
                                "biomes.entries[" + std::to_string(biomeIndex) +
                                "].surface";
                            size_t layerIndex = 0;
                            int totalSurfaceDepth = 0;
                            for (ryml::ConstNodeRef layerNode : surface.children()) {
                                SurfaceLayer layer;
                                layer.block = Util::readString(layerNode, "block", "");
                                if (!layer.block.empty()) {
                                    layer.depth = Util::readIntWithMaximum(
                                        layerNode, "depth", layer.depth,
                                        std::numeric_limits<int>::min(),
                                        MaxSurfaceDepth, sourceName,
                                        "biomes.entries[" +
                                            std::to_string(biomeIndex) +
                                            "].surface[" +
                                            std::to_string(layerIndex) + "]");
                                    validateRetainedCount(
                                        biome.surface.size(), MaxSurfaceLayers,
                                        sourceName, surfacePath);
                                    if (layer.depth > 0) {
                                        if (layer.depth >
                                            MaxSurfaceDepth - totalSurfaceDepth) {
                                            Util::throwConfigurationConstraint(
                                                sourceName,
                                                surfacePath + "[" +
                                                    std::to_string(layerIndex) +
                                                    "].depth",
                                                "cumulative biome surface depth must "
                                                "not exceed " +
                                                    std::to_string(MaxSurfaceDepth)
                                            );
                                        }
                                        totalSurfaceDepth += layer.depth;
                                    }
                                    biome.surface.push_back(std::move(layer));
                                }
                                ++layerIndex;
                            }
                        }
                    }
                    if (!biome.name.empty()) {
                        validateRetainedCount(
                            biomes.entries.size(), MaxBiomeEntries, sourceName,
                            "biomes.entries");
                        biomes.entries.push_back(std::move(biome));
                    }
                    ++biomeIndex;
                }
            }
        }
        if (biomesNode.has_child("coast_band")) {
            ryml::ConstNodeRef bandNode = biomesNode["coast_band"];
            biomes.coastBand.biome = Util::readString(bandNode, "biome", biomes.coastBand.biome);
            biomes.coastBand.minContinentalness = Util::readFloat(
                bandNode, "min_continentalness", biomes.coastBand.minContinentalness);
            biomes.coastBand.maxContinentalness = Util::readFloat(
                bandNode, "max_continentalness", biomes.coastBand.maxContinentalness);
            if (bandNode.has_child("min")) {
                biomes.coastBand.minContinentalness = Util::readFloat(
                    bandNode, "min", biomes.coastBand.minContinentalness);
            }
            if (bandNode.has_child("max")) {
                biomes.coastBand.maxContinentalness = Util::readFloat(
                    bandNode, "max", biomes.coastBand.maxContinentalness);
            }
            biomes.coastBand.enabled = !biomes.coastBand.biome.empty();
            if (biomes.coastBand.minContinentalness > biomes.coastBand.maxContinentalness) {
                std::swap(biomes.coastBand.minContinentalness,
                          biomes.coastBand.maxContinentalness);
            }
        }
    }

    if (root.has_child("density_graph")) {
        ryml::ConstNodeRef graphNode = root["density_graph"];
        if (graphNode.has_child("outputs")) {
            ryml::ConstNodeRef outputs = graphNode["outputs"];
            if (outputs.is_map()) {
                for (ryml::ConstNodeRef output : outputs.children()) {
                    std::string key = Util::toStdString(output.key());
                    std::string value;
                    output >> value;
                    if (!key.empty() && !value.empty()) {
                        if (!densityGraph.outputs.contains(key)) {
                            validateRetainedCount(
                                densityGraph.outputs.size(),
                                MaxDensityGraphOutputs,
                                sourceName,
                                "density_graph.outputs." + key);
                        }
                        densityGraph.outputs[key] = value;
                    }
                }
            }
        }
        if (graphNode.has_child("nodes")) {
            ryml::ConstNodeRef nodes = graphNode["nodes"];
            if (nodes.is_seq()) {
                size_t nodeIndex = 0;
                for (ryml::ConstNodeRef node : nodes.children()) {
                    DensityNodeConfig config;
                    config.id = Util::readString(node, "id", "");
                    config.type = Util::readString(node, "type", "");
                    if (config.id.empty() || config.type.empty()) {
                        ++nodeIndex;
                        continue;
                    }
                    const std::string nodePath = "density_graph.nodes[" +
                        std::to_string(nodeIndex) + "]";
                    config.field = Util::readString(node, "field", "");
                    config.value = Util::readFloat(node, "value", config.value);
                    config.minValue = Util::readFloat(node, "min", config.minValue);
                    config.maxValue = Util::readFloat(node, "max", config.maxValue);
                    config.scale = Util::readFloat(node, "scale", config.scale);
                    config.offset = Util::readFloat(node, "offset", config.offset);
                    if (node.has_child("inputs")) {
                        ryml::ConstNodeRef inputs = node["inputs"];
                        if (inputs.is_seq()) {
                            for (ryml::ConstNodeRef input : inputs.children()) {
                                std::string name;
                                input >> name;
                                if (!name.empty()) {
                                    validateRetainedCount(
                                        config.inputs.size(),
                                        MaxDensityNodeInputs,
                                        sourceName,
                                        nodePath + ".inputs");
                                    config.inputs.push_back(std::move(name));
                                }
                            }
                        }
                    }
                    if (node.has_child("noise")) {
                        applyNoise(
                            node["noise"], config.noise,
                            sourceName, "density_graph.nodes.noise");
                    } else {
                        applyNoise(
                            node, config.noise,
                            sourceName, "density_graph.nodes");
                    }
                    if (node.has_child("spline")) {
                        ryml::ConstNodeRef spline = node["spline"];
                        if (spline.is_seq()) {
                            for (ryml::ConstNodeRef point : spline.children()) {
                                float x = 0.0f;
                                float y = 0.0f;
                                if (point.is_seq() && point.num_children() >= 2) {
                                    point[0] >> x;
                                    point[1] >> y;
                                } else {
                                    x = Util::readFloat(point, "x", x);
                                    y = Util::readFloat(point, "y", y);
                                }
                                validateRetainedCount(
                                    config.splinePoints.size(),
                                    MaxDensitySplinePoints,
                                    sourceName,
                                    nodePath + ".spline");
                                config.splinePoints.emplace_back(x, y);
                            }
                        }
                    }
                    auto it = std::find_if(
                        densityGraph.nodes.begin(),
                        densityGraph.nodes.end(),
                        [&](const DensityNodeConfig& existing) {
                            return existing.id == config.id;
                        });
                    if (it != densityGraph.nodes.end()) {
                        *it = std::move(config);
                    } else {
                        validateRetainedCount(
                            densityGraph.nodes.size(),
                            MaxDensityGraphNodes,
                            sourceName,
                            nodePath);
                        densityGraph.nodes.push_back(std::move(config));
                    }
                    ++nodeIndex;
                }
            }
        }
    }

    if (root.has_child("caves")) {
        ryml::ConstNodeRef cavesNode = root["caves"];
        caves.densityOutput = Util::readString(cavesNode, "density_output", caves.densityOutput);
        caves.threshold = Util::readFloat(cavesNode, "threshold", caves.threshold);
    }

    if (root.has_child("structures")) {
        ryml::ConstNodeRef structuresNode = root["structures"];
        if (structuresNode.has_child("features")) {
            ryml::ConstNodeRef features = structuresNode["features"];
            if (features.is_seq()) {
                structures.features.clear();
                size_t featureIndex = 0;
                for (ryml::ConstNodeRef featureNode : features.children()) {
                    FeatureConfig feature;
                    feature.name = Util::readString(featureNode, "name", "");
                    feature.block = Util::readString(featureNode, "block", "");
                    feature.chance = Util::readFloat(featureNode, "chance", feature.chance);
                    const std::string featurePath = "structures.features[" +
                        std::to_string(featureIndex) + "]";
                    if (!feature.block.empty()) {
                        feature.minHeight = Util::readIntWithMaximum(
                            featureNode, "min_height", feature.minHeight,
                            std::numeric_limits<int>::min(),
                            MaxStructureHeight, sourceName, featurePath);
                        feature.maxHeight = Util::readIntWithMaximum(
                            featureNode, "max_height", feature.maxHeight,
                            std::numeric_limits<int>::min(),
                            MaxStructureHeight, sourceName, featurePath);
                    }
                    if (!feature.block.empty() && featureNode.has_child("biomes")) {
                        ryml::ConstNodeRef biomesNode = featureNode["biomes"];
                        if (biomesNode.is_seq()) {
                            for (ryml::ConstNodeRef biomeNode : biomesNode.children()) {
                                std::string biomeName;
                                biomeNode >> biomeName;
                                if (!biomeName.empty()) {
                                    validateRetainedCount(
                                        feature.biomes.size(),
                                        MaxFeatureBiomeFilters, sourceName,
                                        featurePath + ".biomes");
                                    feature.biomes.push_back(std::move(biomeName));
                                }
                            }
                        }
                    }
                    if (!feature.block.empty()) {
                        validateRetainedCount(
                            structures.features.size(), MaxStructureFeatures,
                            sourceName, "structures.features");
                        structures.features.push_back(std::move(feature));
                    }
                    ++featureIndex;
                }
            }
        }
    }

    if (root.has_child("generation") && root["generation"].has_child("stages")) {
        const ryml::ConstNodeRef stages = root["generation"]["stages"];
        if (stages.is_map()) {
            for (ryml::ConstNodeRef stage : stages.children()) {
                const std::string name = Util::toStdString(stage.key());
                if (!isKnownGenerationStage(name)) {
                    continue;
                }
                stageEnabled[name] = Util::readBool(
                    stages,
                    name.c_str(),
                    true,
                    sourceName,
                    "generation.stages");
            }
        }
    }

    declaredOverlays = applyOverlayRouting(*this, root, sourceName);

    spdlog::debug("Applied world gen config from {}", sourceName);
    return declaredOverlays;
}

void WorldGenConfig::validate(const char* sourceName) const {
    validateWorldBounds(world, sourceName);

    auto validateMaximum = [sourceName](int value,
                                        int maximum,
                                        const std::string& path) {
        if (value > maximum) {
            Util::throwConfigurationConstraint(
                sourceName, path,
                "must be no greater than " + std::to_string(maximum)
            );
        }
    };
    auto validateNoise = [&validateMaximum](const NoiseConfig& noise,
                                            const std::string& path) {
        validateMaximum(noise.octaves, MaxNoiseOctaves, path + ".octaves");
    };

    validateMaximum(
        terrain.surfaceDepth, MaxSurfaceDepth, "terrain.surface_depth");
    validateNoise(terrain.heightNoise, "terrain.noise");
    validateNoise(terrain.densityNoise, "terrain.density_noise");
    validateNoise(climate.global.temperature, "climate.global.temperature");
    validateNoise(climate.global.humidity, "climate.global.humidity");
    validateNoise(
        climate.global.continentalness, "climate.global.continentalness");
    validateNoise(climate.local.temperature, "climate.local.temperature");
    validateNoise(climate.local.humidity, "climate.local.humidity");
    validateNoise(
        climate.local.continentalness, "climate.local.continentalness");

    if (biomes.entries.size() > MaxBiomeEntries) {
        Util::throwConfigurationConstraint(
            sourceName, "biomes.entries",
            "must contain no more than " +
                std::to_string(MaxBiomeEntries) + " entries");
    }
    for (size_t biomeIndex = 0; biomeIndex < biomes.entries.size();
         ++biomeIndex) {
        const auto& biome = biomes.entries[biomeIndex];
        const std::string surfacePath = "biomes.entries[" +
            std::to_string(biomeIndex) + "].surface";
        if (biome.surface.size() > MaxSurfaceLayers) {
            Util::throwConfigurationConstraint(
                sourceName, surfacePath,
                "must contain no more than " +
                    std::to_string(MaxSurfaceLayers) + " entries");
        }
        int totalDepth = 0;
        for (size_t layerIndex = 0; layerIndex < biome.surface.size();
             ++layerIndex) {
            const auto& layer = biome.surface[layerIndex];
            const std::string depthPath = surfacePath + "[" +
                std::to_string(layerIndex) + "].depth";
            if (!layer.block.empty()) {
                validateMaximum(layer.depth, MaxSurfaceDepth, depthPath);
            }
            if (!layer.block.empty() && layer.depth > 0) {
                if (layer.depth > MaxSurfaceDepth - totalDepth) {
                    Util::throwConfigurationConstraint(
                        sourceName, depthPath,
                        "cumulative biome surface depth must not exceed " +
                            std::to_string(MaxSurfaceDepth));
                }
                totalDepth += layer.depth;
            }
        }
    }

    if (densityGraph.outputs.size() > MaxDensityGraphOutputs) {
        Util::throwConfigurationConstraint(
            sourceName, "density_graph.outputs",
            "must contain no more than " +
                std::to_string(MaxDensityGraphOutputs) + " entries");
    }
    if (densityGraph.nodes.size() > MaxDensityGraphNodes) {
        Util::throwConfigurationConstraint(
            sourceName, "density_graph.nodes",
            "must contain no more than " +
                std::to_string(MaxDensityGraphNodes) + " entries");
    }
    for (size_t nodeIndex = 0; nodeIndex < densityGraph.nodes.size();
         ++nodeIndex) {
        const auto& node = densityGraph.nodes[nodeIndex];
        const std::string nodePath = "density_graph.nodes[" +
            std::to_string(nodeIndex) + "]";
        if (node.inputs.size() > MaxDensityNodeInputs) {
            Util::throwConfigurationConstraint(
                sourceName, nodePath + ".inputs",
                "must contain no more than " +
                    std::to_string(MaxDensityNodeInputs) + " entries");
        }
        if (node.splinePoints.size() > MaxDensitySplinePoints) {
            Util::throwConfigurationConstraint(
                sourceName, nodePath + ".spline",
                "must contain no more than " +
                    std::to_string(MaxDensitySplinePoints) + " entries");
        }
        validateNoise(
            node.noise, nodePath + ".noise");
    }

    if (structures.features.size() > MaxStructureFeatures) {
        Util::throwConfigurationConstraint(
            sourceName, "structures.features",
            "must contain no more than " +
                std::to_string(MaxStructureFeatures) + " entries");
    }
    for (size_t featureIndex = 0;
         featureIndex < structures.features.size(); ++featureIndex) {
        const auto& feature = structures.features[featureIndex];
        const std::string featurePath = "structures.features[" +
            std::to_string(featureIndex) + "]";
        validateMaximum(
            feature.minHeight, MaxStructureHeight,
            featurePath + ".min_height");
        validateMaximum(
            feature.maxHeight, MaxStructureHeight,
            featurePath + ".max_height");
        if (feature.biomes.size() > MaxFeatureBiomeFilters) {
            Util::throwConfigurationConstraint(
                sourceName, featurePath + ".biomes",
                "must contain no more than " +
                    std::to_string(MaxFeatureBiomeFilters) + " entries");
        }
    }
}

bool WorldGenConfig::isStageEnabled(const std::string& stage) const {
    if (!isKnownGenerationStage(stage)) {
        return false;
    }
    auto it = stageEnabled.find(stage);
    if (it == stageEnabled.end()) {
        return true;
    }
    return it->second;
}

bool WorldGenConfig::isFlagEnabled(const std::string& name) const {
    auto it = flags.find(name);
    if (it == flags.end()) {
        return false;
    }
    return it->second;
}

} // namespace Rigel::Voxel
