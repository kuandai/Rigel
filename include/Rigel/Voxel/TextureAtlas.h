#pragma once

/**
 * @file TextureAtlas.h
 * @brief Texture atlas for block textures.
 *
 * TextureAtlas packs multiple block textures into an OpenGL array texture
 * to minimize texture binds during rendering.
 */

#include <array>
#include <string>
#include <vector>
#include <unordered_map>
#include <GL/glew.h>

namespace Rigel::Voxel {

/**
 * @brief Handle to a texture in the atlas.
 */
struct TextureHandle {
    uint16_t index = 0;

    bool operator==(const TextureHandle&) const = default;

    /// Check if this is a valid handle
    bool isValid() const { return index != UINT16_MAX; }

    /// Invalid handle constant
    static TextureHandle invalid() { return TextureHandle{UINT16_MAX}; }
};

/**
 * @brief UV coordinates and layer for a texture region.
 */
struct TextureCoords {
    float u0, v0;  ///< Top-left UV
    float u1, v1;  ///< Bottom-right UV
    int layer;     ///< Array texture layer
};

/**
 * @brief Texture atlas using OpenGL array textures.
 *
 * Packs block textures into array texture layers. Each texture gets
 * its own layer, eliminating the need for complex packing algorithms
 * while maintaining simple UV coordinates.
 *
 * @section usage Usage
 *
 * @code
 * TextureAtlas atlas;
 *
 * // Add textures before uploading
 * TextureHandle stone = atlas.addTexture("textures/stone.png");
 * TextureHandle dirt = atlas.addTexture("textures/dirt.png");
 *
 * // Upload to GPU
 * atlas.upload();
 *
 * // Use during rendering
 * atlas.bind(0);  // Bind to texture unit 0
 * @endcode
 *
 * @section format Texture Format
 *
 * All textures must be the same size (default 16x16). Textures are
 * loaded with stb_image and converted to RGBA format.
 *
 * @note Requires valid OpenGL context for upload() and bind().
 */
class TextureAtlas {
public:
    /**
     * @brief Atlas configuration.
     */
    struct Config {
        int tileSize = 16;        ///< Pixels per tile (width and height)
        int maxLayers = 256;      ///< Maximum array texture depth
        bool generateMipmaps = true;  ///< Generate mipmaps for minification
    };

    /**
     * @brief Construct atlas with default configuration.
     */
    TextureAtlas();

    /**
     * @brief Construct atlas with configuration.
     */
    explicit TextureAtlas(const Config& config);

    /**
     * @brief Destructor. Releases GPU resources.
     */
    ~TextureAtlas();

    /// Non-copyable
    TextureAtlas(const TextureAtlas&) = delete;
    TextureAtlas& operator=(const TextureAtlas&) = delete;

    /// Movable
    TextureAtlas(TextureAtlas&& other) noexcept;
    TextureAtlas& operator=(TextureAtlas&& other) noexcept;

    /**
     * @brief Add a texture from raw pixel data.
     *
     * @param path Identifier for the texture (for lookup)
     * @param pixels RGBA pixel data (tileSize * tileSize * 4 bytes)
     * @return Handle to the texture
     *
     * @throws std::runtime_error if max layers exceeded
     */
    TextureHandle addTexture(const std::string& path, const unsigned char* pixels);

    /**
     * @brief Add a texture by loading from embedded resources.
     *
     * Uses stb_image to load the texture from ResourceRegistry.
     *
     * @param path Resource path (e.g., "textures/stone.png")
     * @return Handle to the texture
     *
     * @throws std::runtime_error if loading fails or max layers exceeded
     */
    TextureHandle addTextureFromResource(const std::string& path);

    /**
     * @brief Find texture handle by path.
     *
     * @param path The texture path
     * @return Handle if found, invalid handle otherwise
     */
    TextureHandle findTexture(const std::string& path) const;

    /**
     * @brief Get UV coordinates for a texture.
     *
     * @param handle The texture handle
     * @return UV coordinates and layer
     */
    TextureCoords getUVs(TextureHandle handle) const;

    /**
     * @brief Get array layer for a texture.
     *
     * @param handle The texture handle
     * @return Layer index
     */
    int getLayer(TextureHandle handle) const;

    /**
     * @brief Upload all textures to GPU.
     *
     * Creates the array texture and uploads all added textures.
     * Can be called multiple times to re-upload.
     */
    void upload();

    /**
     * @brief Bind the atlas texture to a texture unit.
     *
     * @param unit Texture unit (0-31)
     */
    void bind(GLuint unit = 0) const;

    /**
     * @brief Bind the tint atlas texture to a texture unit.
     *
     * @param unit Texture unit (0-31)
     */
    void bindTint(GLuint unit = 0) const;

    /**
     * @brief Get the OpenGL texture ID.
     */
    GLuint textureId() const { return m_textureArray; }

    /**
     * @brief Get the OpenGL tint texture ID.
     */
    GLuint tintTextureId() const { return m_tintArray; }

    /**
     * @brief Check if atlas has been uploaded.
     */
    bool isUploaded() const { return m_textureArray != 0; }

    /**
     * @brief Check if tint atlas has been uploaded.
     */
    bool isTintUploaded() const { return m_tintArray != 0; }

    /**
     * @brief Get number of textures in atlas.
     */
    size_t textureCount() const { return m_entries.size(); }

    /**
     * @brief Get tile size in pixels.
     */
    int tileSize() const { return m_config.tileSize; }

    /**
     * @brief Release GPU resources.
     *
     * Safe to call multiple times. Requires a valid OpenGL context.
     */
    void releaseGPU();

private:
    Config m_config;
    GLuint m_textureArray = 0;
    GLuint m_tintArray = 0;

    struct TextureEntry {
        std::string path;
        std::vector<unsigned char> pixels;  // RGBA data
        std::array<unsigned char, 4> tint{};
        int layer;
    };

    std::vector<TextureEntry> m_entries;
    std::unordered_map<std::string, TextureHandle> m_pathToHandle;
};

} // namespace Rigel::Voxel
