#pragma once

#include <glm/mat4x4.hpp>

namespace Rigel::Voxel {
class World;
}

namespace Rigel::Entity {

class Entity;
struct EntityRenderContext;

struct IEntityComponent {
    virtual ~IEntityComponent() = default;
};

struct IUpdateEntityComponent : public IEntityComponent {
    virtual void update(Voxel::World& world, Entity& entity, float dt) {
        (void)world;
        (void)entity;
        (void)dt;
    }

    virtual void clientUpdate(Voxel::World& world, Entity& entity, float dt) {
        (void)world;
        (void)entity;
        (void)dt;
    }
};

struct IRenderEntityComponent : public IEntityComponent {
    virtual void render(Entity& entity,
                        const EntityRenderContext& ctx,
                        const glm::mat4& modelMatrix,
                        bool shouldRender) {
        (void)entity;
        (void)ctx;
        (void)modelMatrix;
        (void)shouldRender;
    }
};

} // namespace Rigel::Entity
