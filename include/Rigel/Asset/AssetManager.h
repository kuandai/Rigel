#pragma once

/**
 * @file AssetManager.h
 * @brief Central asset management system with manifest-based loading.
 *
 * This file provides the AssetManager class, which is the primary interface
 * for loading and accessing assets. It also defines the exception types used
 * throughout the asset system.
 *
 * @section overview Overview
 *
 * The AssetManager provides:
 * - **Manifest-based asset definitions**: Assets are declared in YAML files
 * - **Lazy loading**: Assets are loaded on first access
 * - **Caching**: Loaded assets are cached to prevent duplicate loading
 * - **Type-safe access**: Template-based API with Handle<T> return type
 * - **Extensible loaders**: Custom loaders can be registered for new asset types
 *
 * @section usage Basic Usage
 *
 * @code
 * // Create and initialize
 * AssetManager assets;
 * assets.loadManifest("manifest.yaml");
 *
 * // Load assets (loaded on first access, cached thereafter)
 * auto texture = assets.get<TextureAsset>("textures/stone");
 * auto shader = assets.get<ShaderAsset>("shaders/basic");
 * auto config = assets.get<RawAsset>("raw/settings");
 *
 * // Use the assets
 * texture->bind(GL_TEXTURE0);
 * shader->bind();
 * glUniform1i(shader->uniform("u_texture"), 0);
 * @endcode
 *
 * @section manifest Manifest Format
 *
 * The manifest is a YAML file that declares all available assets:
 *
 * @code{.yaml}
 * namespace: myproject
 *
 * assets:
 *   raw:
 *     config:
 *       path: config.yaml
 *
 *   textures:
 *     stone:
 *       path: textures/stone.png
 *       filter: nearest
 *
 *   shaders:
 *     basic:
 *       vertex: shaders/basic.vert
 *       fragment: shaders/basic.frag
 * @endcode
 *
 * @section exceptions Exception Hierarchy
 *
 * The asset system uses typed exceptions:
 *
 * - **AssetNotFoundError**: Asset ID not in manifest
 * - **AssetLoadError**: General loading failure
 *   - **ShaderCompileError**: GLSL compilation failure
 *   - **ShaderLinkError**: Program linking failure
 *
 * @section thread_safety Thread Safety
 *
 * The AssetManager is NOT thread-safe. All access must be from the same
 * thread (typically the main/render thread with the OpenGL context).
 *
 * @see Handle for type-safe asset references
 * @see IAssetLoader for implementing custom loaders
 */

#include "Handle.h"
#include "Types.h"
#include "AssetLoader.h"

#include <string>
#include <optional>
#include <unordered_map>
#include <memory>
#include <typeindex>
#include <stdexcept>
#include <functional>
#include <GL/glew.h>
#include <ryml.hpp>

namespace Rigel::Asset {

/**
 * @brief Exception thrown when an asset is not found in the manifest.
 *
 * This exception is thrown by AssetManager::get<T>() when the requested
 * asset ID does not exist in the loaded manifest.
 *
 * @section causes Common Causes
 *
 * - Typo in the asset ID
 * - Asset not declared in manifest
 * - Manifest not loaded (forgot to call loadManifest())
 * - Wrong category prefix (e.g., "texture/stone" vs "textures/stone")
 *
 * @section example Example
 *
 * @code
 * try {
 *     auto texture = assets.get<TextureAsset>("textures/missing");
 * } catch (const AssetNotFoundError& e) {
 *     spdlog::error("Asset '{}' not found in manifest", e.assetId());
 * }
 * @endcode
 */
class AssetNotFoundError : public std::runtime_error {
public:
    /**
     * @brief Construct with the missing asset ID.
     * @param id The asset ID that was not found
     */
    explicit AssetNotFoundError(const std::string& id)
        : std::runtime_error("Asset not found: " + id)
        , m_id(id)
    {}

