#pragma once

/**
 * @file BlockLoader.h
 * @brief Loads block definitions from asset manifests.
 *
 * BlockLoader parses block entries from per-block YAML files in
 * "assets/blocks" and registers them with the BlockRegistry and TextureAtlas.
 * Each block file declares its own name or identifier.
 */

#include "BlockRegistry.h"
#include "TextureAtlas.h"
#include "Rigel/Util/Ryml.h"

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
 * solid: true
 * textures:
 *   all: textures/blocks/stone.png
 *
 * # assets/blocks/grass.yaml
 * id: grass
 * model: cube
 * opaque: true
 * solid: true
 * textures:
 *   top: textures/blocks/grass_top.png
 *   bottom: textures/blocks/dirt.png
 *   sides: textures/blocks/grass_side.png
 *
 * # assets/blocks/glass.yaml
 * id: glass
 * model: cube
 * opaque: false
 * solid: true
 * layer: transparent
 * textures:
 *   all: textures/blocks/glass.png
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
     * @brief Load all blocks from the embedded blocks directory.
     *
     * Scans embedded resources under "blocks/" for YAML files, parses their
     * configuration, registers them with the BlockRegistry, and loads their
     * textures into the TextureAtlas.
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
     * @param config The block config node to parse
     * @param ns The manifest namespace (e.g., "rigel")
     * @param atlas The texture atlas for texture loading
     *
     * @return Parsed BlockType
     */
    BlockType parseBlockType(
        const std::string& id,
        ryml::ConstNodeRef config,
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
