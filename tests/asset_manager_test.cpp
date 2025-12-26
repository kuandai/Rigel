#include "TestFramework.h"

#include "Rigel/Asset/AssetManager.h"

using namespace Rigel::Asset;

TEST_CASE(AssetManager_LoadsEmbeddedManifest) {
    AssetManager assets;
    CHECK_NO_THROW(assets.loadManifest("manifest.yaml"));

    CHECK(assets.exists("raw/world_config"));
    CHECK(assets.exists("shaders/voxel"));
    CHECK(assets.exists("blocks/stone"));
}
