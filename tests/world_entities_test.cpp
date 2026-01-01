#include "TestFramework.h"

#include "Rigel/Entity/WorldEntities.h"
#include "Rigel/Voxel/World.h"
#include "Rigel/Voxel/WorldResources.h"
#include "Rigel/Voxel/BlockType.h"

using namespace Rigel::Entity;
using namespace Rigel::Voxel;

TEST_CASE(WorldEntities_SpawnDespawn) {
    WorldResources resources;
    World world(resources);

    BlockType solid;
    solid.identifier = "rigel:stone";
    resources.registry().registerBlock(solid.identifier, solid);

    auto entity = std::make_unique<Entity>("rigel:test_entity");
    entity->setPosition(0.0f, 0.0f, 0.0f);
    EntityId id = world.entities().spawn(std::move(entity));

    CHECK(!id.isNull());
    CHECK_EQ(world.entities().size(), static_cast<size_t>(1));
    CHECK(world.entities().get(id) != nullptr);
    CHECK(world.entities().get(id)->currentChunk() != nullptr);

    CHECK(world.entities().despawn(id));
    CHECK_EQ(world.entities().size(), static_cast<size_t>(0));
}
