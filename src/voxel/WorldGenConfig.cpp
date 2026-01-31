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
    if (yaml.empty()) {
        return;
    }

    ryml::Tree tree = ryml::parse_in_arena(
        ryml::to_csubstr(sourceName),
        ryml::to_csubstr(yaml)
    );
    ryml::ConstNodeRef root = tree.rootref();

    seed = static_cast<uint32_t>(Util::readInt(root, "seed", static_cast<int>(seed)));
    solidBlock = Util::readString(root, "solid_block", solidBlock);
    surfaceBlock = Util::readString(root, "surface_block", surfaceBlock);

    if (root.has_child("world")) {
        ryml::ConstNodeRef worldNode = root["world"];
        world.minY = Util::readInt(worldNode, "min_y", world.minY);
        world.maxY = Util::readInt(worldNode, "max_y", world.maxY);
        world.seaLevel = Util::readInt(worldNode, "sea_level", world.seaLevel);
        world.lavaLevel = Util::readInt(worldNode, "lava_level", world.lavaLevel);
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
        climate.elevationLapse = Util::readFloat(climateNode, "elevation_lapse", climate.elevationLapse);
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
        caves.enabled = Util::readBool(cavesNode, "enabled", caves.enabled);
        caves.densityOutput = Util::readString(cavesNode, "density_output", caves.densityOutput);
        caves.threshold = Util::readFloat(cavesNode, "threshold", caves.threshold);
        caves.sampleStep = Util::readInt(cavesNode, "sample_step", caves.sampleStep);
    }

    if (root.has_child("structures")) {
        ryml::ConstNodeRef structuresNode = root["structures"];
        if (structuresNode.has_child("features")) {
            ryml::ConstNodeRef features = structuresNode["features"];
            if (features.is_seq()) {
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

        stream.loadQueueLimit = Util::readInt(streamNode, "load_queue_limit", stream.loadQueueLimit);
        if (stream.loadQueueLimit < 0) {
            stream.loadQueueLimit = 0;
        }

        int resident = Util::readInt(streamNode, "max_resident_chunks", static_cast<int>(stream.maxResidentChunks));
        if (resident < 0) {
            resident = 0;
        }
        stream.maxResidentChunks = static_cast<size_t>(resident);
    }

    if (root.has_child("generation") && root["generation"].has_child("pipeline")) {
        ryml::ConstNodeRef pipeline = root["generation"]["pipeline"];
        bool orderMatches = (pipeline.num_children() == kWorldGenPipelineStages.size());
        size_t index = 0;
        for (ryml::ConstNodeRef stage : pipeline.children()) {
            if (!stage.has_child("stage")) {
                ++index;
                continue;
            }
            std::string stageName;
            stage["stage"] >> stageName;
            bool enabled = Util::readBool(stage, "enabled", true);
            stageEnabled[stageName] = enabled;
            if (orderMatches) {
                if (index >= kWorldGenPipelineStages.size() ||
                    stageName != kWorldGenPipelineStages[index]) {
                    orderMatches = false;
                }
            }
            ++index;
        }
        if (!orderMatches) {
            spdlog::warn("Pipeline list order does not match fixed stage order; order will be ignored");
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
            for (ryml::ConstNodeRef overlayNode : overlaysNode.children()) {
                OverlayConfig overlay;
                overlay.path = Util::readString(overlayNode, "path", "");
                overlay.when = Util::readString(overlayNode, "when", "");
                if (!overlay.path.empty()) {
                    overlays.push_back(std::move(overlay));
                }
            }
        }
    }

    spdlog::debug("Applied world gen config from {}", sourceName);
}

bool WorldGenConfig::isStageEnabled(const std::string& stage) const {
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