    /**
     * @brief Get the asset ID that was not found.
     * @return The missing asset ID
     */
    const std::string& assetId() const { return m_id; }

private:
    std::string m_id;
};

/**
 * @brief Base exception for asset loading failures.
 *
 * This exception is thrown when an asset exists in the manifest but
 * cannot be loaded due to some error. Derived exceptions provide more
 * specific error information for particular failure modes.
 *
 * @section causes Common Causes
 *
 * - Missing "path" field for path-based assets
 * - Resource file not found in ResourceRegistry
 * - Invalid file format (corrupted image, invalid YAML, etc.)
 * - Type mismatch (requesting wrong asset type)
 *
 * @section example Example
 *
 * @code
 * try {
 *     auto texture = assets.get<TextureAsset>("textures/broken");
 * } catch (const AssetLoadError& e) {
 *     spdlog::error("Failed to load '{}': {}", e.assetId(), e.what());
 * }
 * @endcode
 *
 * @see ShaderCompileError, ShaderLinkError for shader-specific errors
 */
class AssetLoadError : public std::runtime_error {
public:
    /**
     * @brief Construct with asset ID and failure reason.
     * @param id The asset ID that failed to load
     * @param reason Human-readable description of the failure
     */
    AssetLoadError(const std::string& id, const std::string& reason)
        : std::runtime_error("Failed to load asset '" + id + "': " + reason)
        , m_id(id)
    {}

    /**
     * @brief Get the asset ID that failed to load.
     * @return The failing asset ID
     */
    const std::string& assetId() const { return m_id; }

private:
    std::string m_id;
};

/**
 * @brief Exception thrown when GLSL shader compilation fails.
 *
 * This exception is thrown by ShaderCompiler when a shader stage
 * (vertex, fragment, geometry, or compute) fails to compile. It includes
 * information about which stage failed and the OpenGL compiler log.
 *
 * @section error_log Error Log
 *
 * The log() method returns the OpenGL compiler output, which typically
 * contains line numbers and error descriptions. Example:
 *
 * @code
 * 0:15: error: 'undeclared_var' : undeclared identifier
 * 0:15: error: 'assign' : cannot convert from 'float' to 'int'
 * @endcode
 *
 * Note: Line numbers in the log are relative to the preprocessed source,
 * which may differ from the original file due to injected #defines.
 *
 * @section example Example
 *
 * @code
 * try {
 *     auto shader = assets.get<ShaderAsset>("shaders/broken");
 * } catch (const ShaderCompileError& e) {
 *     const char* stageName = (e.stage() == GL_VERTEX_SHADER) ? "vertex" :
 *                             (e.stage() == GL_FRAGMENT_SHADER) ? "fragment" :
 *                             "unknown";
 *     spdlog::error("Shader '{}' {} stage compilation failed:\n{}",
 *                   e.assetId(), stageName, e.log());
 * }
 * @endcode
 *
 * @see ShaderCompiler for compilation details
 */
class ShaderCompileError : public AssetLoadError {
public:
    /**
     * @brief Construct with shader ID, failing stage, and compiler log.
     * @param id The shader asset ID
     * @param stage The OpenGL stage that failed (GL_VERTEX_SHADER, etc.)
     * @param log The OpenGL compiler info log
     */
    ShaderCompileError(const std::string& id, GLenum stage, const std::string& log)
        : AssetLoadError(id, "Shader compilation failed")
        , m_stage(stage)
        , m_log(log)
    {}

    /**
     * @brief Get the shader stage that failed to compile.
     * @return OpenGL shader type constant (GL_VERTEX_SHADER, GL_FRAGMENT_SHADER,
     *         GL_GEOMETRY_SHADER, or GL_COMPUTE_SHADER)
     */
    GLenum stage() const { return m_stage; }

