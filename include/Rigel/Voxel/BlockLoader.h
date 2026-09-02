#pragma once

/**
 * @file BlockLoader.h
 * @brief Loads block definitions from asset manifests.
 *
 * BlockLoader parses normalized `models/blocks/*.yaml` resources before
 * logical `blocks/*.yaml` resources and publishes the complete validated set.
 */

#include "BlockRegistry.h"
#include "TextureAtlas.h"
#include <Rigel/Asset/AssetManager.h>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Rigel::Voxel {

struct BlockLoadFailure {
    std::string definitionPath;
    std::string reason;
};

struct BlockLoadReport {
    size_t modelsDiscovered = 0;
    size_t modelsLoaded = 0;
    size_t modelsFailed = 0;
    size_t discovered = 0;
    size_t loaded = 0;
    size_t failed = 0;
    size_t skipped = 0;
    std::vector<BlockLoadFailure> representativeFailures;
};

struct BlockDefinitionSource {
    std::string_view path;
    std::span<const char> data;
};

struct BlockModelDefinitionSource {
    std::string_view path;
    std::span<const char> data;
};

/**
 * @brief Loads block definitions from asset manifests.
 *
 * Parses YAML block entries and registers block types with the registry
 * while loading textures into the atlas.
 *
 * @section manifest_format Manifest Format
 *
 * @code{.yaml}
 * assets:
 *   # Blocks are discovered from assets/blocks/*.yaml
 * @endcode
 *
 * @section block_file_format Block File Format
 *
 * @code{.yaml}
 * # assets/blocks/stone.yaml
 * id: stone
 * model: cube
 * opaque: true
 * collision: full
 * textures:
 *   all: textures/blocks/stone.png
 *
 * # assets/blocks/grass.yaml
 * id: grass
 * model: cube
 * opaque: true
 * collision: full
 * textures:
 *   top: textures/blocks/grass_top.png
 *   bottom: textures/blocks/dirt.png
 *   sides: textures/blocks/grass_side.png
 *
 * # assets/blocks/glass.yaml
 * id: glass
 * model: cube
 * orientation: [0, 90, 0]
 * opaque: false
 * collision: full
 * layer: transparent
 * textures:
 *   all: textures/blocks/glass.png
 *
 * # Named slots in normalized models may override the block render layer.
 * # texture_render_layers:
 * #   glass: transparent
 *
 * # Orientation is registration-local. Supported angle triples are identity,
 * # X 90/270, Y 90/180/270, and Z 90. rotate_top_bottom is an explicit UV
 * # correction accepted only for X 90 and Z 90 registrations.
 * # Collision is `none`, `full`, or a strict inline box list:
 * # collision:
 * #   boxes:
 * #     - [0.25, 0.0, 0.25, 0.75, 1.0, 0.75]
 * # Imported snapshots may also declare `collision_provenance` as `exact` or
 * # `conservative_fallback`; ordinary authored shapes omit it.
 * # Every normalized block must declare collision explicitly.
 * @endcode
 *
 * @section usage Usage
 *
 * @code
 * BlockLoader loader;
 * BlockModelRegistry models;
 * loader.loadFromManifest(assets, models, registry, atlas);
 *
 * // After loading, block IDs can be looked up by identifier
 * auto stoneId = registry.findByIdentifier("rigel:stone");
 * @endcode
 */
class BlockLoader {
public:
    BlockLoader() = default;
    ~BlockLoader() = default;

    /**
     * @brief Load all blocks from the embedded blocks directory.
     *
     * Scans embedded model, block, and texture resources, validates them as
     * one group, then publishes the immutable models and blocks atomically.
     *
     * @param assets The asset manager containing the manifest
     * @param registry The block registry to register types with
     * @param atlas The texture atlas to load textures into
     *
     * @return Counts and representative failures from the load
     */
    BlockLoadReport loadFromManifest(
        Asset::AssetManager& assets,
        BlockModelRegistry& models,
        BlockRegistry& registry,
        TextureAtlas& atlas
    );

    /**
     * @brief Parse an explicit set of normalized block definitions.
     *
     * The embedded-resource entry point delegates to this typed seam. Tests
     * can therefore use small invented definitions without depending on the
     * generated Cosmic Reach runtime asset tree.
     */
    BlockLoadReport loadDefinitions(
        std::string_view assetNamespace,
        std::span<const BlockModelDefinitionSource> modelDefinitions,
        std::span<const BlockDefinitionSource> definitions,
        BlockModelRegistry& models,
        BlockRegistry& registry,
        TextureAtlas& atlas
    );

    /** Compatibility seam for synthetic cube-only definitions. */
    BlockLoadReport loadDefinitions(
        std::string_view assetNamespace,
        std::span<const BlockDefinitionSource> definitions,
        BlockRegistry& registry,
        TextureAtlas& atlas
    );
};

} // namespace Rigel::Voxel
