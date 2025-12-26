#pragma once

/**
 * @file TextureLoader.h
 * @brief Image file loader for OpenGL textures.
 *
 * This file provides the TextureLoader class, which loads image files and
 * creates OpenGL 2D textures. The loader uses stb_image internally to support
 * a wide variety of image formats.
 *
 * @section supported_formats Supported Image Formats
 *
 * The following image formats are supported via stb_image:
 * - PNG (recommended for sprites, UI, alpha channels)
 * - JPEG (good for photos, no alpha)
 * - BMP
 * - TGA
 * - GIF (first frame only)
 * - PSD (composited view only)
 * - HDR (radiance .hdr files)
 * - PIC
 *
 * @section manifest_format Manifest Format
 *
 * Textures are defined in the manifest under the "textures" category:
 *
 * @code{.yaml}
 * assets:
 *   textures:
 *     # Basic texture with default settings
 *     stone:
 *       path: textures/stone.png
 *
 *     # Texture with linear filtering (good for smooth gradients)
 *     skybox:
 *       path: textures/skybox.png
 *       filter: linear
 *
 *     # Pixel art texture (uses default nearest filtering)
 *     sprite_sheet:
 *       path: textures/sprites.png
 * @endcode
 *
 * @section configuration Configuration Options
 *
 * | Field | Type | Default | Description |
 * |-------|------|---------|-------------|
 * | path | string | (required) | Path to image file in resources |
 * | filter | string | "nearest" | Texture filtering: "nearest" or "linear" |
 *
 * @section opengl_settings OpenGL Settings
 *
 * The loader configures textures with:
 * - **Format**: Automatically detected (RGB, RGBA, or R based on channels)
 * - **Filtering**: MIN and MAG filters set to NEAREST (default) or LINEAR
 * - **Wrapping**: Both S and T set to GL_REPEAT
 * - **Orientation**: Images are flipped vertically (OpenGL expects bottom-left origin)
 *
 * @section usage Usage
 *
 * @code
 * auto texture = assetManager.get<TextureAsset>("textures/stone");
 * texture->bind(GL_TEXTURE0);  // Bind to texture unit 0
 *
 * // Use in shader
 * glUniform1i(shader->uniform("u_texture"), 0);
 * @endcode
 *
 * @see TextureAsset for the loaded texture wrapper
 */

#include "AssetLoader.h"

namespace Rigel::Asset {

/**
 * @brief Asset loader for OpenGL 2D textures from image files.
 *
 * TextureLoader implements the IAssetLoader interface to load image files
 * and create OpenGL textures. It uses stb_image for image decoding, which
 * supports most common image formats.
 *
 * @section loader_lifecycle Loading Lifecycle
 *
 * When `AssetManager::get<TextureAsset>("textures/myimage")` is called:
 *
 * 1. AssetManager calls `TextureLoader::load()` with the asset's LoadContext
 * 2. TextureLoader reads the "path" field from config
 * 3. Raw image bytes are loaded from ResourceRegistry via `ctx.loadResource()`
 * 4. stb_image decodes the image into pixel data
 * 5. An OpenGL texture is created and configured
 * 6. Pixel data is uploaded with glTexImage2D
 * 7. TextureAsset is created with the texture handle
 * 8. Asset is cached and returned as Handle<TextureAsset>
 *
 * @section format_detection Format Detection
 *
 * The loader automatically detects and uses the appropriate OpenGL format:
 *
 * | Channels | Internal Format | External Format |
 * |----------|-----------------|-----------------|
 * | 1 (grayscale) | GL_R8 | GL_RED |
 * | 3 (RGB) | GL_RGB8 | GL_RGB |
 * | 4 (RGBA) | GL_RGBA8 | GL_RGBA |
 *
 * @section error_handling Error Handling
 *
 * The loader throws exceptions on failure:
 * - **AssetLoadError**: Missing "path" field, image decode failure
 *
 * @section registration Registration
 *
 * TextureLoader is automatically registered with AssetManager when
 * `loadManifest()` is called. Manual registration is not required.
 *
 * @note This loader requires a valid OpenGL context to be current on the
 *       calling thread. Texture creation will fail without a context.
 *
 * @see TextureAsset for the loaded texture wrapper
 */
class TextureLoader : public IAssetLoader {
public:
    /**
     * @brief Get the manifest category this loader handles.
     * @return "textures"
     */
    std::string_view category() const override { return "textures"; }

    /**
     * @brief Load a texture asset from an image file.
     *
     * Loads the image file specified in the "path" config field, decodes it
     * using stb_image, creates an OpenGL texture, and returns a TextureAsset.
     *
     * @param ctx Loading context with asset ID, config, and resource access
     *
     * @return Shared pointer to the loaded TextureAsset
     *
     * @throws AssetLoadError if:
     *         - The "path" field is missing from config
     *         - stb_image fails to decode the image
     *         - The resource path is not found
     *
     * @pre A valid OpenGL context must be current on the calling thread
     *
     * @section texture_load_example Example Manifest Entry
     *
     * @code{.yaml}
     * textures:
     *   grass:
     *     path: textures/grass.png
     *     filter: nearest
     * @endcode
     *
     * After loading:
     * @code
     * auto grass = assets.get<TextureAsset>("textures/grass");
     * // grass->width, grass->height, grass->channels are populated
     * // grass->id contains the OpenGL texture handle
     * @endcode
     */
    std::shared_ptr<AssetBase> load(const LoadContext& ctx) override;
};

} // namespace Rigel::Asset