    /**
     * @brief Get the OpenGL compiler error log.
     * @return The compiler info log with error messages and line numbers
     */
    const std::string& log() const { return m_log; }

private:
    GLenum m_stage;
    std::string m_log;
};

/**
 * @brief Exception thrown when shader program linking fails.
 *
 * This exception is thrown by ShaderCompiler when shader stages compile
 * successfully but the program fails to link. Common causes include
 * mismatched varying/in/out declarations between stages.
 *
 * @section error_log Error Log
 *
 * The log() method returns the OpenGL linker output. Example:
 *
 * @code
 * error: varying 'v_texcoord' not found in vertex shader output
 * @endcode
 *
 * @section example Example
 *
 * @code
 * try {
 *     auto shader = assets.get<ShaderAsset>("shaders/mismatched");
 * } catch (const ShaderLinkError& e) {
 *     spdlog::error("Shader '{}' link failed:\n{}", e.assetId(), e.log());
 * }
 * @endcode
 *
 * @see ShaderCompiler for compilation details
 */
class ShaderLinkError : public AssetLoadError {
public:
    /**
     * @brief Construct with shader ID and linker log.
     * @param id The shader asset ID
     * @param log The OpenGL linker info log
     */
    ShaderLinkError(const std::string& id, const std::string& log)
        : AssetLoadError(id, "Shader linking failed")
        , m_log(log)
    {}

    /**
     * @brief Get the OpenGL linker error log.
     * @return The linker info log with error messages
     */
    const std::string& log() const { return m_log; }

private:
    std::string m_log;
};

/**
 * @brief Central registry and loader for game assets.
 *
 * AssetManager is the main interface for the asset system. It loads asset
 * definitions from YAML manifest files and provides type-safe, cached access
 * to loaded assets.
 *
 * @section lifecycle Lifecycle
 *
 * @code
 * // 1. Create manager
 * AssetManager assets;
 *
 * // 2. Load manifest (registers built-in loaders automatically)
 * assets.loadManifest("manifest.yaml");
 *
 * // 3. Access assets (loaded on first request, cached thereafter)
 * auto tex = assets.get<TextureAsset>("textures/stone");
 *
 * // 4. Optional: clear cache to release memory
 * assets.clearCache();
 * @endcode
 *
 * @section caching Caching Behavior
 *
 * - Assets are loaded lazily on first get<T>() call
 * - Loaded assets are cached and reused for subsequent requests
 * - Cache is keyed by both type and ID (same ID can have different types)
 * - clearCache() releases all cached assets (may trigger OpenGL cleanup)
 *
 * @section loaders Loader System
 *
 * Built-in loaders are registered automatically:
 * - "raw" → RawLoader
 * - "textures" → TextureLoader
 * - "shaders" → ShaderLoader
 *
 * Custom loaders can be registered via registerLoader():
 * @code
 * assets.registerLoader("models", std::make_unique<ModelLoader>());
 * @endcode
 *
 * @section error_handling Error Handling
 *
 * All errors are reported via exceptions:
 * - AssetNotFoundError: ID not in manifest
 * - AssetLoadError: Loading failed (base class)
 * - ShaderCompileError: GLSL compilation failed
 * - ShaderLinkError: Program linking failed
 *
 * @note Requires a valid OpenGL context for texture and shader loading.
 *
 * @see IAssetLoader for implementing custom loaders
 * @see Handle for type-safe asset references
 */
class AssetManager {
public:
    /**
     * @brief Default constructor. Creates an empty asset manager.
     *
     * Call loadManifest() to load asset definitions before using get<T>().
     */
    AssetManager() = default;

    /**
     * @brief Destructor. Releases all cached assets.
     *
     * @note If assets own OpenGL resources, a valid context must be current.
     */
    ~AssetManager() = default;

    /// @name Non-copyable
    /// AssetManager owns unique loaders and cannot be copied.
    /// @{
    AssetManager(const AssetManager&) = delete;
    AssetManager& operator=(const AssetManager&) = delete;
    /// @}

    /**
     * @brief Load asset definitions from a YAML manifest file.
     *
     * Parses the manifest file and registers all declared assets. Also
     * registers the built-in loaders (raw, textures, shaders) if not
     * already registered.
     *
     * @param path Path to manifest file (relative to embedded resources)
     *
     * @throws std::runtime_error if manifest file cannot be found or parsed
     *
     * @section manifest_structure Manifest Structure
     *
     * @code{.yaml}
     * namespace: myproject  # Optional namespace prefix
     *
     * assets:
     *   raw:               # Category name
     *     config:          # Asset name → ID becomes "raw/config"
     *       path: config.yaml
     *
     *   textures:
     *     stone:           # ID: "textures/stone"
     *       path: textures/stone.png
     *       filter: linear
     * @endcode
     *
     * @note Multiple calls to loadManifest() will merge assets. Later
     *       declarations override earlier ones with the same ID.
     */
    void loadManifest(const std::string& path);

