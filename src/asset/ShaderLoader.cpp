#include "Rigel/Asset/ShaderLoader.h"
#include "Rigel/Asset/ShaderCompiler.h"
#include "Rigel/Asset/AssetManager.h"
#include "Rigel/Asset/Types.h"

#include <ryml_std.hpp>
#include <spdlog/spdlog.h>
#include <unordered_set>

namespace Rigel::Asset {

namespace {
    std::string toStdString(ryml::csubstr s) {
        return std::string(s.data(), s.size());
    }
}

std::optional<std::string> ShaderLoader::getString(ryml::ConstNodeRef config, const char* key) {
    if (!config.readable() || !config.has_child(ryml::to_csubstr(key))) {
        return std::nullopt;
    }
    std::string value;
    config[ryml::to_csubstr(key)] >> value;
    return value;
}

void ShaderLoader::extractDefines(
    ryml::ConstNodeRef node,
    std::unordered_map<std::string, std::string>& defines
) {
    if (!node.readable() || !node.has_child("defines")) {
        return;
    }

    ryml::ConstNodeRef definesNode = node["defines"];
    for (ryml::ConstNodeRef child : definesNode.children()) {
        std::string key = toStdString(child.key());
        std::string value;
        child >> value;
        defines[key] = value;
    }
}

ResolvedShaderConfig ShaderLoader::resolveConfig(const LoadContext& ctx) {
    ResolvedShaderConfig result;

    // Check for inheritance
    auto inheritOpt = getString(ctx.config, "inherit");
    if (inheritOpt) {
        std::string parentId = *inheritOpt;
        spdlog::debug("Shader '{}' inherits from '{}'", ctx.id, parentId);

        // Get parent's config entry (not the compiled shader)
        const auto* parentEntry = ctx.manager.getEntry(parentId);
        if (!parentEntry) {
            throw AssetLoadError(ctx.id, "Parent shader '" + parentId + "' not found");
        }

        // Recursively resolve parent (to support multi-level inheritance)
        // Create a temporary context for the parent
        LoadContext parentCtx{parentId, parentEntry->config, ctx.manager};
        result = resolveConfig(parentCtx);

        spdlog::debug("Inherited from '{}': vertex={}, fragment={}",
                      parentId, result.vertex, result.fragment);
    }

    // Override with this shader's values (if specified)
    auto vertexOpt = getString(ctx.config, "vertex");
    if (vertexOpt) {
        result.vertex = *vertexOpt;
    }

    auto fragmentOpt = getString(ctx.config, "fragment");
    if (fragmentOpt) {
        result.fragment = *fragmentOpt;
    }

    auto geometryOpt = getString(ctx.config, "geometry");
    if (geometryOpt) {
        result.geometry = *geometryOpt;
    }

    auto computeOpt = getString(ctx.config, "compute");
    if (computeOpt) {
        result.compute = *computeOpt;
    }

    // Merge defines (child values override parent)
    extractDefines(ctx.config, result.defines);

    return result;
}

std::shared_ptr<AssetBase> ShaderLoader::load(const LoadContext& ctx) {
    // Resolve configuration (handles inheritance)
    ResolvedShaderConfig config = resolveConfig(ctx);

    // Build ShaderSource
    ShaderSource source;
    source.defines = config.defines;

    // Determine if this is a compute shader
    if (config.compute) {
        // Compute shader
        auto data = ctx.loadResource(*config.compute);
        source.compute = std::string(data.data(), data.size());
        spdlog::debug("Loaded compute shader source: {}", *config.compute);
    } else {
        // Graphics shader - load vertex and fragment
        if (config.vertex.empty()) {
            throw AssetLoadError(ctx.id, "Shader missing 'vertex' source");
        }

        std::string fragmentPath = config.fragment;
        if (fragmentPath.empty() && !config.vertex.empty()) {
            std::string candidate = config.vertex;
            size_t pos = candidate.rfind(".vert");
            if (pos != std::string::npos) {
                candidate.replace(pos, 5, ".frag");
                fragmentPath = candidate;
            }
        }
        if (fragmentPath.empty()) {
            throw AssetLoadError(ctx.id, "Shader missing 'fragment' source");
        }

        auto vertData = ctx.loadResource(config.vertex);
        source.vertex = std::string(vertData.data(), vertData.size());
        spdlog::debug("Loaded vertex shader source: {}", config.vertex);

        auto fragData = ctx.loadResource(fragmentPath);
        source.fragment = std::string(fragData.data(), fragData.size());
        spdlog::debug("Loaded fragment shader source: {}", fragmentPath);

        // Optional geometry shader
        if (config.geometry) {
            auto geomData = ctx.loadResource(*config.geometry);
            source.geometry = std::string(geomData.data(), geomData.size());
            spdlog::debug("Loaded geometry shader source: {}", *config.geometry);
        }
    }

    // Log defines
    if (!source.defines.empty()) {
        spdlog::debug("Shader defines:");
        for (const auto& [key, value] : source.defines) {
            spdlog::debug("  {} = {}", key, value);
        }
    }

    // Compile the shader
    GLuint program = ShaderCompiler::compile(source, ctx.id);

    // Create ShaderAsset
    auto asset = std::make_shared<ShaderAsset>();
    asset->program = program;

    spdlog::debug("Loaded shader '{}' (program={})", ctx.id, program);

    return asset;
}

} // namespace Rigel::Asset
