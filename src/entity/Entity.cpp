#include "Rigel/Entity/Entity.h"

#include "Rigel/Entity/EntityUtils.h"
#include "Rigel/Voxel/World.h"

#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>

namespace Rigel::Entity {

namespace {

enum class Axis {
    X,
    Y,
    Z
};

size_t axisIndex(Axis axis) {
    return static_cast<size_t>(axis);
}

float axisValue(const glm::vec3& value, Axis axis) {
    switch (axis) {
        case Axis::X: return value.x;
        case Axis::Y: return value.y;
        case Axis::Z: return value.z;
    }
    return 0.0f;
}

void setAxisValue(glm::vec3& value, Axis axis, float axisValue) {
    switch (axis) {
        case Axis::X: value.x = axisValue; break;
        case Axis::Y: value.y = axisValue; break;
        case Axis::Z: value.z = axisValue; break;
    }
}

Voxel::BlockCollisionBox collisionBox(const Aabb& box) {
    return {
        .min = {box.min.x, box.min.y, box.min.z},
        .max = {box.max.x, box.max.y, box.max.z},
    };
}

bool overlapsOnOtherAxes(
    const Aabb& entity,
    const Voxel::BlockCollisionBox& block,
    Axis movementAxis
) {
    const size_t movementIndex = axisIndex(movementAxis);
    for (size_t axis = 0; axis < 3; ++axis) {
        if (axis == movementIndex) continue;
        if (entity.min[axis] >=
                block.max[axis] - Voxel::BlockCollisionContactTolerance ||
            entity.max[axis] <=
                block.min[axis] + Voxel::BlockCollisionContactTolerance) {
            return false;
        }
    }
    return true;
}

void resolveAxis(Voxel::World& world,
                 const Aabb& localBounds,
                 glm::vec3& position,
                 glm::vec3& velocity,
                 Axis axis,
                 float dt,
                 bool& collided,
                 bool& onGround) {
    const float delta = axisValue(velocity, axis) * dt;
    if (!std::isfinite(delta) ||
        !std::isfinite(axisValue(position, axis) + delta)) {
        setAxisValue(velocity, axis, 0.0f);
        return;
    }
    if (delta == 0.0f) {
        return;
    }

    const size_t movementIndex = axisIndex(axis);
    const Aabb start = localBounds.translated(position);
    Aabb swept = start;
    if (delta > 0.0f) {
        swept.max[movementIndex] += delta;
    } else {
        swept.min[movementIndex] += delta;
    }

    float allowedDelta = delta;
    bool hit = false;
    const bool queryAccepted = world.forEachCollisionBox(
        collisionBox(swept),
        [&](const Voxel::BlockCollisionBox& block) {
            if (!overlapsOnOtherAxes(start, block, axis)) return;

            if (delta > 0.0f) {
                const float startFace = start.max[movementIndex];
                const float blockFace = block.min[movementIndex];
                if (startFace >
                    blockFace + Voxel::BlockCollisionContactTolerance) {
                    return;
                }
                if (startFace + delta <
                    blockFace - Voxel::BlockCollisionContactTolerance) {
                    return;
                }
                const float candidate = std::max(
                    0.0f,
                    blockFace - startFace -
                        Voxel::BlockCollisionContactTolerance);
                if (!hit || candidate < allowedDelta) {
                    allowedDelta = candidate;
                }
            } else {
                const float startFace = start.min[movementIndex];
                const float blockFace = block.max[movementIndex];
                if (startFace <
                    blockFace - Voxel::BlockCollisionContactTolerance) {
                    return;
                }
                if (startFace + delta >
                    blockFace + Voxel::BlockCollisionContactTolerance) {
                    return;
                }
                const float candidate = std::min(
                    0.0f,
                    blockFace - startFace +
                        Voxel::BlockCollisionContactTolerance);
                if (!hit || candidate > allowedDelta) {
                    allowedDelta = candidate;
                }
            }
            hit = true;
        });

    if (!queryAccepted) {
        setAxisValue(velocity, axis, 0.0f);
        return;
    }

    setAxisValue(
        position,
        axis,
        axisValue(position, axis) + allowedDelta);

    if (hit) {
        collided = true;
        if (axis == Axis::Y && delta < 0.0f) {
            onGround = true;
        }
        setAxisValue(velocity, axis, 0.0f);
    }
}

bool isSupported(Voxel::World& world, const Aabb& box) {
    Voxel::BlockCollisionBox probe = collisionBox(box);
    probe.min[1] -= Voxel::BlockCollisionContactTolerance * 2.0f;

    bool supported = false;
    const bool queryAccepted = world.forEachCollisionBox(
        probe,
        [&](const Voxel::BlockCollisionBox& block) {
            if (supported || !overlapsOnOtherAxes(box, block, Axis::Y)) {
                return;
            }
            const float distance = box.min.y - block.max[1];
            supported =
                distance >= -Voxel::BlockCollisionContactTolerance &&
                distance <= Voxel::BlockCollisionContactTolerance * 2.0f;
        });
    return queryAccepted && supported;
}

} // namespace

Entity::Entity(std::string typeId)
    : m_id(EntityId::New())
    , m_typeId(std::move(typeId))
{
    updateWorldBounds();
}

void Entity::setPosition(const glm::vec3& pos) {
    m_position = pos;
    updateWorldBounds();
}

void Entity::setPosition(float x, float y, float z) {
    setPosition(glm::vec3(x, y, z));
}

void Entity::setLocalBounds(const Aabb& bounds) {
    m_localBounds = bounds;
    updateWorldBounds();
}

const Aabb& Entity::defaultLocalBounds() {
    static const Aabb bounds{glm::vec3(-0.5f), glm::vec3(0.5f)};
    return bounds;
}

void Entity::render(const EntityRenderContext& ctx,
                    const glm::mat4& modelMatrix,
                    bool shouldRender) {
    if (m_modelInstance) {
        m_modelInstance->setTint(m_renderTint);
        m_modelInstance->render(ctx, *this, modelMatrix, shouldRender);
    }
}

void Entity::setModel(Asset::Handle<EntityModelAsset> model) {
    m_modelIdentifier = model.id();
    m_model = std::move(model);
    m_modelInstance.reset();
    if (m_model && m_model->hitbox) {
        setLocalBounds(*m_model->hitbox);
    } else {
        setLocalBounds(defaultLocalBounds());
    }
}

void Entity::setModelIdentifier(std::string identifier) {
    m_model = {};
    m_modelIdentifier = std::move(identifier);
    m_modelInstance.reset();
    setLocalBounds(defaultLocalBounds());
}

void Entity::clearModelInstance() {
    m_modelInstance.reset();
}

bool Entity::ensureModelInstance(Asset::AssetManager& assets,
                                 const Asset::Handle<Asset::ShaderAsset>& shader) {
    if (m_modelInstance) {
        return true;
    }
    if (!m_model) {
        return false;
    }
    m_modelInstance = m_model->createInstance(assets, shader);
    return m_modelInstance != nullptr;
}

void Entity::applyFloorFriction(float friction) {
    m_floorFriction = friction;
    applyFriction(friction, m_velocity);
}

void Entity::updateWorldBounds() {
    m_worldBounds = m_localBounds.translated(m_position);
}

void Entity::resolveCollisions(Voxel::World& world, float dt) {
    if (isNoClip()) {
        m_position += m_velocity * dt;
        updateWorldBounds();
        return;
    }

    m_collidedX = false;
    m_collidedY = false;
    m_collidedZ = false;
    m_onGround = false;

    resolveAxis(world, m_localBounds, m_position, m_velocity, Axis::X, dt, m_collidedX, m_onGround);
    resolveAxis(world, m_localBounds, m_position, m_velocity, Axis::Y, dt, m_collidedY, m_onGround);
    resolveAxis(world, m_localBounds, m_position, m_velocity, Axis::Z, dt, m_collidedZ, m_onGround);

    if (!m_onGround) {
        if (isSupported(world, m_localBounds.translated(m_position))) {
            m_onGround = true;
        }
    }

    updateWorldBounds();
}

void Entity::update(Voxel::World& world, float dt) {
    if (!std::isfinite(dt) || dt <= 0.0f) {
        return;
    }

    if (glm::dot(m_viewDirection, m_viewDirection) < 1.0e-6f) {
        m_viewDirection = glm::vec3(0.0f, 0.0f, -1.0f);
    }

    if (!isNoClip()) {
        m_acceleration += glm::vec3(0.0f, -29.4f, 0.0f) * m_gravityModifier;
    }

    m_velocity += m_acceleration * dt;
    resolveCollisions(world, dt);

    if (m_onGround) {
        applyFloorFriction(m_floorFriction);
    }

    m_acceleration = glm::vec3(0.0f);
}

} // namespace Rigel::Entity
