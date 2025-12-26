#pragma once

/**
 * @file RawLoader.h
 * @brief Loader for raw binary data assets.
 *
 * This file provides the RawLoader class, which loads arbitrary binary files
 * as raw byte arrays. This is useful for configuration files, custom data
 * formats, or any file that doesn't need specialized processing.
 *
 * @section manifest_format Manifest Format
 *
 * Raw assets are defined in the manifest under the "raw" category:
 *
 * @code{.yaml}
 * assets:
 *   raw:
 *     # Configuration file
 *     config:
 *       path: config.yaml
 *
 *     # Custom binary format
 *     level_data:
 *       path: levels/level1.dat
 *
 *     # JSON data
 *     entity_defs:
 *       path: data/entities.json
 * @endcode
 *
 * @section usage Usage
 *
 * @code
 * // Load raw data
 * auto config = assetManager.get<RawAsset>("raw/config");
 *
 * // Access as bytes
 * const std::vector<char>& bytes = config->data;
 *
 * // Access as string view (for text files)
 * std::string_view text = config->str();
 *
 * // Parse YAML
 * ryml::Tree tree = ryml::parse_in_arena(text);
 *
 * // Parse JSON
 * nlohmann::json json = nlohmann::json::parse(text);
 * @endcode
 *
 * @section data_ownership Data Ownership
 *
 * Unlike LoadContext::loadResource() which returns a view into static data,
 * RawAsset owns a copy of the file data in its `data` vector. This means:
 * - The data can be safely stored and passed around
 * - Multiple RawAsset instances with the same path share data via Handle
 * - Modifications to the vector don't affect other assets
 *
 * @section use_cases Common Use Cases
 *
 * - **Configuration files**: YAML, JSON, INI, XML
 * - **Custom binary formats**: Level data, save files, packed resources
 * - **Script files**: Lua scripts, shader includes
 * - **Text resources**: Localization strings, markdown documentation
 *
 * @see RawAsset for the loaded data container
 */

#include "AssetLoader.h"

namespace Rigel::Asset {

/**
 * @brief Asset loader for raw binary data files.
 *
 * RawLoader implements the IAssetLoader interface to load arbitrary files
 * as raw byte arrays. It performs no processing on the data, simply copying
 * the file contents into a RawAsset.
 *
 * @section loader_lifecycle Loading Lifecycle
 *
 * When `AssetManager::get<RawAsset>("raw/myfile")` is called:
 *
 * 1. AssetManager calls `RawLoader::load()` with the asset's LoadContext
 * 2. RawLoader reads the "path" field from config
 * 3. Raw bytes are loaded from ResourceRegistry via `ctx.loadResource()`
 * 4. Data is copied into a RawAsset's vector
 * 5. Asset is cached and returned as Handle<RawAsset>
 *
 * @section error_handling Error Handling
 *
 * The loader throws exceptions on failure:
 * - **AssetLoadError**: Missing "path" field, resource not found
 *
 * @section registration Registration
 *
 * RawLoader is automatically registered with AssetManager when
 * `loadManifest()` is called. Manual registration is not required.
 *
 * @see RawAsset for the loaded data container
 */
class RawLoader : public IAssetLoader {
public:
    /**
     * @brief Get the manifest category this loader handles.
     * @return "raw"
     */
    std::string_view category() const override { return "raw"; }

    /**
     * @brief Load a raw binary asset from a file.
     *
     * Loads the file specified in the "path" config field and returns a
     * RawAsset containing a copy of the file data.
     *
     * @param ctx Loading context with asset ID, config, and resource access
     *
     * @return Shared pointer to the loaded RawAsset
     *
     * @throws AssetLoadError if:
     *         - The "path" field is missing from config
     *         - The resource path is not found in ResourceRegistry
     *
     * @section raw_load_example Example
     *
     * Manifest:
     * @code{.yaml}
     * raw:
     *   game_settings:
     *     path: config/settings.yaml
     * @endcode
     *
     * Loading:
     * @code
     * auto settings = assets.get<RawAsset>("raw/game_settings");
     * std::string_view yaml = settings->str();
     * // Parse yaml...
     * @endcode
     */
    std::shared_ptr<AssetBase> load(const LoadContext& ctx) override;
};

} // namespace Rigel::Asset
