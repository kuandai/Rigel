#include "TestFramework.h"

#include "Rigel/Asset/AssetLoader.h"
#include "Rigel/Asset/AssetManager.h"
#include "Rigel/Asset/RawLoader.h"
#include "Rigel/Persistence/Backends/Memory/MemoryFormat.h"
#include "Rigel/Persistence/FormatRegistry.h"
#include "Rigel/Persistence/PersistenceService.h"
#include "Rigel/Persistence/Storage.h"
#include "Rigel/Persistence/WorldSettings.h"
#include "Rigel/Voxel/BlockRegistry.h"
#include "Rigel/Voxel/BlockType.h"
#include "Rigel/Voxel/GeneratorDefinitionLoader.h"

#include <algorithm>
#include <memory>
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
    const Rigel::Voxel::BlockRegistry& registry,
    std::string* failedAssetId = nullptr) {
    try {
        static_cast<void>(
            Rigel::Voxel::loadPreparedGeneratorDefinitionSnapshot(
                assets, registry, "test:valid",
                GeneratorDefinitionOrigin::Shipped));
    } catch (const Rigel::Asset::AssetLoadError& error) {
        if (failedAssetId) {
            *failedAssetId = error.assetId();
        }
        return true;
    }
    return false;
}

std::string rawText(
    const Rigel::Asset::Handle<Rigel::Asset::RawAsset>& asset) {
    return std::string(asset->str());
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

TEST_CASE(AssetManager_DefersDuplicateGeneratorDeclarationFields) {
    Rigel::Asset::AssetManager assets;
    Rigel::Voxel::BlockRegistry registry;
    registerDefinitionMaterials(registry);

    CHECK_NO_THROW(assets.loadManifest("duplicate_generator_field.yaml"));
    CHECK(!assets.exists("generator_definitions/default"));
    CHECK_EQ(assets.ns(), std::string("test"));
    CHECK(generatorDeclarationNames(assets).empty());
    CHECK(loadFailsAtAssetBoundary(assets, registry));
    CHECK(assets.ns().empty());
}

TEST_CASE(AssetManager_DefersDuplicateGeneratorDeclarationNames) {
    Rigel::Asset::AssetManager assets;
    Rigel::Voxel::BlockRegistry registry;
    registerDefinitionMaterials(registry);

    CHECK_NO_THROW(assets.loadManifest("duplicate_generator_name.yaml"));
    CHECK(!assets.exists("generator_definitions/default"));
    CHECK_EQ(assets.ns(), std::string("test"));
    CHECK(generatorDeclarationNames(assets).empty());
    CHECK(loadFailsAtAssetBoundary(assets, registry));
    CHECK(assets.ns().empty());
}

TEST_CASE(GeneratorDefinitionLoader_manifest_failure_preserves_prior_complete_set) {
    Rigel::Asset::AssetManager assets;
    Rigel::Voxel::BlockRegistry registry;
    registerDefinitionMaterials(registry);
    commitInitialGeneratorSet(assets, registry);
    const auto retained = assets.get<Rigel::Asset::RawAsset>("raw/stable");

    CHECK_NO_THROW(assets.loadManifest("duplicate_generator_name.yaml"));
    CHECK_EQ(assets.ns(), std::string("test"));
    CHECK(loadFailsAtAssetBoundary(assets, registry));
    CHECK_EQ(assets.ns(), std::string("initial"));
    CHECK(assets.exists("generator_definitions/stable"));
    CHECK(!assets.exists("generator_definitions/default"));
    CHECK_EQ(generatorDeclarationNames(assets),
             std::vector<std::string>{"stable"});
    CHECK_EQ(assets.get<Rigel::Asset::RawAsset>("raw/stable"), retained);
    CHECK_NO_THROW(assets.loadManifest("malformed_generator_declaration.yaml"));
    CHECK_EQ(assets.ns(), std::string("failed-malformed"));
    CHECK(assets.exists("raw/required"));
    CHECK_EQ(
        rawText(assets.get<Rigel::Asset::RawAsset>("raw/required")),
        std::string("ordinary required asset"));
    CHECK(loadFailsAtAssetBoundary(assets, registry));
    CHECK_EQ(assets.ns(), std::string("initial"));
    CHECK_EQ(generatorDeclarationNames(assets),
             std::vector<std::string>{"stable"});
    CHECK_EQ(assets.get<Rigel::Asset::RawAsset>("raw/stable"), retained);
    CHECK(!assets.exists("raw/required"));
}

TEST_CASE(AssetManager_DoesNotDeferUnrelatedManifestLoadFailure) {
    Rigel::Asset::AssetManager assets;
    Rigel::Voxel::BlockRegistry registry;
    registerDefinitionMaterials(registry);
    commitInitialGeneratorSet(assets, registry);
    const auto retained = assets.get<Rigel::Asset::RawAsset>("raw/stable");

    CHECK_THROWS(assets.loadManifest("missing_manifest.yaml"));
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
    const auto retained = assets.get<Rigel::Asset::RawAsset>("raw/stable");

    assets.loadManifest("aggregate_missing_resource.yaml");
    CHECK_EQ(assets.ns(), std::string("failed-missing"));
    CHECK(assets.exists("generator_definitions/stable"));
    CHECK(!assets.exists("generator_definitions/a_valid"));
    CHECK_EQ(assets.get<Rigel::Asset::RawAsset>("raw/stable"), retained);
    const auto provisional =
        assets.get<Rigel::Asset::RawAsset>("raw/provisional");
    CHECK_EQ(rawText(provisional), std::string("new provisional entry"));
    CHECK(loadFailsAtAssetBoundary(assets, registry));
    CHECK_EQ(assets.ns(), std::string("initial"));
    CHECK_EQ(assets.get<Rigel::Asset::RawAsset>("raw/stable"), retained);
    CHECK(!assets.exists("raw/provisional"));
    CHECK_EQ(generatorDeclarationNames(assets),
             std::vector<std::string>{"stable"});
    CHECK_NO_THROW(Rigel::Voxel::loadPreparedGeneratorDefinitionSnapshot(
        assets, registry, "test:valid", GeneratorDefinitionOrigin::Shipped));

    assets.loadManifest("corrected.yaml");
    CHECK_EQ(assets.get<Rigel::Asset::RawAsset>("raw/stable"), retained);
    const auto committedBeforeCommit =
        assets.get<Rigel::Asset::RawAsset>("raw/committed");
    CHECK(assets.exists("generator_definitions/stable"));
    CHECK(!assets.exists("generator_definitions/corrected"));
    const auto corrected =
        Rigel::Voxel::loadPreparedGeneratorDefinitionSnapshot(
            assets, registry, "test:corrected",
            GeneratorDefinitionOrigin::Shipped);
    CHECK_EQ(corrected.sourceId, std::string("test:corrected"));
    CHECK_EQ(corrected.sourceRevision, uint32_t{9});
    CHECK_EQ(corrected.data.densityGraph.nodes.front().value, 0.5f);
    CHECK_EQ(assets.ns(), std::string("corrected"));
    CHECK(!assets.exists("generator_definitions/stable"));
    CHECK(assets.exists("generator_definitions/corrected"));
    CHECK_EQ(generatorDeclarationNames(assets),
             std::vector<std::string>{"corrected"});
    const auto correctedStable =
        assets.get<Rigel::Asset::RawAsset>("raw/stable");
    const auto committedAfterCommit =
        assets.get<Rigel::Asset::RawAsset>("raw/committed");
    CHECK_NE(correctedStable, retained);
    CHECK_EQ(rawText(correctedStable),
             std::string("corrected committed entry"));
    CHECK_NE(committedAfterCommit, committedBeforeCommit);
    CHECK_EQ(rawText(committedAfterCommit),
             std::string("new committed entry"));
}

TEST_CASE(GeneratorDefinitionLoader_aggregate_failure_after_valid_prefix_is_atomic) {
    Rigel::Asset::AssetManager assets;
    Rigel::Voxel::BlockRegistry registry;
    registerDefinitionMaterials(registry);
    commitInitialGeneratorSet(assets, registry);
    const auto retained = assets.get<Rigel::Asset::RawAsset>("raw/stable");

    assets.loadManifest("aggregate_invalid_payload.yaml");
    const auto provisional =
        assets.get<Rigel::Asset::RawAsset>("raw/provisional");
    std::string failedAssetId;
    CHECK(loadFailsAtAssetBoundary(assets, registry, &failedAssetId));
    CHECK_EQ(failedAssetId, std::string("generator_definitions/z_invalid"));
    CHECK_EQ(assets.ns(), std::string("initial"));
    CHECK(assets.exists("generator_definitions/stable"));
    CHECK(!assets.exists("generator_definitions/a_valid"));
    CHECK(!assets.exists("generator_definitions/z_invalid"));
    CHECK_EQ(generatorDeclarationNames(assets),
             std::vector<std::string>{"stable"});
    CHECK_EQ(assets.get<Rigel::Asset::RawAsset>("raw/stable"), retained);
    CHECK(!assets.exists("raw/provisional"));
    CHECK_EQ(rawText(provisional), std::string("new provisional entry"));
}

TEST_CASE(GeneratorDefinitionLoader_missing_selection_discards_complete_candidate) {
    Rigel::Asset::AssetManager assets;
    Rigel::Voxel::BlockRegistry registry;
    registerDefinitionMaterials(registry);
    commitInitialGeneratorSet(assets, registry);
    const auto retained = assets.get<Rigel::Asset::RawAsset>("raw/stable");

    assets.loadManifest("corrected.yaml");
    const auto provisional =
        assets.get<Rigel::Asset::RawAsset>("raw/committed");
    CHECK_THROWS(Rigel::Voxel::loadPreparedGeneratorDefinitionSnapshot(
        assets, registry, "test:missing", GeneratorDefinitionOrigin::Shipped));

    CHECK_EQ(assets.ns(), std::string("initial"));
    CHECK_EQ(generatorDeclarationNames(assets),
             std::vector<std::string>{"stable"});
    CHECK_EQ(assets.get<Rigel::Asset::RawAsset>("raw/stable"), retained);
    CHECK(!assets.exists("raw/committed"));
    CHECK_EQ(rawText(provisional), std::string("new committed entry"));
}

TEST_CASE(GeneratorDefinitionLoader_corrected_manifest_replaces_deferred_error) {
    Rigel::Asset::AssetManager assets;
    Rigel::Voxel::BlockRegistry registry;
    registerDefinitionMaterials(registry);

    assets.loadManifest("malformed_generator_declaration.yaml");
    CHECK(assets.exists("raw/required"));
    CHECK_EQ(
        rawText(assets.get<Rigel::Asset::RawAsset>("raw/required")),
        std::string("ordinary required asset"));

    CHECK_NO_THROW(assets.loadManifest("corrected.yaml"));
    CHECK(!assets.exists("raw/required"));
    const auto corrected =
        Rigel::Voxel::loadPreparedGeneratorDefinitionSnapshot(
            assets, registry, "test:corrected",
            GeneratorDefinitionOrigin::Shipped);
    CHECK_EQ(corrected.sourceId, std::string("test:corrected"));
    CHECK_EQ(corrected.sourceRevision, uint32_t{9});
    CHECK_EQ(assets.ns(), std::string("corrected"));
}

TEST_CASE(GeneratorDefinitionLoader_published_bootstrap_bypasses_deferred_install_failure) {
    Rigel::Test::TemporaryDirectory directory(
        "rigel_deferred_generator_manifest_bootstrap");
    auto storage =
        std::make_shared<Rigel::Persistence::FilesystemBackend>();
    Rigel::Persistence::FormatRegistry formats;
    formats.registerFormat(
        Rigel::Persistence::Backends::Memory::descriptor(),
        Rigel::Persistence::Backends::Memory::factory(),
        Rigel::Persistence::Backends::Memory::probe());
    Rigel::Persistence::PersistenceService persistence(formats);
    Rigel::Voxel::BlockRegistry registry;
    registerDefinitionMaterials(registry);
    Rigel::Asset::AssetManager assets;

    assets.loadManifest("corrected.yaml");
    const auto initialDefinition =
        Rigel::Voxel::loadPreparedGeneratorDefinitionSnapshot(
            assets, registry, "test:corrected",
            GeneratorDefinitionOrigin::Shipped);
    Rigel::Persistence::PersistenceContext publishedContext;
    publishedContext.rootPath =
        (directory.path() / "world_published").string();
    publishedContext.preferredFormat = "memory";
    publishedContext.storage = storage;
    static_cast<void>(Rigel::Persistence::bootstrapWorldGeneration(
        [&] {
            return Rigel::Persistence::NewWorldGeneration{
                "Published", 17u, initialDefinition};
        },
        persistence,
        registry,
        publishedContext));

    assets.loadManifest("malformed_generator_declaration.yaml");
    CHECK_EQ(
        rawText(assets.get<Rigel::Asset::RawAsset>("raw/required")),
        std::string("ordinary required asset"));
    size_t installedResolutionCalls = 0;
    Rigel::Persistence::NewWorldGenerationFactory installedResolver = [&] {
        ++installedResolutionCalls;
        return Rigel::Persistence::NewWorldGeneration{
            "Missing",
            18u,
            Rigel::Voxel::loadPreparedGeneratorDefinitionSnapshot(
                assets, registry, "test:corrected",
                GeneratorDefinitionOrigin::Shipped)};
    };

    const auto published = Rigel::Persistence::bootstrapWorldGeneration(
        installedResolver,
        persistence,
        registry,
        publishedContext);
    CHECK_EQ(installedResolutionCalls, size_t{0});
    CHECK_EQ(published.generation.settings.seed, 17u);
    CHECK_EQ(published.generation.settings.generator.sourceId,
             std::string("test:corrected"));

    Rigel::Persistence::PersistenceContext missingContext = publishedContext;
    missingContext.rootPath =
        (directory.path() / "world_missing").string();
    bool assetBoundary = false;
    try {
        static_cast<void>(Rigel::Persistence::bootstrapWorldGeneration(
            installedResolver,
            persistence,
            registry,
            missingContext));
    } catch (const Rigel::Asset::AssetLoadError&) {
        assetBoundary = true;
    }
    CHECK(assetBoundary);
    CHECK_EQ(installedResolutionCalls, size_t{1});
    CHECK(!storage->exists(missingContext.rootPath));
}
