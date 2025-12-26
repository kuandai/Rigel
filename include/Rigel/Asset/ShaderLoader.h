#pragma once

/**
 * @file ShaderLoader.h
 * @brief Manifest-based shader asset loader with inheritance support.
 *
 * This file provides the ShaderLoader class, which handles loading shader
 * assets from YAML manifest configuration. The loader supports:
 * - Graphics shaders (vertex + fragment + optional geometry)
 * - Compute shaders
 * - Shader inheritance (child shaders can inherit from parent shaders)
 * - Preprocessor defines with merging
 *
 * @section manifest_format Manifest Format
 *
 * Shaders are defined in the manifest under the "shaders" category:
 *
 * @code{.yaml}
 * assets:
 *   shaders:
 *     # Basic graphics shader
 *     basic:
 *       vertex: shaders/basic.vert
 *       fragment: shaders/basic.frag
 *
 *     # Shader with optional geometry stage
 *     wireframe:
 *       vertex: shaders/standard.vert
 *       geometry: shaders/wireframe.geom
 *       fragment: shaders/wireframe.frag
 *
 *     # Shader with defines
 *     lit:
 *       vertex: shaders/standard.vert
 *       fragment: shaders/lit.frag
 *       defines:
 *         MAX_LIGHTS: 4
 *         USE_SHADOWS: true
 *
 *     # Compute shader
 *     particle_update:
 *       compute: shaders/particle.comp
 *       defines:
 *         WORK_GROUP_SIZE: 256
 * @endcode
 *
 * @section inheritance Shader Inheritance
 *
 * Shaders can inherit from other shaders using the `inherit` field. Child
 * shaders receive all configuration from the parent and can override specific
 * fields:
 *
 * @code{.yaml}
 * shaders:
 *   # Base shader with common settings
 *   standard_base:
 *     vertex: shaders/standard.vert
 *     fragment: shaders/standard.frag
 *     defines:
 *       USE_NORMAL_MAP: false
 *       USE_SPECULAR: true
 *
 *   # Override fragment shader, inherit vertex and defines
 *   standard_lit:
 *     inherit: shaders/standard_base
 *     fragment: shaders/lit.frag
 *
 *   # Override defines, inherit shaders
 *   standard_normalmap:
 *     inherit: shaders/standard_base
 *     defines:
 *       USE_NORMAL_MAP: true  # Overrides parent's false
 * @endcode
 *
 * Inheritance rules:
 * - Child's shader sources override parent's (if specified)
 * - Child's defines merge with parent's (child values take precedence)
 * - Multi-level inheritance is supported (grandparent → parent → child)
 *
 * @section usage Usage
 *
 * @code
 * auto shader = assetManager.get<ShaderAsset>("shaders/lit");
 * shader->bind();
 * shader->uniform("u_lightPos") = glm::vec3(0, 10, 0);
 * @endcode
 *
 * @see ShaderAsset for the compiled shader wrapper
 * @see ShaderCompiler for the low-level compilation API
 */

#include "AssetLoader.h"

#include <optional>
#include <string>
#include <unordered_map>
#include <ryml.hpp>

namespace Rigel::Asset {

/**
 * @brief Fully resolved shader configuration after processing inheritance.
 *
 * This struct represents the final shader configuration after all inheritance
 * has been resolved. It contains the paths to shader source files and the
 * merged set of preprocessor defines.
 *
 * ResolvedShaderConfig is an intermediate representation used internally by
 * ShaderLoader. After resolution, the paths are used to load source code,
 * which is then passed to ShaderCompiler.
 *
 * @section resolution_process Resolution Process
 *
 * 1. If shader has `inherit` field, recursively resolve parent first
 * 2. Start with parent's resolved config (or empty if no parent)
 * 3. Override with child's values where specified
 * 4. Merge child's defines with parent's (child wins on conflicts)
 *
 * @section example Example
 *
 * Given manifest:
 * @code{.yaml}
 * shaders:
 *   parent:
 *     vertex: shaders/a.vert
 *     fragment: shaders/a.frag
 *     defines:
 *       X: 1
 *       Y: 2
 *   child:
 *     inherit: shaders/parent
 *     fragment: shaders/b.frag
 *     defines:
 *       Y: 3
 *       Z: 4
 * @endcode
 *
 * Resolved config for "child":
 * @code
 * ResolvedShaderConfig config;
 * config.vertex = "shaders/a.vert";     // Inherited from parent
 * config.fragment = "shaders/b.frag";   // Overridden by child
 * config.geometry = std::nullopt;       // Not set
 * config.compute = std::nullopt;        // Not set (graphics shader)
 * config.defines = {
 *     {"X", "1"},  // From parent
 *     {"Y", "3"},  // Child overrides parent
 *     {"Z", "4"}   // From child
 * };
 * @endcode
 */
struct ResolvedShaderConfig {
    /**
     * @brief Path to vertex shader source file.
     *
     * Required for graphics shaders. Empty string if not yet resolved
     * or if this is a compute shader.
     */
    std::string vertex;

    /**
     * @brief Path to fragment shader source file.
     *
     * Required for graphics shaders. Empty string if not yet resolved
     * or if this is a compute shader.
     */
    std::string fragment;

    /**
     * @brief Optional path to geometry shader source file.
     *
     * If present, the geometry stage is included in the shader program.
     * Mutually exclusive with compute shaders.
     */
    std::optional<std::string> geometry;

