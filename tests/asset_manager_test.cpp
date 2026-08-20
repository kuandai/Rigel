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

TEST_CASE(AssetManager_ShaderEntriesHaveRequiredStages) {
    AssetManager assets;
    CHECK_NO_THROW(assets.loadManifest("manifest.yaml"));

    size_t shaderCount = 0;
    assets.forEachInCategory("shaders", [&](const std::string& name,
                                                const AssetManager::AssetEntry& entry) {
        const auto vertex = entry.getString("vertex");
        const auto fragment = entry.getString("fragment");
        if (!vertex || vertex->empty()) {
            throw Rigel::Test::TestFailure(
                "Shader entry '" + name + "' is missing its vertex source");
        }
        if (!fragment || fragment->empty()) {
            throw Rigel::Test::TestFailure(
                "Shader entry '" + name + "' is missing its fragment source");
        }
        ++shaderCount;
    });
    CHECK(shaderCount > 0);
}
