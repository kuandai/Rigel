#include "Rigel/Voxel/WorldConfigProvider.h"

#include "Rigel/Asset/Types.h"
#include "ResourceRegistry.h"

#include <glm/vec3.hpp>
#include <ryml.hpp>
#include <ryml_std.hpp>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_set>
#include <spdlog/spdlog.h>

namespace Rigel::Voxel {

namespace {

std::optional<std::string> readFile(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::string stripAssetsPrefix(std::string_view path) {
    constexpr std::string_view kAssetsPrefix = "assets/";
    if (path.substr(0, kAssetsPrefix.size()) == kAssetsPrefix) {
        return std::string(path.substr(kAssetsPrefix.size()));
    }
    return std::string(path);
}

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

bool readVec3(ryml::ConstNodeRef node, const char* key, glm::vec3& value) {
    if (!node.readable() || !node.has_child(key)) {
        return false;
    }
    ryml::ConstNodeRef vecNode = node[key];
    if (vecNode.is_seq() && vecNode.num_children() >= 3) {
        vecNode[0] >> value.x;
        vecNode[1] >> value.y;
        vecNode[2] >> value.z;
        return true;
    }
    return false;
}

void applyShadowConfig(ryml::ConstNodeRef shadowNode, ShadowConfig& shadow) {
    if (!shadowNode.readable()) {
        return;
    }
    shadow.enabled = readBool(shadowNode, "enabled", shadow.enabled);
    shadow.cascades = readInt(shadowNode, "cascades", shadow.cascades);
    shadow.mapSize = readInt(shadowNode, "map_size", shadow.mapSize);
    shadow.maxDistance = readFloat(shadowNode, "max_distance", shadow.maxDistance);
    shadow.splitLambda = readFloat(shadowNode, "split_lambda", shadow.splitLambda);
    shadow.bias = readFloat(shadowNode, "bias", shadow.bias);
    shadow.normalBias = readFloat(shadowNode, "normal_bias", shadow.normalBias);
    shadow.pcfRadius = readInt(shadowNode, "pcf_radius", shadow.pcfRadius);
    shadow.pcfRadiusNear = readInt(shadowNode, "pcf_radius_near", shadow.pcfRadius);
    shadow.pcfRadiusFar = readInt(shadowNode, "pcf_radius_far", shadow.pcfRadius);
    shadow.transparentScale = readFloat(shadowNode, "transparent_scale", shadow.transparentScale);
    shadow.strength = readFloat(shadowNode, "strength", shadow.strength);
    shadow.fadePower = readFloat(shadowNode, "fade_power", shadow.fadePower);

    if (shadow.cascades < 1) {
        shadow.cascades = 1;
    } else if (shadow.cascades > ShadowConfig::MaxCascades) {
        shadow.cascades = ShadowConfig::MaxCascades;
    }
    if (shadow.mapSize < 1) {
        shadow.mapSize = 1;
    }
    if (shadow.pcfRadius < 0) {
        shadow.pcfRadius = 0;
    }
    if (shadow.pcfRadiusNear < 0) {
        shadow.pcfRadiusNear = 0;
    }
    if (shadow.pcfRadiusFar < 0) {
        shadow.pcfRadiusFar = 0;
    }
    if (shadow.transparentScale < 0.0f) {
        shadow.transparentScale = 0.0f;
    }
    if (shadow.strength < 0.0f) {
        shadow.strength = 0.0f;
    }
    if (shadow.fadePower < 0.0f) {
        shadow.fadePower = 0.0f;
    }
}

void applyRenderYaml(const char* sourceName,
                     const std::string& yaml,
                     WorldRenderConfig& config) {
    if (yaml.empty()) {
        return;
    }

    ryml::Tree tree = ryml::parse_in_arena(
        ryml::to_csubstr(sourceName),
        ryml::to_csubstr(yaml)
    );
    ryml::ConstNodeRef root = tree.rootref();
    ryml::ConstNodeRef renderNode = root;
    if (root.has_child("render")) {
        renderNode = root["render"];
    }
    if (!renderNode.readable()) {
        return;
    }

    readVec3(renderNode, "sun_direction", config.sunDirection);
    config.transparentAlpha = readFloat(renderNode, "transparent_alpha", config.transparentAlpha);
    config.renderDistance = readFloat(renderNode, "render_distance", config.renderDistance);

    if (renderNode.has_child("shadow")) {
        applyShadowConfig(renderNode["shadow"], config.shadow);
    }
}

} // namespace

std::optional<ConfigSourceResult> IConfigSource::loadPath(std::string_view) const {
    return std::nullopt;
}

EmbeddedConfigSource::EmbeddedConfigSource(Asset::AssetManager& assets, std::string assetId)
    : m_assets(assets)
    , m_assetId(std::move(assetId))
{}

std::optional<std::string> EmbeddedConfigSource::load() const {
    if (!m_assets.exists(m_assetId)) {
        return std::nullopt;
    }
    auto asset = m_assets.get<Asset::RawAsset>(m_assetId);
    return std::string(asset->data.begin(), asset->data.end());
}

std::string EmbeddedConfigSource::name() const {
    return m_assetId;
}

std::optional<ConfigSourceResult> EmbeddedConfigSource::loadPath(std::string_view path) const {
    std::string normalized = stripAssetsPrefix(path);
    try {
        auto data = ResourceRegistry::Get(normalized);
        return ConfigSourceResult{std::move(normalized), std::string(data.begin(), data.end())};
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

FileConfigSource::FileConfigSource(std::string path)
    : m_path(std::move(path))
{}

std::optional<std::string> FileConfigSource::load() const {
    return readFile(m_path);
}

std::string FileConfigSource::name() const {
    return m_path;
}

std::optional<ConfigSourceResult> FileConfigSource::loadPath(std::string_view path) const {
    std::filesystem::path candidate(path);
    auto content = readFile(candidate);
    if (!content && !candidate.is_absolute()) {
        std::filesystem::path baseDir = std::filesystem::path(m_path).parent_path();
        if (!baseDir.empty()) {
            candidate = baseDir / candidate;
            content = readFile(candidate);
        }
    }
    if (!content) {
        return std::nullopt;
    }
    return ConfigSourceResult{candidate.string(), std::move(*content)};
}

void ConfigProvider::addSource(std::unique_ptr<IConfigSource> source) {
    m_sources.push_back(std::move(source));
}

WorldGenConfig ConfigProvider::loadConfig() const {
    WorldGenConfig config;

    for (const auto& source : m_sources) {
        auto yaml = source->load();
        if (!yaml) {
            continue;
        }
        config.applyYaml(source->name().c_str(), *yaml);
    }

    std::unordered_set<std::string> appliedOverlays;
    size_t overlayIndex = 0;
    while (overlayIndex < config.overlays.size()) {
        const auto& overlay = config.overlays[overlayIndex];
        ++overlayIndex;
        if (!overlay.when.empty() && !config.isFlagEnabled(overlay.when)) {
            continue;
        }
        if (appliedOverlays.find(overlay.path) != appliedOverlays.end()) {
            continue;
        }
        std::optional<ConfigSourceResult> overlayData;
        for (const auto& source : m_sources) {
            overlayData = source->loadPath(overlay.path);
            if (overlayData) {
                break;
            }
        }
        if (!overlayData) {
            continue;
        }
        config.applyYaml(overlayData->name.c_str(), overlayData->content);
        appliedOverlays.insert(overlay.path);
    }

    return config;
}

WorldRenderConfig ConfigProvider::loadRenderConfig() const {
    WorldRenderConfig config;
    for (const auto& source : m_sources) {
        auto yaml = source->load();
        if (!yaml) {
            continue;
        }
        applyRenderYaml(source->name().c_str(), *yaml, config);
    }
    return config;
}

} // namespace Rigel::Voxel
