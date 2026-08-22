#include "TestFramework.h"
#include "LogCapture.h"

#include "Rigel/Entity/EntityRenderer.h"
#include "Rigel/Entity/Aabb.h"
#include "Rigel/Asset/AssetManager.h"

#include <glm/mat4x4.hpp>

using namespace Rigel::Entity;

namespace {

class OptionalShaderFailureLoader final : public Rigel::Asset::IAssetLoader {
public:
    std::string_view category() const override {
        return "shaders";
    }

    std::shared_ptr<Rigel::Asset::AssetBase> load(
        const Rigel::Asset::LoadContext& context) override {
        if (context.id == "shaders/entity" ||
            context.id == "shaders/entity_shadow_depth") {
            ++failureCount;
            throw std::runtime_error(
                "injected optional shader failure for " + context.id);
        }
        return std::make_shared<Rigel::Asset::ShaderAsset>();
    }

    size_t failureCount = 0;
};

} // namespace

TEST_CASE(EntityRenderer_CullsOutsideFrustum) {
    Aabb inside;
    inside.min = glm::vec3(-0.5f);
    inside.max = glm::vec3(0.5f);

    Aabb outside;
    outside.min = glm::vec3(2.0f, 2.0f, 2.0f);
    outside.max = glm::vec3(3.0f, 3.0f, 3.0f);

    glm::mat4 viewProjection(1.0f);

    CHECK(EntityRenderer::isVisible(inside, viewProjection));
    CHECK(!EntityRenderer::isVisible(outside, viewProjection));
}

TEST_CASE(EntityRenderer_OptionalShadersCoverAbsentAndFailedAssets) {
    {
        Rigel::Test::LogCapture logs("entity-shader-absent-test");
        Rigel::Asset::AssetManager assets;
        EntityRenderer renderer;
        CHECK_NO_THROW(renderer.initialize(assets));
        const auto output = logs.output();
        CHECK_EQ(Rigel::Test::countOccurrences(
                     output, "Optional startup resource 'shaders/entity'"),
                 static_cast<size_t>(1));
        CHECK_EQ(Rigel::Test::countOccurrences(
                     output,
                     "Optional startup resource 'shaders/entity_shadow_depth'"),
                 static_cast<size_t>(1));
    }

    Rigel::Test::LogCapture logs("entity-shader-failure-test");
    Rigel::Asset::AssetManager assets;
    auto loader = std::make_unique<OptionalShaderFailureLoader>();
    auto* loaderProbe = loader.get();
    assets.registerLoader("shaders", std::move(loader));
    assets.loadManifest("manifest.yaml");

    EntityRenderer renderer;
    CHECK_NO_THROW(renderer.initialize(assets));
    CHECK_EQ(loaderProbe->failureCount, static_cast<size_t>(2));
    const auto output = logs.output();
    CHECK_EQ(Rigel::Test::countOccurrences(
                 output, "Optional startup resource 'shaders/entity'"),
             static_cast<size_t>(1));
    CHECK_EQ(Rigel::Test::countOccurrences(
                 output,
                 "Optional startup resource 'shaders/entity_shadow_depth'"),
             static_cast<size_t>(1));
}
