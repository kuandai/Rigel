#pragma once

/**
 * @file ShaderLoader.h
 * @brief Manifest-based vertex/fragment shader asset loader.
 */

#include "AssetLoader.h"

#include <optional>
#include <string>

namespace Rigel::Asset {

/**
 * @brief Loads required vertex and fragment sources and compiles a shader asset.
 *
 * Shader manifest entries accept exactly two fields:
 *
 * @code{.yaml}
 * shaders:
 *   basic:
 *     vertex: shaders/basic.vert
 *     fragment: shaders/basic.frag
 * @endcode
 */
class ShaderLoader : public IAssetLoader {
public:
    std::string_view category() const override { return "shaders"; }
    std::shared_ptr<AssetBase> load(const LoadContext& ctx) override;

private:
    static void validateConfig(const LoadContext& ctx);
    static std::optional<std::string> getString(ryml::ConstNodeRef config, const char* key);
};

} // namespace Rigel::Asset
