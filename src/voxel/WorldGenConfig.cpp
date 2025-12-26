#include "Rigel/Voxel/WorldGenConfig.h"

#include <ryml.hpp>
#include <ryml_std.hpp>
#include <spdlog/spdlog.h>

namespace Rigel::Voxel {

namespace {
constexpr const char* kPipelineStages[] = {
    "climate_global",
    "climate_local",
    "biome_resolve",
    "terrain_density",
    "caves",
    "surface_rules",
    "structures",
    "post_process"
};

bool readBool(ryml::ConstNodeRef node, const char* key, bool fallback) {
    if (!node.readable() || !node.has_child(key)) {
        return fallback;
    }
    std::string value;
    node[key] >> value;
    return value == "true" || value == "yes" || value == "1";
}

int readInt(ryml::ConstNodeRef node, const char* key, int fallback) {
    if (!node.readable() || !node.has_child(key)) {
        return fallback;
    }
    int value = fallback;
    node[key] >> value;
    return value;
}

float readFloat(ryml::ConstNodeRef node, const char* key, float fallback) {
    if (!node.readable() || !node.has_child(key)) {
        return fallback;
    }
    float value = fallback;
    node[key] >> value;
    return value;
}

std::string readString(ryml::ConstNodeRef node, const char* key, const std::string& fallback) {
    if (!node.readable() || !node.has_child(key)) {
        return fallback;
    }
    std::string value;
    node[key] >> value;
    return value;
}

void applyNoise(ryml::ConstNodeRef node, WorldGenConfig::NoiseConfig& noise) {
    noise.octaves = readInt(node, "octaves", noise.octaves);
    noise.frequency = readFloat(node, "frequency", noise.frequency);
    noise.lacunarity = readFloat(node, "lacunarity", noise.lacunarity);
    noise.persistence = readFloat(node, "persistence", noise.persistence);
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

    seed = static_cast<uint32_t>(readInt(root, "seed", static_cast<int>(seed)));
    solidBlock = readString(root, "solid_block", solidBlock);
    surfaceBlock = readString(root, "surface_block", surfaceBlock);

    if (root.has_child("terrain")) {
        ryml::ConstNodeRef terrainNode = root["terrain"];
        terrain.baseHeight = readFloat(terrainNode, "base_height", terrain.baseHeight);
        terrain.heightVariation = readFloat(terrainNode, "height_variation", terrain.heightVariation);
        terrain.surfaceDepth = readInt(terrainNode, "surface_depth", terrain.surfaceDepth);
        if (terrainNode.has_child("noise")) {
            applyNoise(terrainNode["noise"], terrain.heightNoise);
        }
    }

    if (root.has_child("streaming")) {
        ryml::ConstNodeRef streamNode = root["streaming"];
        stream.viewDistanceChunks = readInt(streamNode, "view_distance_chunks", stream.viewDistanceChunks);
        stream.unloadDistanceChunks = readInt(streamNode, "unload_distance_chunks", stream.unloadDistanceChunks);
        stream.maxGeneratePerFrame = readInt(streamNode, "max_generate_per_frame", stream.maxGeneratePerFrame);
        stream.maxResidentChunks = static_cast<size_t>(
            readInt(streamNode, "max_resident_chunks", static_cast<int>(stream.maxResidentChunks))
        );
    }

    if (root.has_child("generation") && root["generation"].has_child("pipeline")) {
        ryml::ConstNodeRef pipeline = root["generation"]["pipeline"];
        bool orderMatches = (pipeline.num_children() == sizeof(kPipelineStages) / sizeof(kPipelineStages[0]));
        size_t index = 0;
        for (ryml::ConstNodeRef stage : pipeline.children()) {
            if (!stage.has_child("stage")) {
                ++index;
                continue;
            }
            std::string stageName;
            stage["stage"] >> stageName;
            bool enabled = readBool(stage, "enabled", true);
            stageEnabled[stageName] = enabled;
            if (orderMatches) {
                if (index >= sizeof(kPipelineStages) / sizeof(kPipelineStages[0]) ||
                    stageName != kPipelineStages[index]) {
                    orderMatches = false;
                }
            }
            ++index;
        }
        if (!orderMatches) {
            spdlog::warn("Pipeline list order does not match fixed stage order; order will be ignored");
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

} // namespace Rigel::Voxel