    /**
     * @brief Optional path to compute shader source file.
     *
     * If present, this is a compute shader and vertex/fragment/geometry
     * are ignored. Compute shaders cannot inherit from graphics shaders
     * and vice versa.
     */
    std::optional<std::string> compute;

    /**
     * @brief Merged preprocessor defines from inheritance chain.
     *
     * Contains all defines from the inheritance chain, with child values
     * overriding parent values for duplicate keys.
     */
    std::unordered_map<std::string, std::string> defines;
};

/**
 * @brief Asset loader for GLSL shader programs.
 *
 * ShaderLoader implements the IAssetLoader interface to provide shader loading
 * from YAML manifest configuration. It handles:
 * - Parsing shader configuration from manifest
 * - Resolving inheritance chains
 * - Loading source files from embedded resources
 * - Delegating compilation to ShaderCompiler
 * - Wrapping result in ShaderAsset
 *
 * @section loader_lifecycle Loading Lifecycle
 *
 * When `AssetManager::get<ShaderAsset>("shaders/myshader")` is called:
 *
 * 1. AssetManager calls `ShaderLoader::load()` with the asset's LoadContext
 * 2. ShaderLoader calls `resolveConfig()` to process inheritance
 * 3. Source files are loaded from ResourceRegistry via `ctx.loadResource()`
 * 4. ShaderSource is built and passed to `ShaderCompiler::compile()`
 * 5. ShaderAsset is created with the compiled program handle
 * 6. Asset is cached and returned as Handle<ShaderAsset>
 *
 * @section error_handling Error Handling
 *
 * The loader throws exceptions on failure:
 * - **AssetLoadError**: Missing required fields, parent not found
 * - **ShaderCompileError**: GLSL compilation failure (from ShaderCompiler)
 * - **ShaderLinkError**: Program linking failure (from ShaderCompiler)
 *
 * @section registration Registration
 *
 * ShaderLoader is automatically registered with AssetManager when
 * `loadManifest()` is called. Manual registration is not required.
 *
 * @see ShaderCompiler for compilation details
 * @see ResolvedShaderConfig for inheritance resolution
 */
class ShaderLoader : public IAssetLoader {
public:
    /**
     * @brief Get the manifest category this loader handles.
     * @return "shaders"
     */
    std::string_view category() const override { return "shaders"; }

    /**
     * @brief Load a shader asset from manifest configuration.
     *
     * Resolves inheritance, loads source files, compiles the shader program,
     * and returns a ShaderAsset containing the OpenGL program handle.
     *
     * @param ctx Loading context with asset ID, config, and resource access
     *
     * @return Shared pointer to the loaded ShaderAsset
     *
     * @throws AssetLoadError if:
     *         - Required fields (vertex, fragment) are missing for graphics shaders
     *         - Required field (compute) is missing for compute shaders
     *         - Inherited parent shader is not found
     * @throws ShaderCompileError if GLSL compilation fails
     * @throws ShaderLinkError if program linking fails
     *
     * @section load_example Example Manifest Entry
     *
     * @code{.yaml}
     * shaders:
     *   lit_textured:
     *     inherit: shaders/base_lit
     *     fragment: shaders/lit_textured.frag
     *     defines:
     *       USE_TEXTURE: true
     *       TEXTURE_UNIT: 0
     * @endcode
     */
    std::shared_ptr<AssetBase> load(const LoadContext& ctx) override;

private:
    /**
     * @brief Resolve shader configuration including inheritance chain.
     *
     * Recursively processes the inheritance chain to produce a fully resolved
     * configuration. The algorithm:
     *
     * 1. Check for `inherit` field in config
     * 2. If inheriting, get parent's AssetEntry and recursively resolve it
     * 3. Start with parent's resolved config (or empty)
     * 4. Override with current shader's explicit values
     * 5. Merge defines (current shader's values override parent's)
     *
     * @param ctx Loading context for the shader being resolved
     *
     * @return Fully resolved configuration with all inheritance applied
     *
     * @throws AssetLoadError if an inherited parent is not found in manifest
     *
     * @warning Circular inheritance is not detected and will cause stack overflow.
     *          The manifest author must ensure no inheritance cycles exist.
     */
    ResolvedShaderConfig resolveConfig(const LoadContext& ctx);

    /**
     * @brief Extract preprocessor defines from a YAML configuration node.
     *
     * Reads the "defines" child node (if present) and adds all key-value pairs
     * to the provided map. If a key already exists, it is overwritten.
     *
     * @param node The YAML node to extract from (typically the asset's config)
     * @param[in,out] defines Map to add defines to; existing values may be overwritten
     *
     * Expected YAML structure:
     * @code{.yaml}
     * defines:
     *   KEY1: value1
     *   KEY2: value2
     * @endcode
     */
    void extractDefines(ryml::ConstNodeRef node,
                        std::unordered_map<std::string, std::string>& defines);

    /**
     * @brief Helper to extract a string value from a YAML config node.
     *
     * @param config The YAML node to read from
     * @param key The child key to look up
     *
     * @return The string value if the key exists, std::nullopt otherwise
     */
    static std::optional<std::string> getString(ryml::ConstNodeRef config, const char* key);
};

} // namespace Rigel::Asset
