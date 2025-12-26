#pragma once

/**
 * @file Types.h
 * @brief Asset type definitions for the asset system.
 *
 * This file defines the concrete asset types that can be loaded through the
 * AssetManager. Each type inherits from AssetBase and implements RAII for
 * any resources it owns (such as OpenGL objects).
 *
 * @section builtin_types Built-in Asset Types
 *
 * | Type | Description | Loader |
 * |------|-------------|--------|
 * | RawAsset | Binary data (configs, custom formats) | RawLoader |
 * | TextureAsset | OpenGL 2D textures from images | TextureLoader |
 * | ShaderAsset | Compiled GLSL shader programs | ShaderLoader |
 *
 * @section usage Usage
 *
 * Assets are accessed through the AssetManager:
 *
 * @code
 * AssetManager assets;
 * assets.loadManifest("manifest.yaml");
 *
 * // Load different asset types
 * auto config = assets.get<RawAsset>("raw/config");
 * auto texture = assets.get<TextureAsset>("textures/stone");
 * auto shader = assets.get<ShaderAsset>("shaders/basic");
 * @endcode
 *
 * @section ownership Resource Ownership
 *
 * Asset types follow RAII principles:
 * - OpenGL resources (textures, programs) are deleted in destructors
 * - Assets are non-copyable to prevent double-deletion
 * - Assets are movable for efficient container operations
 * - AssetManager holds shared_ptr to assets for reference counting
 *
 * @see AssetManager for loading and caching
 * @see Handle for type-safe asset references
 */

#include "AssetLoader.h"

#include <vector>
#include <string>
#include <string_view>
#include <unordered_map>
#include <cstdint>
#include <GL/glew.h>

namespace Rigel::Asset {

/**
 * @brief Raw binary data asset for configuration files and custom formats.
 *
 * RawAsset stores a copy of file data as a vector of bytes. This is useful
 * for files that need custom parsing (YAML, JSON, custom binary formats).
 *
 * @section usage Usage
 *
 * @code
 * auto config = assets.get<RawAsset>("raw/config");
 *
 * // Access raw bytes
 * const std::vector<char>& bytes = config->data;
 *
 * // For text files, use str() for a string_view
 * std::string_view text = config->str();
 *
 * // Parse as YAML
 * ryml::Tree tree = ryml::parse_in_arena(
 *     ryml::csubstr(text.data(), text.size())
 * );
 * @endcode
 *
 * @section manifest Manifest Configuration
 *
 * @code{.yaml}
 * raw:
 *   config:
 *     path: config.yaml
 * @endcode
 *
 * @note Unlike LoadContext::loadResource() which returns a view into static
 *       data, RawAsset owns a copy of the data that persists with the asset.
 *
 * @see RawLoader for loading implementation
 */
struct RawAsset : AssetBase {
    /**
     * @brief The raw file data.
     *
     * Contains a copy of the file contents as loaded from the ResourceRegistry.
     * For text files, this includes any encoding (typically UTF-8).
     */
    std::vector<char> data;

    /**
     * @brief Get the data as a string view.
     *
     * Convenience method for accessing text-based files as a string_view.
     * No decoding is performed; the view represents the raw bytes.
     *
     * @return String view over the data vector
     *
     * @note The returned view is valid as long as this RawAsset exists
     *       and the data vector is not modified.
     */
    std::string_view str() const {
        return std::string_view(data.data(), data.size());
    }
};

/**
 * @brief OpenGL 2D texture asset loaded from an image file.
 *
 * TextureAsset wraps an OpenGL texture object (GLuint) with RAII semantics.
 * The texture is automatically deleted when the last reference is released.
 *
 * @section usage Usage
 *
 * @code
 * auto texture = assets.get<TextureAsset>("textures/stone");
 *
 * // Bind to a texture unit
 * texture->bind(GL_TEXTURE0);
 *
 * // Access properties
 * int w = texture->width;   // Texture width in pixels
 * int h = texture->height;  // Texture height in pixels
 * int c = texture->channels; // 1=R, 3=RGB, 4=RGBA
 *
 * // Use OpenGL handle directly
 * glBindTexture(GL_TEXTURE_2D, texture->id);
 * @endcode
 *
 * @section manifest Manifest Configuration
 *
 * @code{.yaml}
 * textures:
 *   stone:
 *     path: textures/stone.png
 *     filter: nearest   # or "linear"
 * @endcode
 *
 * @section opengl OpenGL Properties
 *
 * Textures are created with:
 * - Type: GL_TEXTURE_2D
 * - Format: GL_R8, GL_RGB8, or GL_RGBA8 (based on channels)
 * - Filtering: GL_NEAREST (default) or GL_LINEAR
 * - Wrapping: GL_REPEAT for both S and T
 *
 * @note Requires a valid OpenGL context when loading and when the
 *       destructor runs.
 *
 * @see TextureLoader for loading implementation
 */
struct TextureAsset : AssetBase {
    /**
     * @brief OpenGL texture object handle.
     *
     * Created by glGenTextures(). Valid handles are non-zero.
     * Automatically deleted by glDeleteTextures() in destructor.
     */
    GLuint id = 0;

