#include "TestFramework.h"
#include "LogCapture.h"
#include "OpenGLFixture.h"

#include "Rigel/Entity/EntityRenderer.h"
#include "Rigel/Entity/Aabb.h"
#include "Rigel/Entity/Entity.h"
#include "Rigel/Entity/EntityModelLoader.h"
#include "Rigel/Asset/AssetManager.h"
#include "Rigel/Asset/ShaderLoader.h"
#include "Rigel/Voxel/World.h"
#include "Rigel/Voxel/WorldResources.h"

#include <glm/mat4x4.hpp>

using namespace Rigel::Entity;

namespace Rigel::Entity::detail {

struct EntityRendererTestAccess {
    static size_t instanceCount(const EntityRenderer& renderer) {
        return renderer.m_instances.size();
    }

    static const EntityModelAsset* model(
        const EntityRenderer& renderer,
        const EntityId& id
    ) {
        const auto found = renderer.m_instances.find(id);
        return found == renderer.m_instances.end()
            ? nullptr
            : found->second.model.get();
    }

    static GLuint vertexArray(
        const EntityRenderer& renderer,
        const EntityId& id
    ) {
        const auto found = renderer.m_instances.find(id);
        return found == renderer.m_instances.end() || !found->second.instance
            ? 0
            : found->second.instance->m_vao;
    }
};

} // namespace Rigel::Entity::detail

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

TEST_CASE(EntityRenderer_OwnsReplacementRemovalAndTeardownLifetimes) {
    Rigel::Test::HiddenOpenGLContext context;
    context.require();

    Rigel::Asset::AssetManager assets;
    assets.registerLoader(
        "shaders", std::make_unique<Rigel::Asset::ShaderLoader>());
    assets.registerLoader(
        "entity_models", std::make_unique<EntityModelLoader>());
    assets.registerLoader(
        "entity_anims", std::make_unique<EntityAnimationSetLoader>());
    assets.loadManifest("manifest.yaml");

    const auto model =
        assets.get<EntityModelAsset>("entity_models/demo_cube");
    auto replacementAsset = std::make_shared<EntityModelAsset>();
    replacementAsset->texWidth = model->texWidth;
    replacementAsset->texHeight = model->texHeight;
    replacementAsset->modelScale = model->modelScale;
    replacementAsset->renderOffset = model->renderOffset;
    replacementAsset->lighting = model->lighting;
    replacementAsset->hitbox = model->hitbox;
    replacementAsset->bones = model->bones;
    replacementAsset->boneLookup = model->boneLookup;
    const Rigel::Asset::Handle<EntityModelAsset> replacement(
        replacementAsset, "entity_models/replacement");

    Rigel::Voxel::WorldResources resources;
    Rigel::Voxel::World world(resources);
    auto entity = std::make_unique<Entity>();
    Entity* entityProbe = entity.get();
    entity->setModel(model);
    const EntityId id = world.entities().spawn(std::move(entity));

    EntityRenderer renderer;
    renderer.initialize(assets);
    EntityRenderContext renderContext;
    renderer.render(world, renderContext);

    CHECK_EQ(detail::EntityRendererTestAccess::instanceCount(renderer), size_t{1});
    CHECK_EQ(detail::EntityRendererTestAccess::model(renderer, id), model.get());
    const GLuint initialVertexArray =
        detail::EntityRendererTestAccess::vertexArray(renderer, id);
    CHECK_NE(initialVertexArray, 0u);
    CHECK(glIsVertexArray(initialVertexArray) == GL_TRUE);

    entityProbe->setModel(replacement);
    renderer.render(world, renderContext);
    CHECK_EQ(
        detail::EntityRendererTestAccess::model(renderer, id),
        replacement.get());
    CHECK_NE(
        detail::EntityRendererTestAccess::vertexArray(renderer, id), 0u);

    CHECK(world.entities().despawn(id));
    renderer.render(world, renderContext);
    CHECK_EQ(detail::EntityRendererTestAccess::instanceCount(renderer), size_t{0});

    auto teardownEntity = std::make_unique<Entity>();
    teardownEntity->setModel(model);
    const EntityId teardownId =
        world.entities().spawn(std::move(teardownEntity));
    renderer.render(world, renderContext);
    const GLuint teardownVertexArray =
        detail::EntityRendererTestAccess::vertexArray(renderer, teardownId);
    CHECK_NE(teardownVertexArray, 0u);

    renderer.release();
    CHECK_EQ(detail::EntityRendererTestAccess::instanceCount(renderer), size_t{0});
    CHECK(glIsVertexArray(teardownVertexArray) == GL_FALSE);
    assets.clearCache();
}
