#include "Rigel/Voxel/WorldConfigProvider.h"

#include "Rigel/Asset/Types.h"
#include "ResourceRegistry.h"
#include "Rigel/Util/Yaml.h"

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
    shadow.enabled = Util::readBool(shadowNode, "enabled", shadow.enabled);
    shadow.cascades = Util::readInt(shadowNode, "cascades", shadow.cascades);
    shadow.mapSize = Util::readInt(shadowNode, "map_size", shadow.mapSize);
    shadow.maxDistance = Util::readFloat(shadowNode, "max_distance", shadow.maxDistance);
    shadow.splitLambda = Util::readFloat(shadowNode, "split_lambda", shadow.splitLambda);
    shadow.bias = Util::readFloat(shadowNode, "bias", shadow.bias);
    shadow.normalBias = Util::readFloat(shadowNode, "normal_bias", shadow.normalBias);
    shadow.pcfRadius = Util::readInt(shadowNode, "pcf_radius", shadow.pcfRadius);
    shadow.pcfRadiusNear = Util::readInt(shadowNode, "pcf_radius_near", shadow.pcfRadius);
    shadow.pcfRadiusFar = Util::readInt(shadowNode, "pcf_radius_far", shadow.pcfRadius);
    shadow.transparentScale = Util::readFloat(shadowNode, "transparent_scale", shadow.transparentScale);
    shadow.strength = Util::readFloat(shadowNode, "strength", shadow.strength);
    shadow.fadePower = Util::readFloat(shadowNode, "fade_power", shadow.fadePower);

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

void applyTaaConfig(ryml::ConstNodeRef taaNode, TaaConfig& taa) {
    if (!taaNode.readable()) {
        return;
    }

    taa.enabled = Util::readBool(taaNode, "enabled", taa.enabled);
    taa.blend = Util::readFloat(taaNode, "blend", taa.blend);
    taa.jitterScale = Util::readFloat(taaNode, "jitter_scale", taa.jitterScale);

    if (taa.blend < 0.0f) {
        taa.blend = 0.0f;
    } else if (taa.blend > 1.0f) {
        taa.blend = 1.0f;
    }
    if (taa.jitterScale < 0.0f) {
        taa.jitterScale = 0.0f;
    }
}

void applySvoConfig(ryml::ConstNodeRef svoNode, SvoLodConfig& svo) {
    if (!svoNode.readable()) {
        return;
    }

    svo.enabled = Util::readBool(svoNode, "enabled", svo.enabled);
    svo.nearMeshRadiusChunks = Util::readInt(
        svoNode, "near_mesh_radius_chunks", svo.nearMeshRadiusChunks);
    svo.lodStartRadiusChunks = Util::readInt(
        svoNode, "lod_start_radius_chunks", svo.lodStartRadiusChunks);
    svo.lodCellSpanChunks = Util::readInt(
        svoNode, "lod_cell_span_chunks", svo.lodCellSpanChunks);
    svo.lodMaxCells = Util::readInt(
        svoNode, "lod_max_cells", svo.lodMaxCells);
    svo.lodMaxCpuBytes = static_cast<int64_t>(Util::readInt(
        svoNode, "lod_max_cpu_bytes", static_cast<int>(svo.lodMaxCpuBytes)));
    svo.lodMaxGpuBytes = static_cast<int64_t>(Util::readInt(
        svoNode, "lod_max_gpu_bytes", static_cast<int>(svo.lodMaxGpuBytes)));
    svo.lodCopyBudgetPerFrame = Util::readInt(
        svoNode, "lod_copy_budget_per_frame", svo.lodCopyBudgetPerFrame);
    svo.lodApplyBudgetPerFrame = Util::readInt(
        svoNode, "lod_apply_budget_per_frame", svo.lodApplyBudgetPerFrame);

    if (svo.nearMeshRadiusChunks < 0) {
        svo.nearMeshRadiusChunks = 0;
    }
    if (svo.lodStartRadiusChunks < svo.nearMeshRadiusChunks) {
        svo.lodStartRadiusChunks = svo.nearMeshRadiusChunks;
    }
    if (svo.lodCellSpanChunks < 1) {
        svo.lodCellSpanChunks = 1;
    }
    if (svo.lodMaxCells < 0) {
        svo.lodMaxCells = 0;
    }
    if (svo.lodMaxCpuBytes < 0) {
        svo.lodMaxCpuBytes = 0;
    }
    if (svo.lodMaxGpuBytes < 0) {
        svo.lodMaxGpuBytes = 0;
    }
    if (svo.lodCopyBudgetPerFrame < 0) {
        svo.lodCopyBudgetPerFrame = 0;
    }
    if (svo.lodApplyBudgetPerFrame < 0) {
        svo.lodApplyBudgetPerFrame = 0;
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
    config.transparentAlpha = Util::readFloat(renderNode, "transparent_alpha", config.transparentAlpha);
    config.renderDistance = Util::readFloat(renderNode, "render_distance", config.renderDistance);

    if (renderNode.has_child("shadow")) {
        applyShadowConfig(renderNode["shadow"], config.shadow);
    }
    if (renderNode.has_child("taa")) {
        applyTaaConfig(renderNode["taa"], config.taa);
    }
    if (renderNode.has_child("svo")) {
        applySvoConfig(renderNode["svo"], config.svo);
    }
    if (renderNode.has_child("profiling")) {
        const auto profilingNode = renderNode["profiling"];
        if (profilingNode.readable()) {
            config.profilingEnabled = Util::readBool(profilingNode, "enabled", config.profilingEnabled);
        }
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

Persistence::PersistenceConfig ConfigProvider::loadPersistenceConfig() const {
    Persistence::PersistenceConfig config;
    for (const auto& source : m_sources) {
        auto yaml = source->load();
        if (!yaml) {
            continue;
        }
        config.applyYaml(source->name().c_str(), *yaml);
    }
    return config;
}

} // namespace Rigel::Voxel
