#pragma once

/**
 * @file BlockLoader.h
 * @brief Loads block definitions from asset manifests.
 *
 * BlockLoader parses block entries from the "blocks" category in the asset
 * manifest and registers them with the BlockRegistry and TextureAtlas.
 */

#include "BlockRegistry.h"
#include "TextureAtlas.h"

#include <Rigel/Asset/AssetManager.h>

#include <string>

namespace Rigel::Voxel {

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
 *   blocks:
 *     stone:
 *       model: cube
 *       opaque: true
 *       solid: true
 *       textures:
 *         all: textures/blocks/stone.png
 *
 *     grass:
 *       model: cube
 *       opaque: true
 *       solid: true
 *       textures:
 *         top: textures/blocks/grass_top.png
 *         bottom: textures/blocks/dirt.png
 *         sides: textures/blocks/grass_side.png
 *
 *     glass:
 *       model: cube
 *       opaque: false
 *       solid: true
 *       layer: transparent
 *       textures:
 *         all: textures/blocks/glass.png
 * @endcode
 *
 * @section usage Usage
 *
 * @code
 * BlockLoader loader;
 * loader.loadFromManifest(assets, registry, atlas);
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
     * @brief Load all blocks from the "blocks" category in the manifest.
     *
     * Iterates through all block entries in the manifest, parses their
     * configuration, registers them with the BlockRegistry, and loads
     * their textures into the TextureAtlas.
     *
     * @param assets The asset manager containing the manifest
     * @param registry The block registry to register types with
     * @param atlas The texture atlas to load textures into
     *
     * @return Number of blocks loaded
     */
    size_t loadFromManifest(
        Asset::AssetManager& assets,
        BlockRegistry& registry,
        TextureAtlas& atlas
    );

private:
    /**
     * @brief Parse a single block type from manifest config.
     *
     * @param id The block identifier (e.g., "stone")
     * @param entry The asset entry containing the config
     * @param ns The manifest namespace (e.g., "rigel")
     * @param atlas The texture atlas for texture loading
     *
     * @return Parsed BlockType
     */
    BlockType parseBlockType(
        const std::string& id,
        const Asset::AssetManager::AssetEntry& entry,
        const std::string& ns,
        TextureAtlas& atlas
    );

    /**
     * @brief Parse face textures from config.
     */
    FaceTextures parseTextures(
        ryml::ConstNodeRef config,
        TextureAtlas& atlas
    );

    /**
     * @brief Parse render layer from string.
     */
    RenderLayer parseRenderLayer(const std::string& str);
};

} // namespace Rigel::Voxel
