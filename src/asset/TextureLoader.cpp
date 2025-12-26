#include "Rigel/Asset/TextureLoader.h"
#include "Rigel/Asset/AssetManager.h"
#include "Rigel/Asset/Types.h"
#include "ResourceRegistry.h"

#include <stb_image.h>
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

std::shared_ptr<AssetBase> TextureLoader::load(const LoadContext& ctx) {
    auto pathOpt = getString(ctx.config, "path");
    if (!pathOpt) {
        throw AssetLoadError(ctx.id, "Texture asset missing 'path' field");
    }

    spdlog::debug("Loading texture asset: {} from {}", ctx.id, *pathOpt);

    // Get raw image data
    auto data = ctx.loadResource(*pathOpt);

    // Load with stb_image
    int width, height, channels;
    stbi_set_flip_vertically_on_load(true);  // OpenGL expects bottom-left origin

    unsigned char* pixels = stbi_load_from_memory(
        reinterpret_cast<const unsigned char*>(data.data()),
        static_cast<int>(data.size()),
        &width, &height, &channels, 0
    );

    if (!pixels) {
        throw AssetLoadError(ctx.id, std::string("stb_image failed: ") + stbi_failure_reason());
    }

    // Create OpenGL texture
    auto asset = std::make_shared<TextureAsset>();
    asset->width = width;
    asset->height = height;
    asset->channels = channels;

    glGenTextures(1, &asset->id);
    glBindTexture(GL_TEXTURE_2D, asset->id);

    // Determine format
    GLenum format = GL_RGB;
    GLenum internalFormat = GL_RGB8;
    if (channels == 4) {
        format = GL_RGBA;
        internalFormat = GL_RGBA8;
    } else if (channels == 1) {
        format = GL_RED;
        internalFormat = GL_R8;
    }

    // Upload texture data
    glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, width, height, 0,
                 format, GL_UNSIGNED_BYTE, pixels);

    // Set filtering based on properties
    GLenum minFilter = GL_NEAREST;
    GLenum magFilter = GL_NEAREST;

    auto filterOpt = getString(ctx.config, "filter");
    if (filterOpt && *filterOpt == "linear") {
        minFilter = GL_LINEAR;
        magFilter = GL_LINEAR;
    }

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minFilter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, magFilter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    // Free stb_image data
    stbi_image_free(pixels);

    spdlog::debug("Loaded texture: {}x{} ({} channels)", width, height, channels);

    return asset;
}

} // namespace Rigel::Asset
