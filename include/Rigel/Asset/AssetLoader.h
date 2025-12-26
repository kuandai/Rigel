#pragma once

/**
 * @file AssetLoader.h
 * @brief Core interfaces and base types for the asset loading system.
 *
 * This file defines the foundational types for implementing custom asset loaders:
 * - AssetBase: Base class that all loadable asset types must inherit from
 * - LoadContext: Runtime context provided to loaders during asset loading
 * - IAssetLoader: Interface that custom loaders must implement
 *
 * The asset loading system follows a plugin-based architecture where each asset
 * category (textures, shaders, raw data, etc.) has a dedicated loader registered
 * with the AssetManager. This design allows for:
 * - Easy extension with new asset types
 * - Category-specific loading logic and optimization
 * - Complex asset configurations (e.g., shader inheritance)
 *
 * @section usage_example Usage Example
 *
 * Creating a custom loader:
 * @code
 * class MyCustomLoader : public IAssetLoader {
 * public:
 *     std::string_view category() const override { return "custom"; }
 *
 *     std::shared_ptr<AssetBase> load(const LoadContext& ctx) override {
 *         // Read configuration from ctx.config
 *         std::string path;
 *         ctx.config["path"] >> path;
 *
 *         // Load raw data
 *         auto data = ctx.loadResource(path);
 *
 *         // Process and return asset
 *         auto asset = std::make_shared<MyCustomAsset>();
 *         // ... populate asset ...
 *         return asset;
 *     }
 * };
 *
 * // Register with AssetManager
 * assetManager.registerLoader("custom", std::make_unique<MyCustomLoader>());
 * @endcode
 *
 * @see AssetManager for the central registry and loading API
 * @see Handle for type-safe asset references
 */

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <ryml.hpp>

namespace Rigel::Asset {

class AssetManager;  // Forward declaration

/**
 * @brief Abstract base class for all loadable asset types.
 *
 * All asset types that can be loaded through the AssetManager must inherit
 * from this base class. This enables type-erased storage in the asset cache
 * and polymorphic loading through the IAssetLoader interface.
 *
 * Derived classes should:
 * - Implement RAII for any resources they own (OpenGL objects, file handles, etc.)
 * - Be move-constructible and move-assignable (for efficient cache management)
 * - Generally be non-copyable if they own unique resources
 *
 * @section builtin_assets Built-in Asset Types
 *
 * The following asset types are provided by default:
 * - RawAsset: Binary data loaded directly from files
 * - TextureAsset: OpenGL textures loaded from image files
 * - ShaderAsset: Compiled OpenGL shader programs
 *
 * @section custom_assets Creating Custom Asset Types
 *
 * @code
 * struct MyAsset : AssetBase {
 *     // Your asset data
 *     std::vector<float> vertices;
 *     std::vector<uint32_t> indices;
 *
 *     // Resource handles with RAII cleanup
 *     GLuint vao = 0;
 *     GLuint vbo = 0;
 *
 *     ~MyAsset() override {
 *         if (vao) glDeleteVertexArrays(1, &vao);
 *         if (vbo) glDeleteBuffers(1, &vbo);
 *     }
 * };
 * @endcode
 *
 * @note Assets are reference-counted via std::shared_ptr. Multiple Handle<T>
 *       instances can reference the same underlying asset.
 */
struct AssetBase {
    /**
     * @brief Virtual destructor for proper polymorphic cleanup.
     *
     * Derived classes should release any owned resources in their destructors.
     * This destructor is called when the last shared_ptr reference is released.
     */
    virtual ~AssetBase() = default;
};

/**
 * @brief Context provided to asset loaders during the loading process.
 *
 * LoadContext encapsulates all information and utilities needed to load an asset:
 * - The asset's unique identifier
 * - The asset's YAML configuration from the manifest
 * - Access to the AssetManager for loading dependencies
 * - Resource loading utilities
 *
 * This context is created by the AssetManager and passed to the appropriate
 * IAssetLoader when an asset is first requested.
 *
 * @section config_access Accessing Configuration
 *
 * The config member is a rapidyaml node containing this asset's manifest entry:
 *
 * @code
 * // For manifest entry:
 * // shaders:
 * //   my_shader:
 * //     vertex: shaders/basic.vert
 * //     fragment: shaders/basic.frag
 * //     defines:
 * //       USE_LIGHTING: true
 *
 * std::string vertex, fragment;
 * ctx.config["vertex"] >> vertex;
 * ctx.config["fragment"] >> fragment;
 *
 * if (ctx.config.has_child("defines")) {
 *     for (auto define : ctx.config["defines"].children()) {
 *         std::string key(define.key().data(), define.key().size());
 *         std::string value;
 *         define >> value;
 *     }
 * }
 * @endcode
 *
 * @section dependency_loading Loading Dependencies
 *
 * Use the manager reference to load assets that this asset depends on:
 *
 * @code
 * // Load a texture this material depends on
 * auto diffuseTexture = ctx.manager.get<TextureAsset>("textures/diffuse");
 * @endcode
 *
 * @warning Be careful to avoid circular dependencies. The AssetManager does
 *          not currently detect cycles, which would cause infinite recursion.
 */
struct LoadContext {
    /**
     * @brief The unique identifier of the asset being loaded.
     *
     * Format is "category/name" (e.g., "shaders/basic", "textures/stone").
     * This ID was used in the AssetManager::get<T>() call that triggered loading.
     */
    const std::string& id;

