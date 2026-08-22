#include "TestFramework.h"

#include "Rigel/Entity/WorldEntities.h"
#include "Rigel/Voxel/World.h"
#include "Rigel/Voxel/WorldResources.h"

using namespace Rigel::Entity;
using namespace Rigel::Voxel;

namespace {

class DestructionProbeEntity final : public Entity {
public:
    explicit DestructionProbeEntity(bool& destroyed)
        : Entity("rigel:destruction_probe"), m_destroyed(destroyed) {}

    ~DestructionProbeEntity() override {
        m_destroyed = true;
    }

private:
    bool& m_destroyed;
};

class MoveDuringUpdateEntity final : public Entity {
public:
    explicit MoveDuringUpdateEntity(bool& updated)
        : Entity("rigel:moving_entity"), m_updated(updated) {}

    void update(World&, float) override {
        setPosition(1024.0f, -32.0f, 48.0f);
        m_updated = true;
    }

private:
    bool& m_updated;
};

class DespawnDuringUpdateEntity final : public Entity {
public:
    DespawnDuringUpdateEntity(bool& requested, bool& destroyed)
        : Entity("rigel:tick_despawn")
        , m_requested(requested)
        , m_destroyed(destroyed) {}

    ~DespawnDuringUpdateEntity() override {
        m_destroyed = true;
    }

    void update(World& world, float) override {
        m_requested = world.entities().despawn(id());
    }

private:
    bool& m_requested;
    bool& m_destroyed;
};

} // namespace

TEST_CASE(WorldEntities_SpawnDespawn) {
    bool destroyed = false;
    WorldResources resources;
    World world(resources);

    auto entity = std::make_unique<DestructionProbeEntity>(destroyed);
    EntityId id = world.entities().spawn(std::move(entity));

    CHECK(!id.isNull());
    CHECK_EQ(world.entities().size(), static_cast<size_t>(1));
    CHECK(world.entities().get(id) != nullptr);
    CHECK(!destroyed);

    CHECK(world.entities().despawn(id));
    CHECK(destroyed);
    CHECK_EQ(world.entities().size(), static_cast<size_t>(0));
    CHECK_EQ(world.entities().get(id), nullptr);
}

TEST_CASE(WorldEntities_MovementRemainsVisibleToIteration) {
    bool updated = false;
    WorldResources resources;
    World world(resources);

    auto entity = std::make_unique<MoveDuringUpdateEntity>(updated);
    Entity* moving = entity.get();
    const EntityId id = world.entities().spawn(std::move(entity));
    CHECK(!id.isNull());

    world.tickEntities(1.0f);

    CHECK(updated);
    CHECK_EQ(world.entities().get(id), moving);
    CHECK_EQ(moving->position(), (glm::vec3{1024.0f, -32.0f, 48.0f}));

    size_t visits = 0;
    world.entities().forEach([&](Entity& visited) {
        CHECK_EQ(&visited, moving);
        CHECK_EQ(visited.position(), moving->position());
        ++visits;
    });
    CHECK_EQ(visits, static_cast<size_t>(1));
}

TEST_CASE(WorldEntities_ConstIterationTracksLiveEntities) {
    WorldResources resources;
    World world(resources);

    const EntityId first = world.entities().spawn(
        std::make_unique<Entity>("rigel:first"));
    const EntityId second = world.entities().spawn(
        std::make_unique<Entity>("rigel:second"));
    CHECK(!first.isNull());
    CHECK(!second.isNull());
    CHECK(world.entities().despawn(first));

    size_t visits = 0;
    const WorldEntities& entities = world.entities();
    entities.forEach([&](const Entity& entity) {
        CHECK_EQ(entity.id(), second);
        ++visits;
    });
    CHECK_EQ(visits, static_cast<size_t>(1));
}

TEST_CASE(WorldEntities_DespawnDuringTickDestroysEntity) {
    bool requested = false;
    bool destroyed = false;
    WorldResources resources;
    World world(resources);

    auto entity = std::make_unique<DespawnDuringUpdateEntity>(
        requested, destroyed);
    const EntityId id = world.entities().spawn(std::move(entity));
    CHECK(!id.isNull());

    world.tickEntities(1.0f);

    CHECK(requested);
    CHECK(destroyed);
    CHECK_EQ(world.entities().get(id), nullptr);
    CHECK_EQ(world.entities().size(), static_cast<size_t>(0));
}

TEST_CASE(WorldEntities_OwnerDestructionDestroysEntity) {
    bool destroyed = false;
    WorldResources resources;
    {
        World world(resources);
        auto entity = std::make_unique<DestructionProbeEntity>(destroyed);
        CHECK(!world.entities().spawn(std::move(entity)).isNull());
    }

    CHECK(destroyed);
}
