#include "TestFramework.h"

#include "Rigel/Asset/AssetManager.h"

using namespace Rigel::Asset;

TEST_CASE(AssetManager_LoadsEmbeddedManifest) {
    AssetManager assets;
    CHECK_NO_THROW(assets.loadManifest("manifest.yaml"));

    CHECK(assets.exists("raw/world_config"));
    CHECK(assets.exists("shaders/voxel"));
    CHECK(!assets.exists("blocks/dirt"));
    CHECK(assets.exists("entity_models/demo_cube"));
    CHECK(assets.exists("entity_anims/demo_spin"));
}

TEST_CASE(AssetManager_ShaderEntriesHaveFragmentSources) {
    AssetManager assets;
    CHECK_NO_THROW(assets.loadManifest("manifest.yaml"));

    const auto* voxelShadow = assets.getEntry("shaders/voxel_shadow_depth");
    CHECK(voxelShadow);
    CHECK(voxelShadow->hasChild("vertex"));
    CHECK(voxelShadow->hasChild("fragment"));
    auto voxelFrag = voxelShadow->getString("fragment");
    CHECK(voxelFrag);
    CHECK(!voxelFrag->empty());

    const auto* entityShadow = assets.getEntry("shaders/entity_shadow_depth");
    CHECK(entityShadow);
    CHECK(entityShadow->hasChild("vertex"));
    CHECK(entityShadow->hasChild("fragment"));
    auto entityFrag = entityShadow->getString("fragment");
    CHECK(entityFrag);
    CHECK(!entityFrag->empty());
}
