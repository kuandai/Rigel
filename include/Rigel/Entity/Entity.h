#pragma once

#include "Aabb.h"
#include "EntityComponents.h"
#include "EntityId.h"
#include "EntityModel.h"
#include "EntityModelInstance.h"
#include "EntityRenderContext.h"
#include "EntityTags.h"

#include <Rigel/Asset/Handle.h>
#include <Rigel/Asset/AssetManager.h>

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <memory>
#include <string>
#include <vector>

namespace Rigel::Entity {

class EntityChunk;

enum class Axis : uint8_t {
    X,
    Y,
    Z
};

class Entity {
public:
    explicit Entity(std::string typeId = "rigel:entity");
    virtual ~Entity() = default;

    const EntityId& id() const { return m_id; }
    const std::string& typeId() const { return m_typeId; }
    void setId(const EntityId& id) { m_id = id; }

    const glm::vec3& position() const { return m_position; }
    const glm::vec3& velocity() const { return m_velocity; }
    const glm::vec3& viewDirection() const { return m_viewDirection; }

    void setPosition(const glm::vec3& pos);
    void setPosition(float x, float y, float z);

    void setVelocity(const glm::vec3& vel) { m_velocity = vel; }
    void setViewDirection(const glm::vec3& viewDir) { m_viewDirection = viewDir; }
    void accelerate(const glm::vec3& accel) { m_acceleration += accel; }
    void accelerate(float x, float y, float z) { m_acceleration += glm::vec3(x, y, z); }

    void setLocalBounds(const Aabb& bounds);
    const Aabb& localBounds() const { return m_localBounds; }
    const Aabb& worldBounds() const { return m_worldBounds; }

    bool isOnGround() const { return m_onGround; }
    bool collidedX() const { return m_collidedX; }
    bool collidedY() const { return m_collidedY; }
    bool collidedZ() const { return m_collidedZ; }

    void addTag(std::string_view tag) { m_tags.add(tag); }
    void removeTag(std::string_view tag) { m_tags.remove(tag); }
    bool hasTag(std::string_view tag) const { return m_tags.has(tag); }

    bool isNoClip() const { return hasTag(EntityTags::NoClip); }

    void addUpdateComponent(IUpdateEntityComponent* component);
    void removeUpdateComponent(IUpdateEntityComponent* component);

    void addRenderComponent(IRenderEntityComponent* component);
    void removeRenderComponent(IRenderEntityComponent* component);

    virtual void update(Voxel::World& world, float dt);
    virtual void render(const EntityRenderContext& ctx,
                        const glm::mat4& modelMatrix,
                        bool shouldRender);

    void setModel(Asset::Handle<EntityModelAsset> model);
    const Asset::Handle<EntityModelAsset>& model() const { return m_model; }

    IEntityModelInstance* modelInstance() const { return m_modelInstance.get(); }
    void clearModelInstance();
    bool ensureModelInstance(Asset::AssetManager& assets,
                             const Asset::Handle<Asset::ShaderAsset>& shader);

    void setRenderTint(const glm::vec4& tint) { m_renderTint = tint; }
    const glm::vec4& renderTint() const { return m_renderTint; }

    EntityChunk* currentChunk() const { return m_currentChunk; }
    void setCurrentChunk(EntityChunk* chunk) { m_currentChunk = chunk; }

protected:
    virtual void onCollide(Axis axis) { (void)axis; }

    void applyFloorFriction(float friction);
    void updateWorldBounds();
    void resolveCollisions(Voxel::World& world, float dt);

    EntityId m_id;
    std::string m_typeId;
    glm::vec3 m_position{0.0f};
    glm::vec3 m_lastPosition{0.0f};
    glm::vec3 m_velocity{0.0f};
    glm::vec3 m_acceleration{0.0f};
    glm::vec3 m_viewDirection{0.0f, 0.0f, -1.0f};
    float m_gravityModifier = 1.0f;
    float m_maxStepHeight = 0.5f;
    bool m_onGround = false;
    bool m_collidedX = false;
    bool m_collidedY = false;
    bool m_collidedZ = false;
    float m_maxHitpoints = 10.0f;
    float m_hitpoints = 10.0f;
    float m_age = 0.0f;
    float m_floorFriction = 0.1f;

    Aabb m_localBounds{glm::vec3(-0.5f), glm::vec3(0.5f)};
    Aabb m_worldBounds{};

    EntityTagList m_tags;
    std::vector<IUpdateEntityComponent*> m_updateComponents;
    std::vector<IRenderEntityComponent*> m_renderComponents;

    EntityChunk* m_currentChunk = nullptr;
    Asset::Handle<EntityModelAsset> m_model;
    std::unique_ptr<IEntityModelInstance> m_modelInstance;
    glm::vec4 m_renderTint{1.0f};
};

} // namespace Rigel::Entity
