#include "TestFramework.h"

#include "Rigel/Entity/Entity.h"
#include "Rigel/Voxel/World.h"
#include "Rigel/Voxel/WorldResources.h"
#include "Rigel/Voxel/BlockType.h"

#include <array>
#include <cmath>
#include <limits>

#include <glm/vec2.hpp>

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

TEST_CASE(EntityPhysics_LandsOnSlabAndMultiBoxSurfaceHeights) {
    WorldResources resources;
    World world(resources);
    placeBlock(
        resources,
        world,
        "rigel:bottom_slab",
        BlockCollisionShape::boxes({
            {{0.0f, 0.0f, 0.0f}, {1.0f, 0.5f, 1.0f}},
        }),
        0,
        0,
        0);
    placeBlock(
        resources,
        world,
        "rigel:top_slab",
        BlockCollisionShape::boxes({
            {{0.0f, 0.5f, 0.0f}, {1.0f, 1.0f, 1.0f}},
        }),
        2,
        0,
        0);
    placeBlock(
        resources,
        world,
        "rigel:two_level_surface",
        BlockCollisionShape::boxes({
            {{0.0f, 0.0f, 0.0f}, {1.0f, 0.5f, 1.0f}},
            {{0.5f, 0.5f, 0.0f}, {1.0f, 1.0f, 1.0f}},
        }),
        4,
        0,
        0);

    const auto fall = [&](float x) {
        GravityFreeEntity entity;
        entity.setLocalBounds(
            Aabb{glm::vec3(-0.2f), glm::vec3(0.2f)});
        entity.setPosition(x, 3.0f, 0.5f);
        entity.setVelocity(glm::vec3(0.0f, -10.0f, 0.0f));
        entity.update(world, 0.5f);
        CHECK(entity.collidedY());
        CHECK(entity.isOnGround());
        return entity.position().y;
    };

    CHECK_NEAR(
        fall(0.5f),
        0.7f + BlockCollisionContactTolerance,
        0.00001f);
    CHECK_NEAR(
        fall(2.5f),
        1.2f + BlockCollisionContactTolerance,
        0.00001f);
    CHECK_NEAR(
        fall(4.25f),
        0.7f + BlockCollisionContactTolerance,
        0.00001f);
    CHECK_NEAR(
        fall(4.75f),
        1.2f + BlockCollisionContactTolerance,
        0.00001f);
}

TEST_CASE(EntityPhysics_RotatedStairContactsDependOnFootprint) {
    struct StairCase {
        const char* identifier;
        BlockCollisionBox upper;
        glm::vec2 upperFootprint;
        glm::vec2 lowerFootprint;
    };
    const std::array cases = {
        StairCase{
            "rigel:stair_pos_x",
            {{0.5f, 0.5f, 0.0f}, {1.0f, 1.0f, 1.0f}},
            {0.75f, 0.5f},
            {0.25f, 0.5f}},
        StairCase{
            "rigel:stair_neg_x",
            {{0.0f, 0.5f, 0.0f}, {0.5f, 1.0f, 1.0f}},
            {0.25f, 0.5f},
            {0.75f, 0.5f}},
        StairCase{
            "rigel:stair_pos_z",
            {{0.0f, 0.5f, 0.5f}, {1.0f, 1.0f, 1.0f}},
            {0.5f, 0.75f},
            {0.5f, 0.25f}},
        StairCase{
            "rigel:stair_neg_z",
            {{0.0f, 0.5f, 0.0f}, {1.0f, 1.0f, 0.5f}},
            {0.5f, 0.25f},
            {0.5f, 0.75f}},
    };

    for (const StairCase& stair : cases) {
        WorldResources resources;
        World world(resources);
        placeBlock(
            resources,
            world,
            stair.identifier,
            BlockCollisionShape::boxes({
                stair.upper,
                {{0.0f, 0.0f, 0.0f}, {1.0f, 0.5f, 1.0f}},
            }),
            0,
            0,
            0);

        const auto fall = [&](const glm::vec2& footprint) {
            GravityFreeEntity entity;
            entity.setLocalBounds(
                Aabb{glm::vec3(-0.1f, -0.2f, -0.1f),
                     glm::vec3(0.1f, 0.2f, 0.1f)});
            entity.setPosition(footprint.x, 3.0f, footprint.y);
            entity.setVelocity(glm::vec3(0.0f, -10.0f, 0.0f));
            entity.update(world, 0.5f);
            CHECK(entity.collidedY());
            CHECK(entity.isOnGround());
            return entity.position().y;
        };

        CHECK_NEAR(
            fall(stair.upperFootprint),
            1.2f + BlockCollisionContactTolerance,
            0.00001f);
        CHECK_NEAR(
            fall(stair.lowerFootprint),
            0.7f + BlockCollisionContactTolerance,
            0.00001f);
    }
}

