#include "Rigel/Entity/Entity.h"

#include "Rigel/Entity/EntityUtils.h"
#include "Rigel/Voxel/World.h"

#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>

namespace Rigel::Entity {

namespace {
constexpr float kEpsilon = 1.0e-4f;

bool isSolidAt(Voxel::World& world, int x, int y, int z) {
    Voxel::BlockState state = world.getBlock(x, y, z);
    if (state.isAir()) {
        return false;
    }
    return world.blockRegistry().getType(state.id).isSolid;
}

struct AxisRange {
    int min = 0;
    int max = 0;
};

AxisRange toBlockRange(float minCoord, float maxCoord) {
    int minBlock = static_cast<int>(std::floor(minCoord));
    int maxBlock = static_cast<int>(std::floor(maxCoord - kEpsilon));
    if (maxBlock < minBlock) {
        maxBlock = minBlock;
    }
    return {minBlock, maxBlock};
}

bool intersectsSolidBlocks(Voxel::World& world, const Aabb& box) {
    AxisRange xRange = toBlockRange(box.min.x, box.max.x);
    AxisRange yRange = toBlockRange(box.min.y, box.max.y);
    AxisRange zRange = toBlockRange(box.min.z, box.max.z);

    for (int bx = xRange.min; bx <= xRange.max; ++bx) {
        for (int by = yRange.min; by <= yRange.max; ++by) {
            for (int bz = zRange.min; bz <= zRange.max; ++bz) {
                if (!isSolidAt(world, bx, by, bz)) {
                    continue;
                }
                Aabb block{
                    glm::vec3(static_cast<float>(bx),
                              static_cast<float>(by),
                              static_cast<float>(bz)),
                    glm::vec3(static_cast<float>(bx + 1),
                              static_cast<float>(by + 1),
                              static_cast<float>(bz + 1))
                };
                if (box.intersects(block)) {
                    return true;
                }
            }
        }
    }
    return false;
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

void resolveAxis(Voxel::World& world,
                 const Aabb& localBounds,
                 glm::vec3& position,
                 glm::vec3& velocity,
                 Axis axis,
                 float dt,
                 bool& collided,
                 bool& onGround) {
    float delta = axisValue(velocity, axis) * dt;
    if (delta == 0.0f) {
        return;
    }

    glm::vec3 testPos = position;
    setAxisValue(testPos, axis, axisValue(position, axis) + delta);
    Aabb moved = localBounds.translated(testPos);

    AxisRange xRange = toBlockRange(moved.min.x, moved.max.x);
    AxisRange yRange = toBlockRange(moved.min.y, moved.max.y);
    AxisRange zRange = toBlockRange(moved.min.z, moved.max.z);

    bool hit = false;
    float resolved = axisValue(testPos, axis);
    if (delta > 0.0f) {
        float maxAllowed = resolved;
        for (int bx = xRange.min; bx <= xRange.max; ++bx) {
            for (int by = yRange.min; by <= yRange.max; ++by) {
                for (int bz = zRange.min; bz <= zRange.max; ++bz) {
                    if (!isSolidAt(world, bx, by, bz)) {
                        continue;
                    }
                    Aabb block{
                        glm::vec3(static_cast<float>(bx),
                                  static_cast<float>(by),
                                  static_cast<float>(bz)),
                        glm::vec3(static_cast<float>(bx + 1),
                                  static_cast<float>(by + 1),
                                  static_cast<float>(bz + 1))
                    };
                    if (!moved.intersects(block)) {
                        continue;
                    }
                    float candidate = 0.0f;
                    switch (axis) {
                        case Axis::X:
                            candidate = block.min.x - localBounds.max.x - kEpsilon;
                            break;
                        case Axis::Y:
                            candidate = block.min.y - localBounds.max.y - kEpsilon;
                            break;
                        case Axis::Z:
                            candidate = block.min.z - localBounds.max.z - kEpsilon;
                            break;
                    }
                    maxAllowed = std::min(maxAllowed, candidate);
                    hit = true;
                }
            }
        }
        if (hit) {
            setAxisValue(position, axis, maxAllowed);
        } else {
            setAxisValue(position, axis, resolved);
        }
    } else {
        float minAllowed = resolved;
        for (int bx = xRange.min; bx <= xRange.max; ++bx) {
            for (int by = yRange.min; by <= yRange.max; ++by) {
                for (int bz = zRange.min; bz <= zRange.max; ++bz) {
                    if (!isSolidAt(world, bx, by, bz)) {
                        continue;
                    }
                    Aabb block{
                        glm::vec3(static_cast<float>(bx),
                                  static_cast<float>(by),
                                  static_cast<float>(bz)),
                        glm::vec3(static_cast<float>(bx + 1),
                                  static_cast<float>(by + 1),
                                  static_cast<float>(bz + 1))
                    };
                    if (!moved.intersects(block)) {
                        continue;
                    }
                    float candidate = 0.0f;
                    switch (axis) {
                        case Axis::X:
                            candidate = block.max.x - localBounds.min.x + kEpsilon;
                            break;
                        case Axis::Y:
                            candidate = block.max.y - localBounds.min.y + kEpsilon;
                            break;
                        case Axis::Z:
                            candidate = block.max.z - localBounds.min.z + kEpsilon;
                            break;
                    }
                    minAllowed = std::max(minAllowed, candidate);
                    hit = true;
                }
            }
        }
        if (hit) {
            setAxisValue(position, axis, minAllowed);
        } else {
            setAxisValue(position, axis, resolved);
        }
    }

    if (hit) {
        collided = true;
        if (axis == Axis::Y && delta < 0.0f) {
            onGround = true;
        }
        setAxisValue(velocity, axis, 0.0f);
    }
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
    m_lastPosition = pos;
    updateWorldBounds();
}

void Entity::setPosition(float x, float y, float z) {
    setPosition(glm::vec3(x, y, z));
}

void Entity::setLocalBounds(const Aabb& bounds) {
    m_localBounds = bounds;
    updateWorldBounds();
}

void Entity::addUpdateComponent(IUpdateEntityComponent* component) {
    if (!component) {
        return;
    }
    if (std::find(m_updateComponents.begin(), m_updateComponents.end(), component) ==
        m_updateComponents.end()) {
        m_updateComponents.push_back(component);
    }
}

void Entity::removeUpdateComponent(IUpdateEntityComponent* component) {
    auto it = std::remove(m_updateComponents.begin(), m_updateComponents.end(), component);
    m_updateComponents.erase(it, m_updateComponents.end());
}

void Entity::addRenderComponent(IRenderEntityComponent* component) {
    if (!component) {
        return;
    }
    if (std::find(m_renderComponents.begin(), m_renderComponents.end(), component) ==
        m_renderComponents.end()) {
        m_renderComponents.push_back(component);
    }
}

void Entity::removeRenderComponent(IRenderEntityComponent* component) {
    auto it = std::remove(m_renderComponents.begin(), m_renderComponents.end(), component);
    m_renderComponents.erase(it, m_renderComponents.end());
}

void Entity::render(const EntityRenderContext& ctx,
                    const glm::mat4& modelMatrix,
                    bool shouldRender) {
    if (m_modelInstance) {
        m_modelInstance->setTint(m_renderTint);
        m_modelInstance->render(ctx, *this, modelMatrix, shouldRender);
    }

    for (IRenderEntityComponent* component : m_renderComponents) {
        if (component) {
            component->render(*this, ctx, modelMatrix, shouldRender);
        }
    }
}

void Entity::setModel(Asset::Handle<EntityModelAsset> model) {
    m_model = std::move(model);
    m_modelInstance.reset();
    if (m_model && m_model->hitbox) {
        setLocalBounds(*m_model->hitbox);
    }
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
        Aabb probe = m_localBounds.translated(m_position + glm::vec3(0.0f, -kEpsilon * 2.0f, 0.0f));
        if (intersectsSolidBlocks(world, probe)) {
            m_onGround = true;
        }
    }

    updateWorldBounds();
}

void Entity::update(Voxel::World& world, float dt) {
    if (dt <= 0.0f) {
        return;
    }

    m_age += dt;
    if (glm::dot(m_viewDirection, m_viewDirection) < 1.0e-6f) {
        m_viewDirection = glm::vec3(0.0f, 0.0f, -1.0f);
    }

    for (IUpdateEntityComponent* component : m_updateComponents) {
        component->update(world, *this, dt);
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