    /**
     * @brief Get an asset by its identifier.
     *
     * Loads the asset on first access and caches it for subsequent requests.
     * Returns a Handle<T> which provides pointer-like access to the asset.
     *
     * @tparam T The asset type to load (RawAsset, TextureAsset, ShaderAsset, etc.)
     * @param id Asset identifier in "category/name" format
     *
     * @return Handle to the loaded asset
     *
     * @throws AssetNotFoundError if asset is not declared in manifest
     * @throws AssetLoadError if asset fails to load
     * @throws ShaderCompileError if shader compilation fails
     * @throws ShaderLinkError if shader linking fails
     *
     * @code
     * // First call loads and caches
     * auto tex1 = assets.get<TextureAsset>("textures/stone");
     *
     * // Second call returns cached asset
     * auto tex2 = assets.get<TextureAsset>("textures/stone");
     *
     * // tex1 and tex2 point to the same underlying asset
     * assert(tex1.id() == tex2.id());
     * @endcode
     */
    template<typename T>
    Handle<T> get(const std::string& id);

    /**
     * @brief Check if an asset exists in the manifest.
     *
     * Does not load the asset, only checks if it's declared.
     *
     * @param id Asset identifier to check
     * @return true if asset is declared in manifest, false otherwise
     */
    bool exists(const std::string& id) const;

    /**
     * @brief Get the manifest namespace.
     *
     * Returns the namespace declared in the manifest, or empty string if
     * no namespace was specified.
     *
     * @return The manifest namespace
     */
    const std::string& ns() const { return m_namespace; }

    /**
     * @brief Clear all cached assets.
     *
     * Releases all loaded assets from the cache. Does not unload the manifest;
     * assets can still be loaded again via get<T>().
     *
     * @note If cached assets own OpenGL resources, a valid context must be
     *       current when calling this method.
     */
    void clearCache();

    /**
     * @brief Register a custom loader for an asset category.
     *
     * Registers a loader to handle assets in a specific manifest category.
     * If a loader is already registered for the category, it is replaced.
     *
     * @param category The manifest category (e.g., "models", "sounds")
     * @param loader The loader instance (ownership transferred)
     *
     * @code
     * class ModelLoader : public IAssetLoader {
     *     std::string_view category() const override { return "models"; }
     *     std::shared_ptr<AssetBase> load(const LoadContext& ctx) override;
     * };
     *
     * assets.registerLoader("models", std::make_unique<ModelLoader>());
     * @endcode
     */
    void registerLoader(const std::string& category, std::unique_ptr<IAssetLoader> loader);

    /**
     * @brief Internal structure representing an asset entry in the manifest.
     *
     * Each declared asset in the manifest has an associated AssetEntry that
     * stores its category and full YAML configuration. This configuration
     * is passed to loaders during asset loading.
     *
     * @note This is primarily used internally and by loaders implementing
     *       features like shader inheritance.
     */
    struct AssetEntry {
        /**
         * @brief The asset category (e.g., "textures", "shaders").
         */
        std::string category;

        /**
         * @brief Owned YAML tree containing this asset's configuration.
         *
         * This tree is cloned from the manifest to ensure the configuration
         * remains valid after manifest parsing completes.
         */
        ryml::Tree configTree;

        /**
         * @brief Reference to the root of configTree for convenient access.
         */
        ryml::ConstNodeRef config;

        /**
         * @brief Get a string value from the configuration.
         * @param key The configuration key to look up
         * @return The string value if key exists, std::nullopt otherwise
         */
        std::optional<std::string> getString(const std::string& key) const;

