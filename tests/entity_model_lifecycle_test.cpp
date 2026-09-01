#include "TestFramework.h"

#include "Rigel/Entity/Entity.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>

using namespace Rigel::Asset;
using namespace Rigel::Entity;

namespace {

Handle<EntityModelAsset> modelHandle(
    std::string identifier,
    std::optional<Aabb> hitbox
) {
    auto model = std::make_shared<EntityModelAsset>();
    model->hitbox = std::move(hitbox);
    return Handle<EntityModelAsset>(std::move(model), std::move(identifier));
}

void checkDefaultBounds(const Entity& entity) {
    CHECK_EQ(entity.localBounds().min, glm::vec3(-0.5f));
    CHECK_EQ(entity.localBounds().max, glm::vec3(0.5f));
    CHECK_EQ(entity.worldBounds().min, entity.position() + glm::vec3(-0.5f));
    CHECK_EQ(entity.worldBounds().max, entity.position() + glm::vec3(0.5f));
}

void selectCustomHitbox(Entity& entity) {
    entity.setModel(modelHandle(
        "entity_models/custom",
        Aabb{glm::vec3(-0.25f, -1.0f, -0.75f),
             glm::vec3(0.25f, 2.0f, 0.75f)}));
    CHECK_EQ(
        entity.localBounds().min,
        glm::vec3(-0.25f, -1.0f, -0.75f));
    CHECK_EQ(
        entity.localBounds().max,
        glm::vec3(0.25f, 2.0f, 0.75f));
}

} // namespace

TEST_CASE(EntityModelLifecycle_ModelWithoutHitboxRestoresDefaultBounds) {
    Entity entity;
    entity.setPosition(3.0f, 4.0f, 5.0f);
    selectCustomHitbox(entity);

    entity.setModel(modelHandle("entity_models/no_hitbox", std::nullopt));

    CHECK(entity.model());
    CHECK_EQ(entity.modelIdentifier(), std::string("entity_models/no_hitbox"));
    checkDefaultBounds(entity);
}

TEST_CASE(EntityModelLifecycle_ClearingModelRestoresDefaultBounds) {
    Entity entity;
    entity.setPosition(3.0f, 4.0f, 5.0f);
    selectCustomHitbox(entity);

    entity.setModel({});

    CHECK(!entity.model());
    CHECK(entity.modelIdentifier().empty());
    checkDefaultBounds(entity);
}

TEST_CASE(EntityModelLifecycle_UnresolvedIdentifierRestoresDefaultBounds) {
    Entity entity;
    entity.setPosition(3.0f, 4.0f, 5.0f);
    selectCustomHitbox(entity);

    entity.setModelIdentifier("entity_models/unresolved");

    CHECK(!entity.model());
    CHECK_EQ(
        entity.modelIdentifier(),
        std::string("entity_models/unresolved"));
    checkDefaultBounds(entity);
}
