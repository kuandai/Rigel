#include "Rigel/Voxel/WorldGenConfig.h"

#include <ryml.hpp>
#include <ryml_std.hpp>
#include <spdlog/spdlog.h>
#include <algorithm>

#include "Rigel/Util/Yaml.h"
#include "Rigel/Util/Ryml.h"
#include "Rigel/Voxel/WorldGenStages.h"

namespace Rigel::Voxel {

namespace {
bool isKnownGenerationStage(std::string_view name) {
    return std::any_of(
        kWorldGenPipelineStages.begin(),
        kWorldGenPipelineStages.end(),
        [name](const char* stage) { return name == stage; }
    );
}

void warnUnknownGenerationStages(ryml::ConstNodeRef stages,
                                 const char* sourceName) {
    if (!stages.readable() || !stages.is_map()) {
        return;
    }

    for (ryml::ConstNodeRef stage : stages.children()) {
        const std::string name = Util::toStdString(stage.key());
        if (isKnownGenerationStage(name)) {
            continue;
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
                       std::string_view path) {
    Util::warnUnknownKeys(
        node,
        sourceName,
        path,
        {"octaves", "frequency", "lacunarity", "persistence", "scale", "offset"}
    );
}

void validateClimateLayerKeys(ryml::ConstNodeRef node,
                              const char* sourceName,
                              std::string_view path) {
    Util::warnUnknownKeys(
        node,
        sourceName,
        path,
        {"temperature", "humidity", "continentalness"}
    );
    for (const char* key : {"temperature", "humidity", "continentalness"}) {
        if (node.has_child(key)) {
            validateNoiseKeys(node[key], sourceName, std::string(path) + "." + key);
        }
    }
}

void validateWorldConfigKeys(ryml::ConstNodeRef root, const char* sourceName) {
    Util::warnUnknownKeys(
        root,
        sourceName,
        "",
        {
            "seed", "solid_block", "surface_block", "world", "terrain", "climate",
            "biomes", "density_graph", "caves", "structures", "streaming",
            "generation", "flags", "overlays"
        }
    );

    if (root.has_child("world")) {
        Util::warnUnknownKeys(
            root["world"],
            sourceName,
            "world",
            {"min_y", "max_y", "sea_level", "version"}
        );
    }

    if (root.has_child("terrain")) {
        const ryml::ConstNodeRef terrain = root["terrain"];
        Util::warnUnknownKeys(
            terrain,
            sourceName,
            "terrain",
            {
                "base_height", "height_variation", "surface_depth", "noise",
                "density_noise", "density_strength", "gradient_strength"
            }
        );
        if (terrain.has_child("noise")) {
            validateNoiseKeys(terrain["noise"], sourceName, "terrain.noise");
        }
        if (terrain.has_child("density_noise")) {
            validateNoiseKeys(
                terrain["density_noise"], sourceName, "terrain.density_noise");
        }
    }

    if (root.has_child("climate")) {
        const ryml::ConstNodeRef climate = root["climate"];
        Util::warnUnknownKeys(
            climate,
            sourceName,
            "climate",
            {
                "global", "local", "local_blend", "latitude_scale",
                "latitude_strength"
            }
        );
        if (climate.has_child("global")) {
            validateClimateLayerKeys(climate["global"], sourceName, "climate.global");
        }
        if (climate.has_child("local")) {
            validateClimateLayerKeys(climate["local"], sourceName, "climate.local");
        }
    }

    if (root.has_child("biomes")) {
        const ryml::ConstNodeRef biomes = root["biomes"];
        Util::warnUnknownKeys(
            biomes,
            sourceName,
            "biomes",
            {"blend_power", "epsilon", "entries", "coast_band"}
        );
        if (biomes.has_child("coast_band")) {
            Util::warnUnknownKeys(
                biomes["coast_band"],
                sourceName,
                "biomes.coast_band",
                {"biome", "min_continentalness", "max_continentalness", "min", "max"}
            );
        }
        if (biomes.has_child("entries")) {
            for (ryml::ConstNodeRef entry : biomes["entries"].children()) {
                Util::warnUnknownKeys(
                    entry,
                    sourceName,
                    "biomes.entries",
                    {"name", "target", "weight", "surface"}
                );
                if (entry.has_child("target")) {
                    Util::warnUnknownKeys(
                        entry["target"],
                        sourceName,
                        "biomes.entries.target",
                        {"temperature", "humidity", "continentalness"}
                    );
                }
                if (entry.has_child("surface")) {
                    for (ryml::ConstNodeRef layer : entry["surface"].children()) {
                        Util::warnUnknownKeys(
                            layer,
                            sourceName,
                            "biomes.entries.surface",
                            {"block", "depth"}
                        );
                    }
                }
            }
        }
    }

    if (root.has_child("density_graph")) {
        const ryml::ConstNodeRef graph = root["density_graph"];
        Util::warnUnknownKeys(
            graph, sourceName, "density_graph", {"outputs", "nodes"});
        if (graph.has_child("nodes")) {
            for (ryml::ConstNodeRef node : graph["nodes"].children()) {
                Util::warnUnknownKeys(
                    node,
                    sourceName,
                    "density_graph.nodes",
                    {
                        "id", "type", "inputs", "field", "noise", "value", "min",
                        "max", "scale", "offset", "spline", "octaves", "frequency",
                        "lacunarity", "persistence"
                    }
                );
                if (node.has_child("noise")) {
                    validateNoiseKeys(
                        node["noise"], sourceName, "density_graph.nodes.noise");
                }
                if (node.has_child("spline")) {
                    for (ryml::ConstNodeRef point : node["spline"].children()) {
                        Util::warnUnknownKeys(
                            point,
                            sourceName,
                            "density_graph.nodes.spline",
                            {"x", "y"}
                        );
                    }
                }
            }
        }
    }

    if (root.has_child("caves")) {
        Util::warnUnknownKeys(
            root["caves"],
            sourceName,
            "caves",
            {"density_output", "threshold"}
        );
    }

    if (root.has_child("structures")) {
        const ryml::ConstNodeRef structures = root["structures"];
        Util::warnUnknownKeys(
            structures, sourceName, "structures", {"features"});
        if (structures.has_child("features")) {
            for (ryml::ConstNodeRef feature : structures["features"].children()) {
                Util::warnUnknownKeys(
                    feature,
                    sourceName,
                    "structures.features",
                    {"name", "block", "chance", "min_height", "max_height", "biomes"}
                );
            }
        }
    }

    if (root.has_child("streaming")) {
        Util::warnUnknownKeys(
            root["streaming"],
            sourceName,
            "streaming",
            {
                "view_distance_chunks", "unload_distance_chunks", "gen_queue_limit",
                "mesh_queue_limit", "update_budget_per_frame", "apply_budget_per_frame",
                "worker_threads", "io_threads", "load_worker_threads",
                "load_apply_budget_per_frame", "load_region_drain_budget",
                "load_queue_limit", "load_max_cached_regions",
                "load_max_inflight_regions", "load_prefetch_radius",
                "load_prefetch_per_request", "max_resident_chunks"
            }
        );
    }

    if (root.has_child("generation")) {
        const ryml::ConstNodeRef generation = root["generation"];
        Util::warnUnknownKeys(
            generation, sourceName, "generation", {"stages"});
        if (generation.has_child("stages")) {
            warnUnknownGenerationStages(generation["stages"], sourceName);
        }
    }

    if (root.has_child("overlays")) {
        for (ryml::ConstNodeRef overlay : root["overlays"].children()) {
            Util::warnUnknownKeys(
                overlay, sourceName, "overlays", {"path", "when"});
        }
    }
}

void applyNoise(ryml::ConstNodeRef node, WorldGenConfig::NoiseConfig& noise) {
    noise.octaves = Util::readInt(node, "octaves", noise.octaves);
    noise.frequency = Util::readFloat(node, "frequency", noise.frequency);
    noise.lacunarity = Util::readFloat(node, "lacunarity", noise.lacunarity);
    noise.persistence = Util::readFloat(node, "persistence", noise.persistence);
    noise.scale = Util::readFloat(node, "scale", noise.scale);
    noise.offset = Util::readFloat(node, "offset", noise.offset);
}

void applyClimateLayer(ryml::ConstNodeRef node, WorldGenConfig::ClimateLayerConfig& layer) {
    if (!node.readable()) {
        return;
    }
    if (node.has_child("temperature")) {
        applyNoise(node["temperature"], layer.temperature);
    }
    if (node.has_child("humidity")) {
        applyNoise(node["humidity"], layer.humidity);
    }
    if (node.has_child("continentalness")) {
        applyNoise(node["continentalness"], layer.continentalness);
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

std::vector<WorldGenConfig::OverlayConfig> WorldGenConfig::applyYamlWithOverlays(
    const char* sourceName,
    const std::string& yaml) {
    std::vector<OverlayConfig> declaredOverlays;
    if (yaml.empty()) {
        return declaredOverlays;
    }

    ryml::Tree tree = ryml::parse_in_arena(
        ryml::to_csubstr(sourceName),
        ryml::to_csubstr(yaml)
    );
    ryml::ConstNodeRef root = tree.rootref();
    validateWorldConfigKeys(root, sourceName);

    seed = static_cast<uint32_t>(Util::readInt(root, "seed", static_cast<int>(seed)));
    solidBlock = Util::readString(root, "solid_block", solidBlock);
    surfaceBlock = Util::readString(root, "surface_block", surfaceBlock);

    if (root.has_child("world")) {
        ryml::ConstNodeRef worldNode = root["world"];
        world.minY = Util::readInt(worldNode, "min_y", world.minY);
        world.maxY = Util::readInt(worldNode, "max_y", world.maxY);
        world.seaLevel = Util::readInt(worldNode, "sea_level", world.seaLevel);
        world.version = static_cast<uint32_t>(Util::readInt(worldNode, "version",
                                                      static_cast<int>(world.version)));
    }

    if (root.has_child("terrain")) {
        ryml::ConstNodeRef terrainNode = root["terrain"];
        terrain.baseHeight = Util::readFloat(terrainNode, "base_height", terrain.baseHeight);
        terrain.heightVariation = Util::readFloat(terrainNode, "height_variation", terrain.heightVariation);
        terrain.surfaceDepth = Util::readInt(terrainNode, "surface_depth", terrain.surfaceDepth);
        terrain.densityStrength = Util::readFloat(terrainNode, "density_strength", terrain.densityStrength);
        terrain.gradientStrength = Util::readFloat(terrainNode, "gradient_strength", terrain.gradientStrength);
        if (terrainNode.has_child("noise")) {
            applyNoise(terrainNode["noise"], terrain.heightNoise);
        }
        if (terrainNode.has_child("density_noise")) {
            applyNoise(terrainNode["density_noise"], terrain.densityNoise);
        }
    }

    if (root.has_child("climate")) {
        ryml::ConstNodeRef climateNode = root["climate"];
        climate.localBlend = Util::readFloat(climateNode, "local_blend", climate.localBlend);
        climate.latitudeScale = Util::readFloat(climateNode, "latitude_scale", climate.latitudeScale);
        climate.latitudeStrength = Util::readFloat(climateNode, "latitude_strength", climate.latitudeStrength);
        if (climateNode.has_child("global")) {
            applyClimateLayer(climateNode["global"], climate.global);
        }
        if (climateNode.has_child("local")) {
            applyClimateLayer(climateNode["local"], climate.local);
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
                for (ryml::ConstNodeRef entry : entries.children()) {
                    BiomeConfig biome;
                    biome.name = Util::readString(entry, "name", "");
                    if (entry.has_child("target")) {
                        biome.target = readBiomeTarget(entry["target"], biome.target);
                    }
                    biome.weight = Util::readFloat(entry, "weight", biome.weight);
                    if (entry.has_child("surface")) {
                        ryml::ConstNodeRef surface = entry["surface"];
                        if (surface.is_seq()) {
                            for (ryml::ConstNodeRef layerNode : surface.children()) {
                                SurfaceLayer layer;
                                layer.block = Util::readString(layerNode, "block", "");
                                layer.depth = Util::readInt(layerNode, "depth", layer.depth);
                                if (!layer.block.empty()) {
                                    biome.surface.push_back(std::move(layer));
                                }
                            }
                        }
                    }
                    if (!biome.name.empty()) {
                        biomes.entries.push_back(std::move(biome));
                    }
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
                        densityGraph.outputs[key] = value;
                    }
                }
            }
        }
        if (graphNode.has_child("nodes")) {
            ryml::ConstNodeRef nodes = graphNode["nodes"];
            if (nodes.is_seq()) {
                for (ryml::ConstNodeRef node : nodes.children()) {
                    DensityNodeConfig config;
                    config.id = Util::readString(node, "id", "");
                    config.type = Util::readString(node, "type", "");
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
                                    config.inputs.push_back(std::move(name));
                                }
                            }
                        }
                    }
                    if (node.has_child("noise")) {
                        applyNoise(node["noise"], config.noise);
                    } else {
                        applyNoise(node, config.noise);
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
                                config.splinePoints.emplace_back(x, y);
                            }
                        }
                    }
                    if (!config.id.empty() && !config.type.empty()) {
                        auto it = std::find_if(
                            densityGraph.nodes.begin(),
                            densityGraph.nodes.end(),
                            [&](const DensityNodeConfig& existing) { return existing.id == config.id; }
                        );
                        if (it != densityGraph.nodes.end()) {
                            *it = std::move(config);
                        } else {
                            densityGraph.nodes.push_back(std::move(config));
                        }
                    }
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
                for (ryml::ConstNodeRef featureNode : features.children()) {
                    FeatureConfig feature;
                    feature.name = Util::readString(featureNode, "name", "");
                    feature.block = Util::readString(featureNode, "block", "");
                    feature.chance = Util::readFloat(featureNode, "chance", feature.chance);
                    feature.minHeight = Util::readInt(featureNode, "min_height", feature.minHeight);
                    feature.maxHeight = Util::readInt(featureNode, "max_height", feature.maxHeight);
                    if (featureNode.has_child("biomes")) {
                        ryml::ConstNodeRef biomesNode = featureNode["biomes"];
                        if (biomesNode.is_seq()) {
                            for (ryml::ConstNodeRef biomeNode : biomesNode.children()) {
                                std::string biomeName;
                                biomeNode >> biomeName;
                                if (!biomeName.empty()) {
                                    feature.biomes.push_back(std::move(biomeName));
                                }
                            }
                        }
                    }
                    if (!feature.block.empty()) {
                        structures.features.push_back(std::move(feature));
                    }
                }
            }
        }
    }

    if (root.has_child("streaming")) {
        ryml::ConstNodeRef streamNode = root["streaming"];
        stream.viewDistanceChunks = Util::readInt(streamNode, "view_distance_chunks", stream.viewDistanceChunks);
        stream.unloadDistanceChunks = Util::readInt(streamNode, "unload_distance_chunks", stream.unloadDistanceChunks);
        int genLimit = Util::readInt(streamNode, "gen_queue_limit", static_cast<int>(stream.genQueueLimit));
        if (genLimit < 0) {
            genLimit = 0;
        }
        stream.genQueueLimit = static_cast<size_t>(genLimit);

        int meshLimit = Util::readInt(streamNode, "mesh_queue_limit", static_cast<int>(stream.meshQueueLimit));
        if (meshLimit < 0) {
            meshLimit = 0;
        }
        stream.meshQueueLimit = static_cast<size_t>(meshLimit);

        stream.updateBudgetPerFrame =
            Util::readInt(streamNode, "update_budget_per_frame", stream.updateBudgetPerFrame);
        if (stream.updateBudgetPerFrame < 0) {
            stream.updateBudgetPerFrame = 0;
        }

        stream.applyBudgetPerFrame = Util::readInt(streamNode, "apply_budget_per_frame", stream.applyBudgetPerFrame);
        if (stream.applyBudgetPerFrame < 0) {
            stream.applyBudgetPerFrame = 0;
        }

        stream.workerThreads = Util::readInt(streamNode, "worker_threads", stream.workerThreads);
        if (stream.workerThreads < 0) {
            stream.workerThreads = 0;
        }

        stream.ioThreads = Util::readInt(streamNode, "io_threads", stream.ioThreads);
        if (stream.ioThreads < 0) {
            stream.ioThreads = 0;
        }

        stream.loadWorkerThreads = Util::readInt(streamNode, "load_worker_threads", stream.loadWorkerThreads);
        if (stream.loadWorkerThreads < 0) {
            stream.loadWorkerThreads = 0;
        }

        stream.loadApplyBudgetPerFrame =
            Util::readInt(streamNode, "load_apply_budget_per_frame", stream.loadApplyBudgetPerFrame);
        if (stream.loadApplyBudgetPerFrame < 0) {
            stream.loadApplyBudgetPerFrame = 0;
        }

        stream.loadRegionDrainBudget =
            Util::readInt(streamNode, "load_region_drain_budget", stream.loadRegionDrainBudget);
        if (stream.loadRegionDrainBudget < 0) {
            stream.loadRegionDrainBudget = 0;
        }

        stream.loadQueueLimit = Util::readInt(streamNode, "load_queue_limit", stream.loadQueueLimit);
        if (stream.loadQueueLimit < 0) {
            stream.loadQueueLimit = 0;
        }

        stream.loadMaxCachedRegions =
            Util::readInt(streamNode, "load_max_cached_regions", stream.loadMaxCachedRegions);
        if (stream.loadMaxCachedRegions < 0) {
            stream.loadMaxCachedRegions = 0;
        }

        stream.loadMaxInFlightRegions =
            Util::readInt(streamNode, "load_max_inflight_regions", stream.loadMaxInFlightRegions);
        if (stream.loadMaxInFlightRegions < 0) {
            stream.loadMaxInFlightRegions = 0;
        }

        stream.loadPrefetchRadius =
            Util::readInt(streamNode, "load_prefetch_radius", stream.loadPrefetchRadius);
        if (stream.loadPrefetchRadius < 0) {
            stream.loadPrefetchRadius = 0;
        }

        stream.loadPrefetchPerRequest =
            Util::readInt(streamNode, "load_prefetch_per_request", stream.loadPrefetchPerRequest);
        if (stream.loadPrefetchPerRequest < 0) {
            stream.loadPrefetchPerRequest = 0;
        }

        int resident = Util::readInt(streamNode, "max_resident_chunks", static_cast<int>(stream.maxResidentChunks));
        if (resident < 0) {
            resident = 0;
        }
        stream.maxResidentChunks = static_cast<size_t>(resident);
    }

    if (root.has_child("generation") && root["generation"].has_child("stages")) {
        const ryml::ConstNodeRef stages = root["generation"]["stages"];
        if (stages.is_map()) {
            for (ryml::ConstNodeRef stage : stages.children()) {
                const std::string name = Util::toStdString(stage.key());
                if (!isKnownGenerationStage(name)) {
                    continue;
                }
                stageEnabled[name] = Util::readBool(stages, name.c_str(), true);
            }
        }
    }

    if (root.has_child("flags")) {
        ryml::ConstNodeRef flagsNode = root["flags"];
        if (flagsNode.is_map()) {
            for (ryml::ConstNodeRef flagNode : flagsNode.children()) {
                std::string key = Util::toStdString(flagNode.key());
                bool value = Util::readBool(flagsNode, key.c_str(), false);
                flags[key] = value;
            }
        }
    }

    if (root.has_child("overlays")) {
        ryml::ConstNodeRef overlaysNode = root["overlays"];
        if (overlaysNode.is_seq()) {
            overlays.clear();
            for (ryml::ConstNodeRef overlayNode : overlaysNode.children()) {
                OverlayConfig overlay;
                overlay.path = Util::readString(overlayNode, "path", "");
                overlay.when = Util::readString(overlayNode, "when", "");
                if (!overlay.path.empty()) {
                    declaredOverlays.push_back(overlay);
                    overlays.push_back(std::move(overlay));
                }
            }
        }
    }

    spdlog::debug("Applied world gen config from {}", sourceName);
    return declaredOverlays;
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
