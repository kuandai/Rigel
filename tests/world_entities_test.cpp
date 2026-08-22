#include "TestFramework.h"

#include "Rigel/Entity/WorldEntities.h"
#include "Rigel/Voxel/World.h"
#include "Rigel/Voxel/WorldResources.h"
#include "Rigel/Voxel/BlockType.h"

using namespace Rigel::Entity;
using namespace Rigel::Voxel;

namespace Rigel::Entity::detail {
struct WorldEntitiesTestAccess {
    static size_t chunkCount(const WorldEntities& entities) {
        return entities.m_chunkIndex.size();
    }

    static size_t regionCount(const WorldEntities& entities) {
        return entities.m_regions.size();
    }

    static EntityChunk* findChunk(const WorldEntities& entities,
                                  Voxel::ChunkCoord coord) {
        return entities.findChunk(coord);
    }
};
}

namespace {

class DestructionProbeEntity final : public Entity {
public:
    explicit DestructionProbeEntity(bool& detached)
        : Entity("rigel:destruction_probe"), m_detached(detached) {}

    ~DestructionProbeEntity() override {
        m_detached = currentChunk() == nullptr;
    }

private:
    bool& m_detached;
};

class DespawnDuringUpdateEntity final : public Entity {
public:
    DespawnDuringUpdateEntity(bool& requested, bool& detachedBeforeDestruction)
        : Entity("rigel:tick_despawn")
        , m_requested(requested)
        , m_detachedBeforeDestruction(detachedBeforeDestruction) {}

    ~DespawnDuringUpdateEntity() override {
        m_detachedBeforeDestruction = currentChunk() == nullptr;
    }

    void update(World& world, float) override {
        m_requested = world.entities().despawn(id());
    }

private:
    bool& m_requested;
    bool& m_detachedBeforeDestruction;
};

}

TEST_CASE(WorldEntities_SpawnDespawn) {
    bool detachedBeforeDestruction = false;
    WorldResources resources;
    World world(resources);

    BlockType solid;
    solid.identifier = "rigel:stone";
    resources.registry().registerBlock(solid.identifier, solid);

    auto entity = std::make_unique<DestructionProbeEntity>(
        detachedBeforeDestruction);
    entity->setPosition(0.0f, 0.0f, 0.0f);
    EntityId id = world.entities().spawn(std::move(entity));

    CHECK(!id.isNull());
    CHECK_EQ(world.entities().size(), static_cast<size_t>(1));
    CHECK(world.entities().get(id) != nullptr);
    CHECK(world.entities().get(id)->currentChunk() != nullptr);

    CHECK(world.entities().despawn(id));
    CHECK(detachedBeforeDestruction);
    CHECK_EQ(world.entities().size(), static_cast<size_t>(0));
    CHECK_EQ(detail::WorldEntitiesTestAccess::chunkCount(world.entities()),
             static_cast<size_t>(0));
    CHECK_EQ(detail::WorldEntitiesTestAccess::regionCount(world.entities()),
             static_cast<size_t>(0));
}

TEST_CASE(WorldEntities_MovePrunesPreviousChunk) {
    WorldResources resources;
    World world(resources);

    auto entity = std::make_unique<Entity>("rigel:moving_entity");
    Entity* moving = entity.get();
    entity->setPosition(0.0f, 0.0f, 0.0f);
    const EntityId id = world.entities().spawn(std::move(entity));
    CHECK(!id.isNull());

    const ChunkCoord oldCoord{0, 0, 0};
    moving->setPosition(static_cast<float>(ChunkSize), 0.0f, 0.0f);
    world.entities().updateEntityChunk(*moving);

    CHECK(moving->currentChunk() != nullptr);
    CHECK_EQ(moving->currentChunk()->coord(), (ChunkCoord{1, 0, 0}));
    CHECK_EQ(detail::WorldEntitiesTestAccess::findChunk(world.entities(), oldCoord),
             nullptr);
    CHECK_EQ(detail::WorldEntitiesTestAccess::chunkCount(world.entities()),
             static_cast<size_t>(1));
    CHECK_EQ(detail::WorldEntitiesTestAccess::regionCount(world.entities()),
             static_cast<size_t>(1));
}

TEST_CASE(WorldEntities_DespawnDuringTickPrunesOwnership) {
    bool requested = false;
    bool detachedBeforeDestruction = false;
    WorldResources resources;
    World world(resources);

    auto entity = std::make_unique<DespawnDuringUpdateEntity>(
        requested, detachedBeforeDestruction);
    const EntityId id = world.entities().spawn(std::move(entity));
    CHECK(!id.isNull());

    world.tickEntities(1.0f);

    CHECK(requested);
    CHECK(detachedBeforeDestruction);
    CHECK_EQ(world.entities().get(id), nullptr);
    CHECK_EQ(detail::WorldEntitiesTestAccess::chunkCount(world.entities()),
             static_cast<size_t>(0));
    CHECK_EQ(detail::WorldEntitiesTestAccess::regionCount(world.entities()),
             static_cast<size_t>(0));
}

TEST_CASE(WorldEntities_OwnerDestructionDetachesEntity) {
    bool detachedBeforeDestruction = false;
    WorldResources resources;
    {
        World world(resources);
        auto entity = std::make_unique<DestructionProbeEntity>(
            detachedBeforeDestruction);
        CHECK(!world.entities().spawn(std::move(entity)).isNull());
    }

    CHECK(detachedBeforeDestruction);
}

TEST_CASE(WorldEntities_RepeatedTravelKeepsBucketCountsBounded) {
    WorldResources resources;
    World world(resources);

    auto entity = std::make_unique<Entity>("rigel:traveler");
    Entity* traveler = entity.get();
    const EntityId id = world.entities().spawn(std::move(entity));
    CHECK(!id.isNull());

    constexpr int travelSteps = 256;
    for (int i = 1; i <= travelSteps; ++i) {
        const int direction = i % 2 == 0 ? 1 : -1;
        const ChunkCoord coord{
            direction * i * EntityRegionChunkSpan, i % 5, -(i % 7)};
        traveler->setPosition(coord.toWorldCenter());
        world.entities().updateEntityChunk(*traveler);

        CHECK_EQ(traveler->currentChunk()->coord(), coord);
        CHECK_EQ(detail::WorldEntitiesTestAccess::chunkCount(world.entities()),
                 static_cast<size_t>(1));
        CHECK_EQ(detail::WorldEntitiesTestAccess::regionCount(world.entities()),
                 static_cast<size_t>(1));
    }

    CHECK(world.entities().despawn(id));
    CHECK_EQ(detail::WorldEntitiesTestAccess::chunkCount(world.entities()),
             static_cast<size_t>(0));
    CHECK_EQ(detail::WorldEntitiesTestAccess::regionCount(world.entities()),
             static_cast<size_t>(0));
}
