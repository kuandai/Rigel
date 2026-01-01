#include "TestFramework.h"

#include "Rigel/Entity/Entity.h"
#include "Rigel/Voxel/World.h"
#include "Rigel/Voxel/WorldResources.h"
#include "Rigel/Voxel/BlockType.h"

using namespace Rigel::Entity;
using namespace Rigel::Voxel;

TEST_CASE(EntityPhysics_FloorCollision) {
    WorldResources resources;
    World world(resources);

    BlockType solid;
    solid.identifier = "rigel:stone";
    solid.isSolid = true;
    auto solidId = resources.registry().registerBlock(solid.identifier, solid);

    BlockState block;
    block.id = solidId;
    world.setBlock(0, 0, 0, block);

    Entity entity("rigel:test_entity");
    entity.setLocalBounds(Aabb{glm::vec3(-0.4f), glm::vec3(0.4f)});
    entity.setPosition(0.0f, 3.0f, 0.0f);

    constexpr float dt = 1.0f / 60.0f;
    for (int i = 0; i < 240; ++i) {
        entity.update(world, dt);
    }

    CHECK(entity.position().y >= 1.35f);
    CHECK(entity.position().y <= 1.45f);
}
