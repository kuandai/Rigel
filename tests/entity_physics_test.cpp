#include "TestFramework.h"

#include "Rigel/Entity/Entity.h"
#include "Rigel/Voxel/World.h"
#include "Rigel/Voxel/WorldResources.h"
#include "Rigel/Voxel/BlockType.h"

using namespace Rigel::Entity;
using namespace Rigel::Voxel;

namespace {

class GravityFreeEntity : public Entity {
public:
    GravityFreeEntity() : Entity("rigel:test_entity") {
        m_gravityModifier = 0.0f;
    }
};

BlockID placeBlock(
    WorldResources& resources,
    World& world,
    const std::string& identifier,
    BlockCollisionShape collision,
    int x,
    int y,
    int z
) {
    BlockType type;
    type.identifier = identifier;
    type.collision = std::move(collision);
    const BlockID id = resources.registry().registerBlock(identifier, type);
    world.setBlock(x, y, z, BlockState{id});
    return id;
}

} // namespace

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

TEST_CASE(EntityPhysics_SweepsAgainstPartialCollisionBoxes) {
    WorldResources resources;
    World world(resources);
    placeBlock(
        resources,
        world,
        "rigel:partial",
        BlockCollisionShape::boxes({
            {{0.25f, 0.0f, 0.0f}, {0.5f, 1.0f, 1.0f}},
        }),
        2,
        0,
        0);

    GravityFreeEntity entity;
    entity.setLocalBounds(
        Aabb{glm::vec3(-0.25f), glm::vec3(0.25f)});
    entity.setPosition(0.0f, 0.5f, 0.5f);
    entity.setVelocity(glm::vec3(10.0f, 0.0f, 0.0f));

    entity.update(world, 0.5f);

    CHECK(entity.collidedX());
    CHECK_NEAR(
        entity.position().x,
        2.0f - BlockCollisionContactTolerance,
        0.00001f);
    CHECK_EQ(entity.velocity().x, 0.0f);
}

TEST_CASE(EntityPhysics_UsesNearestOverhangingBox) {
    WorldResources resources;
    World world(resources);
    placeBlock(
        resources,
        world,
        "rigel:overhang",
        BlockCollisionShape::boxes({
            {{0.75f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}},
            {{-0.25f, 0.0f, 0.0f}, {0.0f, 1.0f, 1.0f}},
        }),
        2,
        0,
        0);

    GravityFreeEntity entity;
    entity.setLocalBounds(
        Aabb{glm::vec3(-0.25f), glm::vec3(0.25f)});
    entity.setPosition(0.0f, 0.5f, 0.5f);
    entity.setVelocity(glm::vec3(10.0f, 0.0f, 0.0f));

    entity.update(world, 0.5f);

    CHECK(entity.collidedX());
    CHECK_NEAR(
        entity.position().x,
        1.5f - BlockCollisionContactTolerance,
        0.00001f);
}

TEST_CASE(EntityPhysics_ExplicitEmptyShapeDoesNotCollide) {
    WorldResources resources;
    World world(resources);
    placeBlock(
        resources,
        world,
        "rigel:empty_collision",
        BlockCollisionShape::empty(),
        2,
        0,
        0);

    GravityFreeEntity entity;
    entity.setLocalBounds(
        Aabb{glm::vec3(-0.25f), glm::vec3(0.25f)});
    entity.setPosition(0.0f, 0.5f, 0.5f);
    entity.setVelocity(glm::vec3(10.0f, 0.0f, 0.0f));

    entity.update(world, 0.5f);

    CHECK(!entity.collidedX());
    CHECK_EQ(entity.position().x, 5.0f);
}

TEST_CASE(EntityPhysics_PartialFloorSetsGroundContact) {
    WorldResources resources;
    World world(resources);
    placeBlock(
        resources,
        world,
        "rigel:slab",
        BlockCollisionShape::boxes({
            {{0.0f, 0.0f, 0.0f}, {1.0f, 0.5f, 1.0f}},
        }),
        0,
        0,
        0);

    GravityFreeEntity entity;
    entity.setLocalBounds(
        Aabb{glm::vec3(-0.4f), glm::vec3(0.4f)});
    entity.setPosition(0.5f, 2.0f, 0.5f);
    entity.setVelocity(glm::vec3(0.0f, -10.0f, 0.0f));

    entity.update(world, 0.5f);

    CHECK(entity.collidedY());
    CHECK(entity.isOnGround());
    CHECK_NEAR(
        entity.position().y,
        0.9f + BlockCollisionContactTolerance,
        0.00001f);
}

TEST_CASE(EntityPhysics_StationaryInitialOverlapIsNotDepenetrated) {
    WorldResources resources;
    World world(resources);
    placeBlock(
        resources,
        world,
        "rigel:overlap",
        BlockCollisionShape::fullCube(),
        0,
        0,
        0);

    GravityFreeEntity entity;
    entity.setLocalBounds(
        Aabb{glm::vec3(-0.25f), glm::vec3(0.25f)});
    entity.setPosition(0.5f, 0.5f, 0.5f);

    entity.update(world, 0.5f);

    CHECK_EQ(entity.position(), glm::vec3(0.5f));
    CHECK(!entity.collidedX());
    CHECK(!entity.collidedY());
    CHECK(!entity.collidedZ());
}