    /**
     * @brief Texture width in pixels.
     */
    int width = 0;

    /**
     * @brief Texture height in pixels.
     */
    int height = 0;

    /**
     * @brief Number of color channels.
     *
     * - 1: Grayscale (GL_RED format)
     * - 3: RGB (GL_RGB format)
     * - 4: RGBA (GL_RGBA format)
     */
    int channels = 0;

    /**
     * @brief Default constructor. Creates an empty texture asset.
     */
    TextureAsset() = default;

    /// @name Non-copyable
    /// TextureAsset owns an OpenGL resource and cannot be copied.
    /// @{
    TextureAsset(const TextureAsset&) = delete;
    TextureAsset& operator=(const TextureAsset&) = delete;
    /// @}

    /**
     * @brief Move constructor. Transfers ownership of the texture.
     * @param other The texture to move from. Will have id set to 0.
     */
    TextureAsset(TextureAsset&& other) noexcept
        : id(other.id)
        , width(other.width)
        , height(other.height)
        , channels(other.channels)
    {
        other.id = 0;
    }

    /**
     * @brief Move assignment. Releases current texture and takes ownership.
     * @param other The texture to move from. Will have id set to 0.
     * @return Reference to this.
     */
    TextureAsset& operator=(TextureAsset&& other) noexcept {
        if (this != &other) {
            release();
            id = other.id;
            width = other.width;
            height = other.height;
            channels = other.channels;
            other.id = 0;
        }
        return *this;
    }

    /**
     * @brief Destructor. Releases the OpenGL texture if owned.
     * @note Requires a valid OpenGL context.
     */
    ~TextureAsset() {
        release();
    }

    /**
     * @brief Bind this texture to a texture unit.
     *
     * Activates the specified texture unit and binds this texture to it.
     * After binding, the texture can be sampled in shaders.
     *
     * @param unit The texture unit to bind to (GL_TEXTURE0, GL_TEXTURE1, etc.)
     *             Defaults to GL_TEXTURE0.
     *
     * @code
     * texture->bind(GL_TEXTURE0);
     * shader->bind();
     * glUniform1i(shader->uniform("u_texture"), 0);  // Unit 0
     * @endcode
     */
    void bind(GLenum unit = GL_TEXTURE0) const {
        glActiveTexture(unit);
        glBindTexture(GL_TEXTURE_2D, id);
    }

private:
    /**
     * @brief Release the OpenGL texture resource.
     */
    void release() {
        if (id != 0) {
            glDeleteTextures(1, &id);
            id = 0;
        }
    }
};

/**
 * @brief Compiled OpenGL shader program with uniform/attribute caching.
 *
 * ShaderAsset wraps an OpenGL program object (GLuint) with RAII semantics
 * and provides convenient access to uniforms and attributes with lazy caching.
 *
 * @section usage Usage
 *
 * @code
 * auto shader = assets.get<ShaderAsset>("shaders/lit");
 *
 * // Bind the shader program
 * shader->bind();
 *
 * // Set uniforms (locations are cached automatically)
 * glUniformMatrix4fv(shader->uniform("u_mvp"), 1, GL_FALSE, &mvp[0][0]);
 * glUniform3fv(shader->uniform("u_lightPos"), 1, &lightPos[0]);
 * glUniform1f(shader->uniform("u_time"), time);
 *
 * // Get attribute locations for vertex buffer setup
 * GLint posLoc = shader->attribute("a_position");
 * GLint texLoc = shader->attribute("a_texcoord");
 * @endcode
 *
 * @section manifest Manifest Configuration
 *
 * @code{.yaml}
 * shaders:
 *   lit:
 *     vertex: shaders/lit.vert
 *     fragment: shaders/lit.frag
 *     defines:
 *       MAX_LIGHTS: 4
 *       USE_SHADOWS: true
 * @endcode
 *
 * @section caching Uniform and Attribute Caching
 *
 * The first call to uniform() or attribute() for a given name queries OpenGL
 * and caches the result. Subsequent calls return the cached value directly,
 * avoiding the overhead of glGetUniformLocation/glGetAttribLocation.
 *
 * Cache behavior:
 * - Locations are cached per-name, per-ShaderAsset instance
 * - Cache is cleared when the shader is moved or destroyed
 * - Missing uniforms/attributes return -1 (with a warning logged)
 *
 * @section inheritance Shader Inheritance
 *
 * Shaders can inherit from other shaders in the manifest:
 *
 * @code{.yaml}
 * shaders:
 *   base_lit:
 *     vertex: shaders/standard.vert
 *     fragment: shaders/lit.frag
 *
 *   textured_lit:
 *     inherit: shaders/base_lit
 *     defines:
 *       USE_TEXTURE: true
 * @endcode
 *
 * @note Requires a valid OpenGL context when loading, using, and when
 *       the destructor runs.
 *
 * @see ShaderLoader for loading implementation
 * @see ShaderCompiler for compilation details
 */
struct ShaderAsset : AssetBase {
    /**
     * @brief OpenGL program object handle.
     *
     * Created by glCreateProgram() and linked with glLinkProgram().
     * Valid handles are non-zero. Automatically deleted by
     * glDeleteProgram() in destructor.
     */
    GLuint program = 0;

