#pragma once

namespace Rigel::Voxel {
class World;
}

namespace Rigel::Entity {

class Entity;

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
    virtual void render(Entity& entity) {
        (void)entity;
    }
};

} // namespace Rigel::Entity
