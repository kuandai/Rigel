#include "TestFramework.h"

#include "ResourceRegistry.h"
#include "Rigel/Asset/AssetManager.h"
#include "Rigel/Voxel/BlockLoader.h"
#include "Rigel/Voxel/WorldResources.h"

#include <array>
#include <span>
#include <string>
#include <string_view>

using namespace Rigel::Asset;
using namespace Rigel::Voxel;

TEST_CASE(WorldResources_RejectsMissingBlockTexture) {
    ResourceRegistry::SetScenario(ResourceRegistry::Scenario::MissingTexture);
    AssetManager assets;
    WorldResources resources;
    std::string diagnostic;

    try {
        resources.initialize(assets);
    } catch (const std::exception& e) {
        diagnostic = e.what();
    }

    CHECK(!diagnostic.empty());
    CHECK(!resources.initialized());
    CHECK(diagnostic.find("0 definitions loaded") != std::string::npos);
    CHECK(diagnostic.find("1 failed") != std::string::npos);
    CHECK(diagnostic.find("0 textures loaded") != std::string::npos);
    CHECK(diagnostic.find("scripts/rigel_assets.py stage") != std::string::npos);
    CHECK(diagnostic.find("blocks/required_block.yaml") != std::string::npos);
    CHECK(diagnostic.find("textures/blocks/required.png") != std::string::npos);
}

TEST_CASE(BlockLoader_RollsBackEarlierTexturesAfterLateResourceFailure) {
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
    TextureAtlas atlas;

    BlockLoader loader;
    const BlockLoadReport report = loader.loadDefinitions(
        "test", modelDefinitions, blockDefinitions, models, blocks, atlas);

    CHECK_EQ(report.failed, static_cast<size_t>(1));
    CHECK_EQ(report.modelsLoaded, static_cast<size_t>(0));
    CHECK_EQ(report.loaded, static_cast<size_t>(0));
    CHECK_EQ(models.size(), static_cast<size_t>(2));
    CHECK_EQ(blocks.size(), static_cast<size_t>(1));
    CHECK_EQ(atlas.textureCount(), static_cast<size_t>(0));
    CHECK(!models.find("test:two_textures"));
    CHECK(!blocks.findByIdentifier("test:late_texture_failure"));
    CHECK(report.representativeFailures.front().reason.find("z_missing.png") !=
          std::string::npos);
}

TEST_CASE(WorldResources_PostLoadRejectionIsPristineAndRetryable) {
    ResourceRegistry::SetScenario(ResourceRegistry::Scenario::TexturelessBlock);
    AssetManager assets;
    WorldResources resources;

    std::string firstDiagnostic;
    std::string secondDiagnostic;
    try {
        resources.initialize(assets);
    } catch (const std::exception& error) {
        firstDiagnostic = error.what();
    }

    CHECK(!firstDiagnostic.empty());
    CHECK(!resources.initialized());
    CHECK_EQ(resources.registry().size(), static_cast<size_t>(1));
    CHECK_EQ(resources.textureAtlas().textureCount(), static_cast<size_t>(0));

    try {
        resources.initialize(assets);
    } catch (const std::exception& error) {
        secondDiagnostic = error.what();
    }

    CHECK_EQ(secondDiagnostic, firstDiagnostic);
    CHECK(!resources.initialized());
    CHECK_EQ(resources.registry().size(), static_cast<size_t>(1));
    CHECK_EQ(resources.textureAtlas().textureCount(), static_cast<size_t>(0));
}