    /**
     * @brief Default constructor. Creates an empty shader asset.
     */
    ShaderAsset() = default;

    /// @name Non-copyable
    /// ShaderAsset owns an OpenGL resource and cannot be copied.
    /// @{
    ShaderAsset(const ShaderAsset&) = delete;
    ShaderAsset& operator=(const ShaderAsset&) = delete;
    /// @}

    /**
     * @brief Move constructor. Transfers ownership of the program.
     * @param other The shader to move from. Will have program set to 0.
     */
    ShaderAsset(ShaderAsset&& other) noexcept;

    /**
     * @brief Move assignment. Releases current program and takes ownership.
     * @param other The shader to move from. Will have program set to 0.
     * @return Reference to this.
     */
    ShaderAsset& operator=(ShaderAsset&& other) noexcept;

    /**
     * @brief Destructor. Releases the OpenGL program if owned.
     * @note Requires a valid OpenGL context.
     */
    ~ShaderAsset();

    /**
     * @brief Bind this shader program for use.
     *
     * Equivalent to glUseProgram(program). After binding, this shader
     * will be used for subsequent draw calls.
     *
     * @code
     * shader->bind();
     * // Set uniforms...
     * // Draw...
     * @endcode
     */
    void bind() const { glUseProgram(program); }

    /**
     * @brief Get the location of a uniform variable (cached).
     *
     * Returns the location of the specified uniform variable in this
     * shader program. The location is cached after the first lookup,
     * so subsequent calls for the same name are very fast.
     *
     * @param name The name of the uniform variable (e.g., "u_mvp")
     * @return The uniform location, or -1 if not found
     *
     * @note A warning is logged if the uniform is not found. This often
     *       indicates a typo in the uniform name or that the uniform
     *       was optimized out by the compiler (if unused).
     *
     * @code
     * GLint loc = shader->uniform("u_color");
     * if (loc != -1) {
     *     glUniform4f(loc, 1.0f, 0.0f, 0.0f, 1.0f);
     * }
     * @endcode
     */
    GLint uniform(const std::string& name) const;

    /**
     * @brief Get the location of a vertex attribute (cached).
     *
     * Returns the location of the specified vertex attribute in this
     * shader program. The location is cached after the first lookup.
     *
     * @param name The name of the attribute variable (e.g., "a_position")
     * @return The attribute location, or -1 if not found
     *
     * @note Attribute locations can also be set explicitly in shader code:
     *       `layout(location = 0) in vec3 a_position;`
     *
     * @code
     * GLint posLoc = shader->attribute("a_position");
     * glVertexAttribPointer(posLoc, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
     * glEnableVertexAttribArray(posLoc);
     * @endcode
     */
    GLint attribute(const std::string& name) const;

private:
    /**
     * @brief Cache of uniform name → location mappings.
     *
     * Populated lazily by uniform(). Marked mutable because lookups
     * may populate the cache even on const shader objects.
     */
    mutable std::unordered_map<std::string, GLint> m_uniformCache;

    /**
     * @brief Cache of attribute name → location mappings.
     *
     * Populated lazily by attribute(). Marked mutable because lookups
     * may populate the cache even on const shader objects.
     */
    mutable std::unordered_map<std::string, GLint> m_attributeCache;

    /**
     * @brief Release the OpenGL program and clear caches.
     */
    void release();
};

} // namespace Rigel::Asset
