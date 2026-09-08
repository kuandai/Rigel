#include "TestFramework.h"

#include "ResourceRegistry.h"
#include "Rigel/Asset/AssetManager.h"
#include "Rigel/Voxel/BlockLoader.h"
#include "Rigel/Voxel/TextureAtlas.h"
#include "Rigel/Voxel/WorldResources.h"

#include <array>
#include <span>
#include <string>
#include <string_view>

using namespace Rigel::Asset;
using namespace Rigel::Voxel;

TEST_CASE(TextureAtlas_RejectsMissingBlockTextureWithoutDiscardingSemantics) {
    ResourceRegistry::SetScenario(ResourceRegistry::Scenario::MissingTexture);
    AssetManager assets;
    WorldResources resources;
    CHECK_NO_THROW(resources.initialize(assets));
    CHECK(resources.initialized());
    CHECK_EQ(resources.registry().size(), static_cast<size_t>(2));

    TextureAtlas atlas;
    std::string diagnostic;
    try {
        atlas.loadFromRegistry(resources.registry());
    } catch (const std::exception& error) {
        diagnostic = error.what();
    }
    CHECK(!diagnostic.empty());
    CHECK(diagnostic.find("textures/blocks/required.png") != std::string::npos);
    CHECK_EQ(atlas.textureCount(), static_cast<size_t>(0));
}

TEST_CASE(TextureAtlas_RollsBackEarlierTexturesAfterLateResourceFailure) {
    constexpr std::string_view modelYaml = R"(
id: two_textures
texture_slots: [first, second]
cuboids:
  - bounds: [0, 0, 0, 1, 1, 1]
    faces:
      pos_x: {texture: first}
      neg_x: {texture: second}
)";
    constexpr std::string_view blockYaml = R"(
id: late_texture_failure
model: two_textures
collision: full
textures:
  first: textures/invented/a_available.png
  second: textures/invented/z_missing.png
)";
    const std::array modelDefinitions = {
        BlockModelDefinitionSource{
            "models/blocks/two_textures.yaml",
            std::span<const char>(modelYaml.data(), modelYaml.size())}};
    const std::array blockDefinitions = {
        BlockDefinitionSource{
            "blocks/late_texture_failure.yaml",
            std::span<const char>(blockYaml.data(), blockYaml.size())}};
    BlockModelRegistry models;
    BlockRegistry blocks;
    BlockLoader loader;
    const BlockLoadReport report = loader.loadDefinitions(
        "test", modelDefinitions, blockDefinitions, models, blocks);

    CHECK_EQ(report.failed, static_cast<size_t>(0));
    CHECK_EQ(report.modelsLoaded, static_cast<size_t>(1));
    CHECK_EQ(report.loaded, static_cast<size_t>(1));
    CHECK(models.find("test:two_textures"));
    CHECK(blocks.findByIdentifier("test:late_texture_failure"));

    TextureAtlas atlas;
    CHECK_THROWS(atlas.loadFromRegistry(blocks));
    CHECK_EQ(atlas.textureCount(), static_cast<size_t>(0));
}

TEST_CASE(WorldResources_AcceptsTexturelessSemanticBlock) {
    ResourceRegistry::SetScenario(ResourceRegistry::Scenario::TexturelessBlock);
    AssetManager assets;
    WorldResources resources;

    CHECK_NO_THROW(resources.initialize(assets));
    CHECK(resources.initialized());
    CHECK_EQ(resources.registry().size(), static_cast<size_t>(2));
}
