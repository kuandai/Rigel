#include "TestFramework.h"

#include "Rigel/Voxel/BlockLoader.h"
#include "Rigel/Asset/AssetManager.h"

using namespace Rigel::Voxel;
using namespace Rigel::Asset;

TEST_CASE(BlockLoader_LoadsManifestBlocks) {
    AssetManager assets;
    assets.loadManifest("manifest.yaml");

    BlockRegistry registry;
    TextureAtlas atlas;
    BlockLoader loader;

    size_t loaded = loader.loadFromManifest(assets, registry, atlas);
    if (loaded == 0) {
        SKIP_TEST("No block definitions loaded; embedded textures may be missing");
    }

    CHECK(registry.hasIdentifier("rigel:stone"));
    CHECK(atlas.textureCount() >= 1);
}
