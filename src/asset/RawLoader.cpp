#include "Rigel/Asset/RawLoader.h"
#include "Rigel/Asset/AssetManager.h"
#include "Rigel/Asset/Types.h"
#include "ResourceRegistry.h"

#include <spdlog/spdlog.h>
#include <ryml.hpp>
#include <ryml_std.hpp>

namespace Rigel::Asset {

namespace {
    std::optional<std::string> getString(ryml::ConstNodeRef config, const char* key) {
        if (!config.readable() || !config.has_child(ryml::to_csubstr(key))) {
            return std::nullopt;
        }
        std::string value;
        config[ryml::to_csubstr(key)] >> value;
        return value;
    }
}

std::shared_ptr<AssetBase> RawLoader::load(const LoadContext& ctx) {
    auto pathOpt = getString(ctx.config, "path");
    if (!pathOpt) {
        throw AssetLoadError(ctx.id, "Raw asset missing 'path' field");
    }

    spdlog::debug("Loading raw asset: {} from {}", ctx.id, *pathOpt);

    auto data = ctx.loadResource(*pathOpt);

    auto asset = std::make_shared<RawAsset>();
    asset->data.assign(data.begin(), data.end());

    return asset;
}

} // namespace Rigel::Asset
