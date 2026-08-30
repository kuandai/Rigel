#include "TestFramework.h"

#include "OpenGLFixture.h"
#include "Rigel/Asset/AssetManager.h"
#include "Rigel/Voxel/BlockLoader.h"
#include "Rigel/Voxel/BlockModel.h"
#include "Rigel/Voxel/BlockRegistry.h"
#include "Rigel/Voxel/GeneratorDefinitionLoader.h"
#include "Rigel/Voxel/TextureAtlas.h"
#include "Rigel/Voxel/WorldGenerator.h"
#include "Rigel/Voxel/WorldResources.h"

#include <array>
#include <string_view>

namespace {
constexpr std::array<std::string_view, 5> RequiredMaterials = {
    "base:dirt",
    "base:grass",
    "base:sand",
    "base:stone_shale",
    "base:water[type=source]",
};
} // namespace

TEST_CASE(GeneratedAssets_LoadNormalizedBlockDefinitions) {
    Rigel::Asset::AssetManager assets;
    assets.loadManifest("manifest.yaml");

    Rigel::Voxel::BlockModelRegistry models;
    Rigel::Voxel::BlockRegistry preparedRegistry;
    Rigel::Voxel::TextureAtlas preparedAtlas;
    const Rigel::Voxel::BlockLoadReport report =
        Rigel::Voxel::BlockLoader{}.loadFromManifest(
            assets, models, preparedRegistry, preparedAtlas);
    CHECK(report.modelsLoaded > 0);
    CHECK_EQ(report.modelsFailed, static_cast<size_t>(0));
    CHECK_EQ(report.failed, static_cast<size_t>(0));
    CHECK(report.loaded > 0);
    for (const std::string_view identifier : RequiredMaterials) {
        CHECK(preparedRegistry.findByIdentifier(std::string(identifier)));
    }
}

TEST_CASE(GeneratedAssets_InitializeProductionWorldResources) {
    Rigel::Test::HiddenOpenGLContext context;
    context.require();

    Rigel::Asset::AssetManager assets;
    assets.loadManifest("manifest.yaml");

    Rigel::Voxel::WorldResources resources;
    resources.initialize(assets);
    CHECK(resources.initialized());

    const auto generator =
        Rigel::Voxel::loadPreparedGeneratorDefinitionSnapshot(
            assets, resources.registry(), "rigel:default");
    Rigel::Voxel::WorldGenerator worldGenerator(
        resources.registry(), generator.data, 1337u);
    Rigel::Voxel::ChunkBuffer chunk;
    worldGenerator.generate({0, 3, 0}, chunk);
    CHECK(!chunk.blocks.empty());

    resources.releaseRenderResources();
}
