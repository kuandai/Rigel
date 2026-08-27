#include "TestFramework.h"

#include "Rigel/Asset/AssetLoader.h"
#include "Rigel/Asset/AssetManager.h"
#include "Rigel/Asset/RawLoader.h"
#include "Rigel/Voxel/BlockRegistry.h"
#include "Rigel/Voxel/GeneratorDefinitionLoader.h"

TEST_CASE(AssetManager_RejectsDuplicateGeneratorDeclarationFields) {
    Rigel::Asset::AssetManager assets;
    CHECK_NO_THROW(assets.loadManifest("duplicate_generator_field.yaml"));
    CHECK(!assets.exists("generator_definitions/default"));
    CHECK_EQ(assets.ns(), std::string("test"));
    CHECK(assets.categoryDeclarationError("generator_definitions").has_value());
}

TEST_CASE(AssetManager_RejectsDuplicateGeneratorDeclarationNames) {
    Rigel::Asset::AssetManager assets;
    CHECK_NO_THROW(assets.loadManifest("duplicate_generator_name.yaml"));
    CHECK(!assets.exists("generator_definitions/default"));
    CHECK_EQ(assets.ns(), std::string("test"));
    CHECK(assets.categoryDeclarationError("generator_definitions").has_value());
}

TEST_CASE(GeneratorDefinitionLoader_missing_resource_is_atomic_asset_failure) {
    Rigel::Asset::AssetManager assets;
    assets.loadManifest("aggregate_missing_resource.yaml");
    const auto retained = assets.get<Rigel::Asset::RawAsset>("raw/stable");
    Rigel::Voxel::BlockRegistry registry;

    bool assetBoundary = false;
    try {
        static_cast<void>(Rigel::Voxel::loadDeclaredGeneratorDefinitions(
            assets,
            registry,
            Rigel::Voxel::GeneratorDefinitionOrigin::Shipped));
    } catch (const Rigel::Asset::AssetLoadError&) {
        assetBoundary = true;
    }
    CHECK(assetBoundary);
    CHECK_EQ(assets.get<Rigel::Asset::RawAsset>("raw/stable"), retained);

    // Aggregate resolution never publishes the successfully parsed prefix.
    CHECK_THROWS(assets.get<Rigel::Asset::AssetBase>(
        "generator_definitions/a_valid"));
}

TEST_CASE(GeneratorDefinitionLoader_malformed_path_is_asset_failure) {
    Rigel::Asset::AssetManager assets;
    assets.loadManifest("malformed_generator_declaration.yaml");
    Rigel::Voxel::BlockRegistry registry;

    bool assetBoundary = false;
    try {
        static_cast<void>(Rigel::Voxel::loadDeclaredGeneratorDefinitions(
            assets,
            registry,
            Rigel::Voxel::GeneratorDefinitionOrigin::Shipped));
    } catch (const Rigel::Asset::AssetLoadError&) {
        assetBoundary = true;
    }
    CHECK(assetBoundary);
}
