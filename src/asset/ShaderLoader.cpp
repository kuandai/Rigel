#include "Rigel/Asset/ShaderLoader.h"
#include "Rigel/Asset/ShaderCompiler.h"
#include "Rigel/Asset/AssetManager.h"
#include "Rigel/Asset/Types.h"
#include "Rigel/Util/Ryml.h"

#include <spdlog/spdlog.h>

namespace Rigel::Asset {

std::optional<std::string> ShaderLoader::getString(ryml::ConstNodeRef config, const char* key) {
    if (!config.readable() || !config.has_child(ryml::to_csubstr(key))) {
        return std::nullopt;
    }
    std::string value;
    config[ryml::to_csubstr(key)] >> value;
    return value;
}

void ShaderLoader::validateConfig(const LoadContext& ctx) {
    for (ryml::ConstNodeRef child : ctx.config.children()) {
        const std::string key = Util::toStdString(child.key());
        if (key != "vertex" && key != "fragment") {
            throw AssetLoadError(
                ctx.id,
                "Shader configuration contains unsupported field '" + key + "'");
        }
    }
}

std::shared_ptr<AssetBase> ShaderLoader::load(const LoadContext& ctx) {
    validateConfig(ctx);

    const auto vertexPath = getString(ctx.config, "vertex");
    const auto fragmentPath = getString(ctx.config, "fragment");
    if (!vertexPath || vertexPath->empty()) {
        throw AssetLoadError(ctx.id, "Shader missing 'vertex' source");
    }
    if (!fragmentPath || fragmentPath->empty()) {
        throw AssetLoadError(ctx.id, "Shader missing 'fragment' source");
    }

    const auto vertexData = ctx.loadResource(*vertexPath);
    const auto fragmentData = ctx.loadResource(*fragmentPath);
    ShaderSource source{
        std::string(vertexData.data(), vertexData.size()),
        std::string(fragmentData.data(), fragmentData.size())
    };
    spdlog::debug("Loaded vertex shader source: {}", *vertexPath);
    spdlog::debug("Loaded fragment shader source: {}", *fragmentPath);

    auto asset = std::make_shared<ShaderAsset>();
    asset->program = ShaderCompiler::compile(source, ctx.id);

    spdlog::debug("Loaded shader '{}' (program={})", ctx.id, asset->program);

    return asset;
}

} // namespace Rigel::Asset
