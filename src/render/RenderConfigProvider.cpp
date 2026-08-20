#include "Rigel/Render/RenderConfigProvider.h"

#include "Rigel/Util/Yaml.h"

#include <glm/vec3.hpp>
#include <ryml.hpp>
#include <ryml_std.hpp>

#include <algorithm>

namespace Rigel::Render {
namespace {

bool readVec3(ryml::ConstNodeRef node, const char* key, glm::vec3& value) {
    if (!node.readable() || !node.has_child(key)) {
        return false;
    }
    const ryml::ConstNodeRef vecNode = node[key];
    if (vecNode.is_seq() && vecNode.num_children() >= 3) {
        vecNode[0] >> value.x;
        vecNode[1] >> value.y;
        vecNode[2] >> value.z;
        return true;
    }
    return false;
}

struct PcfRadiusState {
    bool hasNearOverride = false;
    bool hasFarOverride = false;
};

void validateRenderConfigKeys(ryml::ConstNodeRef root,
                              ryml::ConstNodeRef renderNode,
                              const char* sourceName) {
    if (root.has_child("render")) {
        Util::warnUnknownKeys(root, sourceName, "", {"render"});
    }
    Util::warnUnknownKeys(
        renderNode,
        sourceName,
        "render",
        {"sun_direction", "transparent_alpha", "render_distance", "shadow", "taa", "profiling"}
    );
    if (renderNode.has_child("shadow")) {
        Util::warnUnknownKeys(
            renderNode["shadow"],
            sourceName,
            "render.shadow",
            {
                "enabled", "cascades", "map_size", "max_distance", "split_lambda",
                "bias", "normal_bias", "pcf_radius", "pcf_radius_near",
                "pcf_radius_far", "transparent_scale", "strength", "fade_power"
            }
        );
    }
    if (renderNode.has_child("taa")) {
        Util::warnUnknownKeys(
            renderNode["taa"],
            sourceName,
            "render.taa",
            {"enabled", "blend", "jitter_scale"}
        );
    }
    if (renderNode.has_child("profiling")) {
        Util::warnUnknownKeys(
            renderNode["profiling"],
            sourceName,
            "render.profiling",
            {"enabled"}
        );
    }
}

void applyShadowConfig(ryml::ConstNodeRef shadowNode,
                       Voxel::ShadowConfig& shadow,
                       PcfRadiusState& pcfState) {
    if (!shadowNode.readable()) {
        return;
    }
    shadow.enabled = Util::readBool(shadowNode, "enabled", shadow.enabled);
    shadow.cascades = Util::readInt(shadowNode, "cascades", shadow.cascades);
    shadow.mapSize = Util::readInt(shadowNode, "map_size", shadow.mapSize);
    shadow.maxDistance = Util::readFloat(
        shadowNode, "max_distance", shadow.maxDistance);
    shadow.splitLambda = Util::readFloat(
        shadowNode, "split_lambda", shadow.splitLambda);
    shadow.bias = Util::readFloat(shadowNode, "bias", shadow.bias);
    shadow.normalBias = Util::readFloat(
        shadowNode, "normal_bias", shadow.normalBias);

    const bool hasPcfRadius = shadowNode.has_child("pcf_radius");
    shadow.pcfRadius = Util::readInt(
        shadowNode, "pcf_radius", shadow.pcfRadius);
    if (shadowNode.has_child("pcf_radius_near")) {
        shadow.pcfRadiusNear = Util::readInt(
            shadowNode, "pcf_radius_near", shadow.pcfRadiusNear);
        pcfState.hasNearOverride = true;
    } else if (hasPcfRadius && !pcfState.hasNearOverride) {
        shadow.pcfRadiusNear = shadow.pcfRadius;
    }
    if (shadowNode.has_child("pcf_radius_far")) {
        shadow.pcfRadiusFar = Util::readInt(
            shadowNode, "pcf_radius_far", shadow.pcfRadiusFar);
        pcfState.hasFarOverride = true;
    } else if (hasPcfRadius && !pcfState.hasFarOverride) {
        shadow.pcfRadiusFar = shadow.pcfRadius;
    }

    shadow.transparentScale = Util::readFloat(
        shadowNode, "transparent_scale", shadow.transparentScale);
    shadow.strength = Util::readFloat(
        shadowNode, "strength", shadow.strength);
    shadow.fadePower = Util::readFloat(
        shadowNode, "fade_power", shadow.fadePower);

    shadow.cascades = std::clamp(
        shadow.cascades, 1, Voxel::ShadowConfig::MaxCascades);
    shadow.mapSize = std::max(1, shadow.mapSize);
    shadow.pcfRadius = std::max(0, shadow.pcfRadius);
    shadow.pcfRadiusNear = std::max(0, shadow.pcfRadiusNear);
    shadow.pcfRadiusFar = std::max(0, shadow.pcfRadiusFar);
    shadow.transparentScale = std::max(0.0f, shadow.transparentScale);
    shadow.strength = std::max(0.0f, shadow.strength);
    shadow.fadePower = std::max(0.0f, shadow.fadePower);
}

void applyTaaConfig(ryml::ConstNodeRef taaNode, Voxel::TaaConfig& taa) {
    if (!taaNode.readable()) {
        return;
    }

    taa.enabled = Util::readBool(taaNode, "enabled", taa.enabled);
    taa.blend = std::clamp(
        Util::readFloat(taaNode, "blend", taa.blend), 0.0f, 1.0f);
    taa.jitterScale = std::max(
        0.0f, Util::readFloat(taaNode, "jitter_scale", taa.jitterScale));
}

void applyRenderYaml(const char* sourceName,
                     const std::string& yaml,
                     Voxel::WorldRenderConfig& config,
                     PcfRadiusState& pcfState) {
    if (yaml.empty()) {
        return;
    }

    ryml::Tree tree = ryml::parse_in_arena(
        ryml::to_csubstr(sourceName),
        ryml::to_csubstr(yaml)
    );
    const ryml::ConstNodeRef root = tree.rootref();
    ryml::ConstNodeRef renderNode = root;
    if (root.has_child("render")) {
        renderNode = root["render"];
    }
    if (!renderNode.readable()) {
        return;
    }
    validateRenderConfigKeys(root, renderNode, sourceName);

    readVec3(renderNode, "sun_direction", config.sunDirection);
    config.transparentAlpha = Util::readFloat(
        renderNode, "transparent_alpha", config.transparentAlpha);
    config.renderDistance = Util::readFloat(
        renderNode, "render_distance", config.renderDistance);

    if (renderNode.has_child("shadow")) {
        applyShadowConfig(renderNode["shadow"], config.shadow, pcfState);
    }
    if (renderNode.has_child("taa")) {
        applyTaaConfig(renderNode["taa"], config.taa);
    }
    if (renderNode.has_child("profiling")) {
        const auto profilingNode = renderNode["profiling"];
        if (profilingNode.readable()) {
            config.profilingEnabled = Util::readBool(
                profilingNode, "enabled", config.profilingEnabled);
        }
    }
}

} // namespace

void RenderConfigProvider::addSource(
    std::unique_ptr<Config::IConfigSource> source) {
    m_sources.push_back(std::move(source));
}

Voxel::WorldRenderConfig RenderConfigProvider::load() const {
    Voxel::WorldRenderConfig config;
    PcfRadiusState pcfState;
    for (const auto& source : m_sources) {
        auto yaml = source->load();
        if (yaml) {
            applyRenderYaml(source->name().c_str(), *yaml, config, pcfState);
        }
    }
    return config;
}

} // namespace Rigel::Render