TEST_CASE(EntityPhysics_HorizontalStairContactDoesNotStepUp) {
    WorldResources resources;
    World world(resources);
    placeBlock(
        resources,
        world,
        "rigel:static_stair",
        BlockCollisionShape::boxes({
            {{0.5f, 0.5f, 0.0f}, {1.0f, 1.0f, 1.0f}},
            {{0.0f, 0.0f, 0.0f}, {1.0f, 0.5f, 1.0f}},
        }),
        1,
        0,
        0);

    GravityFreeEntity entity;
    entity.setLocalBounds(
        Aabb{glm::vec3(-0.25f), glm::vec3(0.25f)});
    entity.setPosition(0.5f, 0.75f, 0.5f);
    entity.setVelocity(glm::vec3(2.0f, 0.0f, 0.0f));

    entity.update(world, 1.0f);

    CHECK(entity.collidedX());
    CHECK(!entity.collidedY());
    CHECK_NEAR(
        entity.position().x,
        1.25f - BlockCollisionContactTolerance,
        0.00001f);
    CHECK_EQ(entity.position().y, 0.75f);
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

TEST_CASE(EntityPhysics_SweepsFullCubesInBothDirections) {
    WorldResources resources;
    World world(resources);
    placeBlock(
        resources,
        world,
        "rigel:positive_cube",
        BlockCollisionShape::fullCube(),
        2,
        0,
        0);
    placeBlock(
        resources,
        world,
        "rigel:negative_cube",
        BlockCollisionShape::fullCube(),
        -3,
        0,
        0);

    GravityFreeEntity positive;
    positive.setLocalBounds(
        Aabb{glm::vec3(-0.25f), glm::vec3(0.25f)});
    positive.setPosition(0.0f, 0.5f, 0.5f);
    positive.setVelocity(glm::vec3(10.0f, 0.0f, 0.0f));

    GravityFreeEntity negative;
    negative.setLocalBounds(
        Aabb{glm::vec3(-0.25f), glm::vec3(0.25f)});
    negative.setPosition(0.0f, 0.5f, 0.5f);
    negative.setVelocity(glm::vec3(-10.0f, 0.0f, 0.0f));

    positive.update(world, 1.0f);
    negative.update(world, 1.0f);

    CHECK(positive.collidedX());
    CHECK_NEAR(
        positive.position().x,
        1.75f - BlockCollisionContactTolerance,
        0.00001f);
    CHECK_EQ(positive.velocity().x, 0.0f);
    CHECK(negative.collidedX());
    CHECK_NEAR(
        negative.position().x,
        -1.75f + BlockCollisionContactTolerance,
        0.00001f);
    CHECK_EQ(negative.velocity().x, 0.0f);
}

TEST_CASE(EntityPhysics_UsesNearestOfMultipleBoxesAndCrossedBlocks) {
    WorldResources resources;
    World world(resources);
    const BlockID obstacle = placeBlock(
        resources,
        world,
        "rigel:multiple_crossed",
        BlockCollisionShape::boxes({
            {{0.1f, 0.0f, 0.0f}, {0.2f, 1.0f, 1.0f}},
            {{0.7f, 0.0f, 0.0f}, {0.8f, 1.0f, 1.0f}},
        }),
        -8,
        0,
        0);
    world.setBlock(-5, 0, 0, BlockState{obstacle});
    world.setBlock(-3, 0, 0, BlockState{obstacle});

    GravityFreeEntity entity;
    entity.setLocalBounds(
        Aabb{glm::vec3(-0.25f), glm::vec3(0.25f)});
    entity.setPosition(0.0f, 0.5f, 0.5f);
    entity.setVelocity(glm::vec3(-10.0f, 0.0f, 0.0f));

    entity.update(world, 1.0f);

    CHECK(entity.collidedX());
    CHECK_NEAR(
        entity.position().x,
        -1.95f + BlockCollisionContactTolerance,
        0.00001f);
    CHECK_EQ(entity.velocity().x, 0.0f);
}

TEST_CASE(EntityPhysics_SweepsThinBoxesAtRepresentativeDistances) {
    WorldResources resources;
    World world(resources);
    placeBlock(
        resources,
        world,
        "rigel:quarter_cell_thin",
        BlockCollisionShape::boxes({
            {{0.15f, 0.0f, 0.0f}, {0.2f, 1.0f, 1.0f}},
        }),
        0,
        0,
        0);
    placeBlock(
        resources,
        world,
        "rigel:two_cell_thin",
        BlockCollisionShape::boxes({
            {{0.25f, 0.0f, 0.0f}, {0.3f, 1.0f, 1.0f}},
        }),
        1,
        0,
        2);
    placeBlock(
        resources,
        world,
        "rigel:ten_cell_thin",
        BlockCollisionShape::boxes({
            {{0.25f, 0.0f, 0.0f}, {0.3f, 1.0f, 1.0f}},
        }),
        9,
        0,
        4);

    GravityFreeEntity quarterCell;
    quarterCell.setLocalBounds(
        Aabb{glm::vec3(-0.05f), glm::vec3(0.05f)});
    quarterCell.setPosition(0.0f, 0.5f, 0.5f);
    quarterCell.setVelocity(glm::vec3(0.25f, 0.0f, 0.0f));

    GravityFreeEntity twoCells;
    twoCells.setLocalBounds(
        Aabb{glm::vec3(-0.05f), glm::vec3(0.05f)});
    twoCells.setPosition(0.0f, 0.5f, 2.5f);
    twoCells.setVelocity(glm::vec3(2.0f, 0.0f, 0.0f));

    GravityFreeEntity tenCells;
    tenCells.setLocalBounds(
        Aabb{glm::vec3(-0.05f), glm::vec3(0.05f)});
    tenCells.setPosition(0.0f, 0.5f, 4.5f);
    tenCells.setVelocity(glm::vec3(10.0f, 0.0f, 0.0f));

    quarterCell.update(world, 1.0f);
    twoCells.update(world, 1.0f);
    tenCells.update(world, 1.0f);

    CHECK(quarterCell.collidedX());
    CHECK_NEAR(
        quarterCell.position().x,
        0.1f - BlockCollisionContactTolerance,
        0.00001f);
    CHECK(twoCells.collidedX());
    CHECK_NEAR(
        twoCells.position().x,
        1.2f - BlockCollisionContactTolerance,
        0.00001f);
    CHECK(tenCells.collidedX());
    CHECK_NEAR(
        tenCells.position().x,
        9.2f - BlockCollisionContactTolerance,
        0.00001f);
}

TEST_CASE(EntityPhysics_CeilingCollisionStopsUpwardVelocity) {
    WorldResources resources;
    World world(resources);
    placeBlock(
        resources,
        world,
        "rigel:ceiling",
        BlockCollisionShape::fullCube(),
        0,
        2,
        0);

    GravityFreeEntity entity;
    entity.setLocalBounds(
        Aabb{glm::vec3(-0.25f), glm::vec3(0.25f)});
    entity.setPosition(0.5f, 0.5f, 0.5f);
    entity.setVelocity(glm::vec3(0.0f, 4.0f, 0.0f));

    entity.update(world, 1.0f);

    CHECK(entity.collidedY());
    CHECK(!entity.isOnGround());
    CHECK_NEAR(
        entity.position().y,
        1.75f - BlockCollisionContactTolerance,
        0.00001f);
    CHECK_EQ(entity.velocity().y, 0.0f);

    const float contactPosition = entity.position().y;
    for (int iteration = 0; iteration < 4; ++iteration) {
        entity.setVelocity(glm::vec3(0.0f, 1.0f, 0.0f));
        entity.update(world, 1.0f);
        CHECK(entity.collidedY());
        CHECK(!entity.isOnGround());
        CHECK_EQ(entity.position().y, contactPosition);
    }
}

TEST_CASE(EntityPhysics_WallCollisionPreservesSlidingAxis) {
    WorldResources resources;
    World world(resources);
    placeBlock(
        resources,
        world,
        "rigel:slide_wall",
        BlockCollisionShape::fullCube(),
        1,
        0,
        0);

    GravityFreeEntity entity;
    entity.setLocalBounds(
        Aabb{glm::vec3(-0.25f), glm::vec3(0.25f)});
    entity.setPosition(0.0f, 0.5f, 0.5f);
    entity.setVelocity(glm::vec3(2.0f, 0.0f, 2.0f));

    entity.update(world, 1.0f);

    CHECK(entity.collidedX());
    CHECK(!entity.collidedZ());
    CHECK_NEAR(
        entity.position().x,
        0.75f - BlockCollisionContactTolerance,
        0.00001f);
    CHECK_EQ(entity.position().z, 2.5f);
    CHECK_EQ(entity.velocity().x, 0.0f);
    CHECK_EQ(entity.velocity().z, 2.0f);
}

TEST_CASE(EntityPhysics_DiagonalMovementResolvesXBeforeZ) {
    WorldResources resources;
    World world(resources);
    placeBlock(
        resources,
        world,
        "rigel:diagonal_corner",
        BlockCollisionShape::fullCube(),
        1,
        0,
        1);

    GravityFreeEntity entity;
    entity.setLocalBounds(
        Aabb{glm::vec3(-0.25f), glm::vec3(0.25f)});
    entity.setPosition(0.5f, 0.5f, 0.5f);
    entity.setVelocity(glm::vec3(1.0f, 0.0f, 1.0f));

    entity.update(world, 1.0f);

    CHECK(!entity.collidedX());
    CHECK(entity.collidedZ());
    CHECK_EQ(entity.position().x, 1.5f);
    CHECK_NEAR(
        entity.position().z,
        0.75f - BlockCollisionContactTolerance,
        0.00001f);
    CHECK_EQ(entity.velocity().x, 1.0f);
    CHECK_EQ(entity.velocity().z, 0.0f);
}

TEST_CASE(EntityPhysics_ContactAndGroundProbeRemainStable) {
    WorldResources resources;
    World world(resources);
    placeBlock(
        resources,
        world,
        "rigel:stable_floor",
        BlockCollisionShape::fullCube(),
        0,
        0,
        0);

    GravityFreeEntity entity;
    entity.setLocalBounds(
        Aabb{glm::vec3(-0.25f), glm::vec3(0.25f)});
    entity.setPosition(0.5f, 2.0f, 0.5f);
    entity.setVelocity(glm::vec3(0.0f, -2.0f, 0.0f));

    entity.update(world, 1.0f);
    const float contactPosition = entity.position().y;

    CHECK(entity.collidedY());
    CHECK(entity.isOnGround());
    CHECK_NEAR(
        contactPosition,
        1.25f + BlockCollisionContactTolerance,
        0.00001f);

    entity.update(world, 1.0f);

    CHECK(!entity.collidedY());
    CHECK(entity.isOnGround());
    CHECK_EQ(entity.position().y, contactPosition);

    entity.setVelocity(glm::vec3(0.0f, -1.0f, 0.0f));
    entity.update(world, 1.0f);

    CHECK(entity.collidedY());
    CHECK(entity.isOnGround());
    CHECK_EQ(entity.position().y, contactPosition);
    CHECK_EQ(entity.velocity().y, 0.0f);
}

TEST_CASE(EntityPhysics_AdjacentFloorBoxesKeepStableGroundContact) {
    WorldResources resources;
    World world(resources);
    const BlockID floor = placeBlock(
        resources,
        world,
        "rigel:adjacent_floor",
        BlockCollisionShape::fullCube(),
        0,
        0,
        0);
    world.setBlock(1, 0, 0, BlockState{floor});

    GravityFreeEntity entity;
    entity.setLocalBounds(
        Aabb{glm::vec3(-0.25f), glm::vec3(0.25f)});
    entity.setPosition(1.0f, 2.0f, 0.5f);
    entity.setVelocity(glm::vec3(0.0f, -2.0f, 0.0f));

    entity.update(world, 1.0f);
    const float contactPosition = entity.position().y;
    CHECK(entity.collidedY());
    CHECK(entity.isOnGround());

    for (int iteration = 0; iteration < 4; ++iteration) {
        entity.setVelocity(glm::vec3(0.0f, -1.0f, 0.0f));
        entity.update(world, 1.0f);
        CHECK(entity.collidedY());
        CHECK(entity.isOnGround());
        CHECK_EQ(entity.position().y, contactPosition);
    }
}

TEST_CASE(EntityPhysics_InvalidOrUnboundedMovementDoesNotCorruptPosition) {
    WorldResources resources;
    World world(resources);

    GravityFreeEntity invalidTime;
    invalidTime.setPosition(1.0f, 2.0f, 3.0f);
    invalidTime.setVelocity(glm::vec3(1.0f));
    invalidTime.update(
        world,
        std::numeric_limits<float>::quiet_NaN());

    CHECK_EQ(invalidTime.position(), glm::vec3(1.0f, 2.0f, 3.0f));
    CHECK_EQ(invalidTime.velocity(), glm::vec3(1.0f));

    GravityFreeEntity invalidDisplacement;
    invalidDisplacement.setPosition(1.0f, 2.0f, 3.0f);
    invalidDisplacement.setVelocity(glm::vec3(
        std::numeric_limits<float>::infinity(),
        0.0f,
        0.0f));
    invalidDisplacement.update(world, 1.0f);

    CHECK_EQ(
        invalidDisplacement.position(),
        glm::vec3(1.0f, 2.0f, 3.0f));
    CHECK_EQ(invalidDisplacement.velocity().x, 0.0f);
    CHECK(std::isfinite(invalidDisplacement.worldBounds().min.x));
    CHECK(std::isfinite(invalidDisplacement.worldBounds().max.x));

    GravityFreeEntity unboundedDisplacement;
    const float largePosition =
        std::numeric_limits<float>::max() * 0.5f;
    unboundedDisplacement.setPosition(largePosition, 2.0f, 3.0f);
    unboundedDisplacement.setVelocity(glm::vec3(
        std::numeric_limits<float>::max(),
        0.0f,
        0.0f));
    unboundedDisplacement.update(world, 1.0f);

    CHECK_EQ(unboundedDisplacement.position().x, largePosition);
    CHECK_EQ(unboundedDisplacement.velocity().x, 0.0f);
    CHECK(std::isfinite(unboundedDisplacement.worldBounds().min.x));
    CHECK(std::isfinite(unboundedDisplacement.worldBounds().max.x));
}

TEST_CASE(EntityPhysics_RejectsLargeFiniteSweepsInBothDirections) {
    WorldResources resources;
    World world(resources);

    GravityFreeEntity positive;
    positive.setLocalBounds(
        Aabb{glm::vec3(-0.25f), glm::vec3(0.25f)});
    positive.setPosition(1.0f, 2.0f, 3.0f);
    positive.setVelocity(glm::vec3(1.0e9f, 0.0f, 0.0f));

    GravityFreeEntity negative;
    negative.setLocalBounds(
        Aabb{glm::vec3(-0.25f), glm::vec3(0.25f)});
    negative.setPosition(-1.0f, -2.0f, -3.0f);
    negative.setVelocity(glm::vec3(-1.0e9f, 0.0f, 0.0f));

    positive.update(world, 1.0f);
    negative.update(world, 1.0f);

    CHECK_EQ(positive.position(), glm::vec3(1.0f, 2.0f, 3.0f));
    CHECK_EQ(negative.position(), glm::vec3(-1.0f, -2.0f, -3.0f));
    CHECK_EQ(positive.velocity().x, 0.0f);
    CHECK_EQ(negative.velocity().x, 0.0f);
    CHECK(!positive.collidedX());
    CHECK(!negative.collidedX());
    CHECK_EQ(
        positive.worldBounds().min,
        glm::vec3(0.75f, 1.75f, 2.75f));
    CHECK_EQ(
        positive.worldBounds().max,
        glm::vec3(1.25f, 2.25f, 3.25f));
    CHECK_EQ(
        negative.worldBounds().min,
        glm::vec3(-1.25f, -2.25f, -3.25f));
    CHECK_EQ(
        negative.worldBounds().max,
        glm::vec3(-0.75f, -1.75f, -2.75f));
}