        /**
         * @brief Check if a configuration key exists.
         * @param key The key to check for
         * @return true if key exists, false otherwise
         */
        bool hasChild(const std::string& key) const;
    };

    /**
     * @brief Get an asset's manifest entry.
     *
     * Returns the raw manifest entry for an asset, including its full YAML
     * configuration. This is used by loaders to implement features like
     * shader inheritance.
     *
     * @param id Asset identifier
     * @return Pointer to entry if found, nullptr otherwise
     *
     * @note The returned pointer is valid until the next loadManifest() call.
     */
    const AssetEntry* getEntry(const std::string& id) const;

    /**
     * @brief Iterate over all entries in a specific category.
     *
     * Calls the provided function for each asset entry in the given category.
     * This is useful for systems that need to discover all assets of a type
     * (e.g., loading all block definitions).
     *
     * @param category The category to iterate (e.g., "blocks", "textures")
     * @param fn Function called for each entry: fn(assetName, entry)
     *           - assetName is the name without category prefix (e.g., "stone")
     *           - entry is the full AssetEntry with configuration
     *
     * @code
     * assets.forEachInCategory("blocks", [&](const std::string& name, const AssetEntry& entry) {
     *     spdlog::info("Found block: {}", name);
     * });
     * @endcode
     */
    void forEachInCategory(
        const std::string& category,
        const std::function<void(const std::string& name, const AssetEntry& entry)>& fn
    ) const;

private:
    /**
     * @brief Legacy fallback loader for raw assets.
     * @deprecated Use RawLoader instead.
     */
    std::shared_ptr<RawAsset> loadRawAsset(const std::string& id, const AssetEntry& entry);

    /**
     * @brief Legacy fallback loader for texture assets.
     * @deprecated Use TextureLoader instead.
     */
    std::shared_ptr<TextureAsset> loadTextureAsset(const std::string& id, const AssetEntry& entry);

    std::string m_namespace;
    std::unordered_map<std::string, AssetEntry> m_entries;
    std::unordered_map<std::string, std::unique_ptr<IAssetLoader>> m_loaders;

    // Type-erased cache: maps (type_index, id) -> shared_ptr<void>
    using CacheKey = std::pair<std::type_index, std::string>;
    struct CacheKeyHash {
        size_t operator()(const CacheKey& k) const {
            return std::hash<std::type_index>{}(k.first) ^
                   (std::hash<std::string>{}(k.second) << 1);
        }
    };
    std::unordered_map<CacheKey, std::shared_ptr<void>, CacheKeyHash> m_cache;
};

// Template implementation
template<typename T>
Handle<T> AssetManager::get(const std::string& id) {
    // Check cache first
    CacheKey key{std::type_index(typeid(T)), id};
    auto cacheIt = m_cache.find(key);
    if (cacheIt != m_cache.end()) {
        return Handle<T>(std::static_pointer_cast<T>(cacheIt->second), id);
    }

    // Find entry in manifest
    auto entryIt = m_entries.find(id);
    if (entryIt == m_entries.end()) {
        throw AssetNotFoundError(id);
    }

    const auto& entry = entryIt->second;
    std::shared_ptr<T> asset;

    // Check if there's a registered loader for this category
    auto loaderIt = m_loaders.find(entry.category);
    if (loaderIt != m_loaders.end()) {
        // Use registered loader
        LoadContext ctx{id, entry.config, *this};
        auto baseAsset = loaderIt->second->load(ctx);
        asset = std::dynamic_pointer_cast<T>(baseAsset);
        if (!asset) {
            throw AssetLoadError(id, "Loader returned incompatible asset type");
        }
    } else {
        // Fall back to built-in loading for backwards compatibility
        if constexpr (std::is_same_v<T, RawAsset>) {
            asset = loadRawAsset(id, entry);
        } else if constexpr (std::is_same_v<T, TextureAsset>) {
            asset = loadTextureAsset(id, entry);
        } else {
            throw AssetLoadError(id, "Unsupported asset type and no loader registered");
        }
    }

    // Cache and return
    m_cache[key] = asset;
    return Handle<T>(asset, id);
}

} // namespace Rigel::Asset