    /**
     * @brief The YAML configuration node for this asset from the manifest.
     *
     * This node contains all properties defined for this asset in the manifest.
     * Use rapidyaml API to extract values:
     *
     * @code
     * std::string path;
     * if (config.has_child("path")) {
     *     config["path"] >> path;
     * }
     * @endcode
     *
     * @note The node remains valid for the duration of the load() call.
     *       Do not store references to it beyond that scope.
     */
    ryml::ConstNodeRef config;

    /**
     * @brief Reference to the AssetManager for loading dependencies.
     *
     * Use this to load other assets that the current asset depends on:
     *
     * @code
     * auto baseTexture = ctx.manager.get<TextureAsset>("textures/base");
     * @endcode
     *
     * This enables complex asset relationships while maintaining proper
     * caching and reference counting.
     */
    AssetManager& manager;

    /**
     * @brief Load raw binary data from the embedded resource registry.
     *
     * This method retrieves file data from the ResourceRegistry, which contains
     * all files embedded into the executable from the assets/ directory.
     *
     * @param path The resource path (e.g., "shaders/basic.vert", "textures/stone.png")
     * @return A span view over the raw file bytes. The data remains valid for
     *         the lifetime of the application (static storage).
     *
     * @throws std::runtime_error if the resource path is not found in the registry
     *
     * @code
     * auto shaderSource = ctx.loadResource("shaders/basic.vert");
     * std::string sourceCode(shaderSource.data(), shaderSource.size());
     * @endcode
     *
     * @note The returned span is a view into static data. Do not attempt to
     *       modify the data or use it after the application exits.
     */
    std::span<const char> loadResource(const std::string& path) const;
};

/**
 * @brief Interface for category-specific asset loaders.
 *
 * Implement this interface to add support for new asset categories. Each loader
 * is responsible for:
 * - Declaring which manifest category it handles
 * - Parsing category-specific configuration
 * - Loading and processing the asset data
 * - Returning a properly initialized asset object
 *
 * @section loader_registration Registration
 *
 * Loaders are registered with the AssetManager by category name:
 *
 * @code
 * assetManager.registerLoader("models", std::make_unique<ModelLoader>());
 * @endcode
 *
 * Built-in loaders for "raw", "textures", and "shaders" are registered
 * automatically when loadManifest() is called.
 *
 * @section loader_lifecycle Lifecycle
 *
 * 1. User calls `assetManager.get<T>("category/name")`
 * 2. AssetManager finds the loader registered for "category"
 * 3. AssetManager creates a LoadContext with the asset's config
 * 4. AssetManager calls `loader->load(ctx)`
 * 5. Loader processes data and returns shared_ptr<AssetBase>
 * 6. AssetManager caches the result and returns Handle<T>
 *
 * @section error_handling Error Handling
 *
 * Loaders should throw exceptions on failure:
 * - AssetLoadError for general loading failures
 * - ShaderCompileError for shader compilation failures
 * - ShaderLinkError for shader linking failures
 *
 * @code
 * if (!data.valid()) {
 *     throw AssetLoadError(ctx.id, "Failed to parse model data");
 * }
 * @endcode
 *
 * @see RawLoader, TextureLoader, ShaderLoader for implementation examples
 */
class IAssetLoader {
public:
    /**
     * @brief Virtual destructor for proper cleanup of derived loaders.
     */
    virtual ~IAssetLoader() = default;

    /**
     * @brief Get the manifest category this loader handles.
     *
     * This string must match the top-level key under "assets" in the manifest
     * that this loader should process.
     *
     * @return Category name (e.g., "textures", "shaders", "raw", "models")
     *
     * Example manifest structure:
     * @code{.yaml}
     * assets:
     *   textures:    # <- category() returns "textures"
     *     stone:
     *       path: textures/stone.png
     *   shaders:     # <- category() returns "shaders"
     *     basic:
     *       vertex: shaders/basic.vert
     *       fragment: shaders/basic.frag
     * @endcode
     */
    virtual std::string_view category() const = 0;

    /**
     * @brief Load an asset using the provided context.
     *
     * Implement this method to perform the actual asset loading. The method
     * receives full context including configuration, resource access, and
     * the ability to load dependencies.
     *
     * @param ctx The loading context containing configuration and utilities
     * @return A shared pointer to the loaded asset, cast to AssetBase
     *
     * @throws AssetLoadError if loading fails for any reason
     * @throws ShaderCompileError if shader compilation fails (shader-specific)
     * @throws ShaderLinkError if shader linking fails (shader-specific)
     *
     * @section implementation_notes Implementation Notes
     *
     * - Always validate configuration before using it
     * - Use ctx.loadResource() to load file data from embedded resources
     * - Use ctx.manager.get<T>() to load asset dependencies
     * - Return nullptr is not valid; throw an exception on failure
     * - The returned asset must inherit from AssetBase
     *
     * @code
     * std::shared_ptr<AssetBase> MyLoader::load(const LoadContext& ctx) {
     *     // 1. Read configuration
     *     std::string path;
     *     if (!ctx.config.has_child("path")) {
     *         throw AssetLoadError(ctx.id, "Missing 'path' field");
     *     }
     *     ctx.config["path"] >> path;
     *
     *     // 2. Load raw data
     *     auto data = ctx.loadResource(path);
     *
     *     // 3. Process data
     *     auto asset = std::make_shared<MyAsset>();
     *     if (!parseData(data, *asset)) {
     *         throw AssetLoadError(ctx.id, "Failed to parse data");
     *     }
     *
     *     // 4. Return as base type
     *     return asset;
     * }
     * @endcode
     */
    virtual std::shared_ptr<AssetBase> load(const LoadContext& ctx) = 0;
};

} // namespace Rigel::Asset
