#pragma once

#include "Aabb.h"
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

namespace Rigel::Voxel {
class World;
}

namespace Rigel::Entity {

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

    virtual void update(Voxel::World& world, float dt);
    virtual void render(const EntityRenderContext& ctx,
                        const glm::mat4& modelMatrix,
                        bool shouldRender);

    /**
     * Select a loaded model and use its hitbox. A missing model or a model
     * without a hitbox restores the centered one-unit default local bounds.
     */
    void setModel(Asset::Handle<EntityModelAsset> model);
    const Asset::Handle<EntityModelAsset>& model() const { return m_model; }
    /**
     * Retain an unresolved model identifier for persistence. This clears the
     * loaded model and restores the centered one-unit default local bounds.
     */
    void setModelIdentifier(std::string identifier);
    const std::string& modelIdentifier() const { return m_modelIdentifier; }

    EntityModelInstance* modelInstance() const { return m_modelInstance.get(); }
    void clearModelInstance();
    bool ensureModelInstance(Asset::AssetManager& assets,
                             const Asset::Handle<Asset::ShaderAsset>& shader);

    void setRenderTint(const glm::vec4& tint) { m_renderTint = tint; }
    const glm::vec4& renderTint() const { return m_renderTint; }

protected:
    void applyFloorFriction(float friction);
    void updateWorldBounds();

    /**
     * Sweep against static block shapes in X, then Y, then Z order.
     * Stationary initial overlap is intentionally left in place; this is not
     * a general depenetration solver. An axis movement whose collision query
     * is outside the world's finite work and coordinate bounds is cancelled.
     */
    void resolveCollisions(Voxel::World& world, float dt);

    EntityId m_id;
    std::string m_typeId;
    glm::vec3 m_position{0.0f};
    glm::vec3 m_velocity{0.0f};
    glm::vec3 m_acceleration{0.0f};
    glm::vec3 m_viewDirection{0.0f, 0.0f, -1.0f};
    float m_gravityModifier = 1.0f;
    bool m_onGround = false;
    bool m_collidedX = false;
    bool m_collidedY = false;
    bool m_collidedZ = false;
    float m_floorFriction = 0.1f;

    Aabb m_localBounds{defaultLocalBounds()};
    Aabb m_worldBounds{};

    EntityTagList m_tags;
    Asset::Handle<EntityModelAsset> m_model;
    std::string m_modelIdentifier;
    std::unique_ptr<EntityModelInstance> m_modelInstance;
    glm::vec4 m_renderTint{1.0f};

private:
    static const Aabb& defaultLocalBounds();
};

} // namespace Rigel::Entity
