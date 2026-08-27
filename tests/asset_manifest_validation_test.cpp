#include "TestFramework.h"

#include "Rigel/Asset/AssetLoader.h"
#include "Rigel/Asset/AssetManager.h"
#include "Rigel/Asset/RawLoader.h"
#include "Rigel/Voxel/BlockRegistry.h"
#include "Rigel/Voxel/BlockType.h"
#include "Rigel/Voxel/GeneratorDefinitionLoader.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace {

using Rigel::Voxel::GeneratorDefinitionOrigin;

void registerDefinitionMaterials(Rigel::Voxel::BlockRegistry& registry) {
    for (const std::string& id : {
             "test:stone", "test:water", "test:surface"}) {
        Rigel::Voxel::BlockType block;
        block.identifier = id;
        registry.registerBlock(id, std::move(block));
    }
}

std::vector<std::string> generatorDeclarationNames(
    const Rigel::Asset::AssetManager& assets) {
    std::vector<std::string> names;
    assets.forEachInCategory(
        "generator_definitions",
        [&](const std::string& name,
            const Rigel::Asset::AssetManager::AssetEntry&) {
            names.push_back(name);
        });
    std::sort(names.begin(), names.end());
    return names;
}

bool loadFailsAtAssetBoundary(
    Rigel::Asset::AssetManager& assets,
    const Rigel::Voxel::BlockRegistry& registry) {
    try {
        static_cast<void>(
            Rigel::Voxel::loadPreparedGeneratorDefinitionSnapshot(
                assets, registry, "test:valid",
                GeneratorDefinitionOrigin::Shipped));
    } catch (const Rigel::Asset::AssetLoadError&) {
        return true;
    }
    return false;
}

void commitInitialGeneratorSet(
    Rigel::Asset::AssetManager& assets,
    const Rigel::Voxel::BlockRegistry& registry) {
    assets.loadManifest("initial.yaml");
    const auto prepared =
        Rigel::Voxel::loadPreparedGeneratorDefinitionSnapshot(
            assets, registry, "test:valid",
            GeneratorDefinitionOrigin::Shipped);
    CHECK_EQ(prepared.sourceId, std::string("test:valid"));
    CHECK_EQ(assets.ns(), std::string("initial"));
    CHECK(assets.exists("generator_definitions/stable"));
    CHECK_EQ(generatorDeclarationNames(assets),
             std::vector<std::string>{"stable"});
}

} // namespace

TEST_CASE(AssetManager_RejectsDuplicateGeneratorDeclarationFields) {
    Rigel::Asset::AssetManager assets;
    CHECK_THROWS(assets.loadManifest("duplicate_generator_field.yaml"));
    CHECK(!assets.exists("generator_definitions/default"));
    CHECK(assets.ns().empty());
    CHECK(generatorDeclarationNames(assets).empty());
}

TEST_CASE(AssetManager_RejectsDuplicateGeneratorDeclarationNames) {
    Rigel::Asset::AssetManager assets;
    CHECK_THROWS(assets.loadManifest("duplicate_generator_name.yaml"));
    CHECK(!assets.exists("generator_definitions/default"));
    CHECK(assets.ns().empty());
    CHECK(generatorDeclarationNames(assets).empty());
}

TEST_CASE(GeneratorDefinitionLoader_manifest_failure_preserves_prior_complete_set) {
    Rigel::Asset::AssetManager assets;
    Rigel::Voxel::BlockRegistry registry;
    registerDefinitionMaterials(registry);
    commitInitialGeneratorSet(assets, registry);
    const auto retained = assets.get<Rigel::Asset::RawAsset>("raw/stable");

    bool duplicateBoundary = false;
    try {
        assets.loadManifest("duplicate_generator_name.yaml");
    } catch (const Rigel::Asset::AssetLoadError&) {
        duplicateBoundary = true;
    }
    CHECK(duplicateBoundary);
    CHECK_EQ(assets.ns(), std::string("initial"));
    CHECK(assets.exists("generator_definitions/stable"));
    CHECK(!assets.exists("generator_definitions/default"));
    CHECK_EQ(generatorDeclarationNames(assets),
             std::vector<std::string>{"stable"});
    CHECK_EQ(assets.get<Rigel::Asset::RawAsset>("raw/stable"), retained);
    CHECK_NO_THROW(Rigel::Voxel::loadPreparedGeneratorDefinitionSnapshot(
        assets, registry, "test:valid", GeneratorDefinitionOrigin::Shipped));

    bool malformedBoundary = false;
    try {
        assets.loadManifest("malformed_generator_declaration.yaml");
    } catch (const Rigel::Asset::AssetLoadError&) {
        malformedBoundary = true;
    }
    CHECK(malformedBoundary);
    CHECK_EQ(assets.ns(), std::string("initial"));
    CHECK_EQ(generatorDeclarationNames(assets),
             std::vector<std::string>{"stable"});
    CHECK_EQ(assets.get<Rigel::Asset::RawAsset>("raw/stable"), retained);
}

TEST_CASE(GeneratorDefinitionLoader_missing_resource_preserves_prior_set_and_recovers) {
    Rigel::Asset::AssetManager assets;
    Rigel::Voxel::BlockRegistry registry;
    registerDefinitionMaterials(registry);
    commitInitialGeneratorSet(assets, registry);

    assets.loadManifest("aggregate_missing_resource.yaml");
    CHECK(assets.exists("generator_definitions/stable"));
    CHECK(!assets.exists("generator_definitions/a_valid"));
    CHECK(loadFailsAtAssetBoundary(assets, registry));
    CHECK_EQ(generatorDeclarationNames(assets),
             std::vector<std::string>{"stable"});
    CHECK_NO_THROW(Rigel::Voxel::loadPreparedGeneratorDefinitionSnapshot(
        assets, registry, "test:valid", GeneratorDefinitionOrigin::Shipped));

    assets.loadManifest("corrected.yaml");
    CHECK(assets.exists("generator_definitions/stable"));
    CHECK(!assets.exists("generator_definitions/corrected"));
    CHECK_NO_THROW(Rigel::Voxel::loadPreparedGeneratorDefinitionSnapshot(
        assets, registry, "test:valid", GeneratorDefinitionOrigin::Shipped));
    CHECK_EQ(assets.ns(), std::string("corrected"));
    CHECK(!assets.exists("generator_definitions/stable"));
    CHECK(assets.exists("generator_definitions/corrected"));
    CHECK_EQ(generatorDeclarationNames(assets),
             std::vector<std::string>{"corrected"});
}

TEST_CASE(GeneratorDefinitionLoader_aggregate_failure_after_valid_prefix_is_atomic) {
    Rigel::Asset::AssetManager assets;
    Rigel::Voxel::BlockRegistry registry;
    registerDefinitionMaterials(registry);
    commitInitialGeneratorSet(assets, registry);

    assets.loadManifest("aggregate_invalid_payload.yaml");
    CHECK(loadFailsAtAssetBoundary(assets, registry));
    CHECK_EQ(assets.ns(), std::string("initial"));
    CHECK(assets.exists("generator_definitions/stable"));
    CHECK(!assets.exists("generator_definitions/a_valid"));
    CHECK(!assets.exists("generator_definitions/z_invalid"));
    CHECK_EQ(generatorDeclarationNames(assets),
             std::vector<std::string>{"stable"});
    CHECK_NO_THROW(Rigel::Voxel::loadPreparedGeneratorDefinitionSnapshot(
        assets, registry, "test:valid", GeneratorDefinitionOrigin::Shipped));
}
