#include "TestFramework.h"

#include "Rigel/Asset/AssetManager.h"

TEST_CASE(AssetManager_RejectsDuplicateGeneratorDeclarationFields) {
    Rigel::Asset::AssetManager assets;
    CHECK_THROWS(assets.loadManifest("duplicate_generator_field.yaml"));
    CHECK(!assets.exists("generator_definitions/default"));
}

TEST_CASE(AssetManager_RejectsDuplicateGeneratorDeclarationNames) {
    Rigel::Asset::AssetManager assets;
    CHECK_THROWS(assets.loadManifest("duplicate_generator_name.yaml"));
    CHECK(!assets.exists("generator_definitions/default"));
}
