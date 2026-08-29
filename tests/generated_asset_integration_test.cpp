#include "TestFramework.h"

#include "OpenGLFixture.h"
#include "Rigel/Asset/AssetManager.h"
#include "Rigel/Voxel/GeneratorDefinitionLoader.h"
#include "Rigel/Voxel/WorldGenerator.h"
#include "Rigel/Voxel/WorldResources.h"

#include <array>
#include <string_view>

TEST_CASE(GeneratedAssets_InitializeProductionWorldResources) {
    Rigel::Test::HiddenOpenGLContext context;
    context.require();

    Rigel::Asset::AssetManager assets;
    assets.loadManifest("manifest.yaml");

    Rigel::Voxel::WorldResources resources;
    resources.initialize(assets);
    CHECK(resources.initialized());

    constexpr std::array<std::string_view, 5> RequiredMaterials = {
        "base:dirt",
        "base:grass",
        "base:sand",
        "base:stone_shale",
        "base:water[type=source]",
    };
    for (const std::string_view identifier : RequiredMaterials) {
        CHECK(resources.registry().findByIdentifier(std::string(identifier)));
    }

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
