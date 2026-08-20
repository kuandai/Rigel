#include "TestFramework.h"

#include "Rigel/Voxel/BlockLoader.h"
#include "Rigel/Asset/AssetManager.h"

#include <array>
#include <vector>

using namespace Rigel::Voxel;
using namespace Rigel::Asset;

TEST_CASE(BlockLoader_LoadsManifestBlocks) {
    AssetManager assets;
    assets.loadManifest("manifest.yaml");

    BlockRegistry registry;
    TextureAtlas atlas;
    const std::vector<unsigned char> pixels(
        static_cast<size_t>(atlas.tileSize() * atlas.tileSize() * 4),
        255
    );
    const std::array<const char*, 4> texturePaths = {
        "textures/blocks/dirt.png",
        "textures/blocks/glass.png",
        "textures/blocks/foliage/grass_top.png",
        "textures/blocks/foliage/grass_side.png"
    };
    for (const char* path : texturePaths) {
        atlas.addTexture(path, pixels.data());
    }

    BlockLoader loader;

    size_t loaded = loader.loadFromManifest(assets, registry, atlas);
    CHECK(loaded >= 4);
    CHECK(registry.hasIdentifier("base:dirt"));
    CHECK(registry.hasIdentifier("base:glass"));
    CHECK(registry.hasIdentifier("base:grass"));
    CHECK(registry.hasIdentifier("base:grass[type=full]"));

    const BlockType& dirt = registry.getType(*registry.findByIdentifier("base:dirt"));
    CHECK_EQ(dirt.textures.forFace(Direction::PosX), texturePaths[0]);
    CHECK_EQ(dirt.textures.forFace(Direction::NegY), texturePaths[0]);

    const BlockType& glass = registry.getType(*registry.findByIdentifier("base:glass"));
    CHECK(!glass.isOpaque);
    CHECK_EQ(glass.layer, RenderLayer::Transparent);
    CHECK_EQ(glass.textures.forFace(Direction::PosZ), texturePaths[1]);

    const BlockType& grass = registry.getType(*registry.findByIdentifier("base:grass"));
    CHECK_EQ(grass.textures.forFace(Direction::PosY), texturePaths[2]);
    CHECK_EQ(grass.textures.forFace(Direction::NegY), texturePaths[0]);
    CHECK_EQ(grass.textures.forFace(Direction::NegX), texturePaths[3]);

    const BlockType& fullGrass =
        registry.getType(*registry.findByIdentifier("base:grass[type=full]"));
    CHECK_EQ(fullGrass.textures.forFace(Direction::PosY), texturePaths[2]);
    CHECK_EQ(fullGrass.textures.forFace(Direction::NegY), texturePaths[2]);
}
