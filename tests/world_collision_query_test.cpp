#include "TestFramework.h"

#include "Rigel/Voxel/BlockType.h"
#include "Rigel/Voxel/World.h"
#include "Rigel/Voxel/WorldResources.h"

#include <string>
#include <utility>
#include <vector>

using namespace Rigel::Voxel;

namespace {

void placeBlock(
    WorldResources& resources,
    World& world,
    const std::string& identifier,
    BlockCollisionShape collision,
    int x,
    int y,
    int z,
    uint8_t metadata = 0
) {
    BlockType type;
    type.identifier = identifier;
    type.collision = std::move(collision);
    const BlockID id = resources.registry().registerBlock(identifier, type);
    world.setBlock(x, y, z, BlockState{id, metadata});
}

std::vector<BlockCollisionBox> query(
    const World& world,
    BlockCollisionBox bounds
) {
    std::vector<BlockCollisionBox> result;
    world.forEachCollisionBox(
        bounds,
        [&](const BlockCollisionBox& box) { result.push_back(box); });
    return result;
}

} // namespace

TEST_CASE(WorldCollisionQuery_ReturnsTranslatedFullCubeWithoutLoadedNeighbors) {
    WorldResources resources;
    World world(resources);
    placeBlock(
        resources,
        world,
        "rigel:query_full_cube",
        BlockCollisionShape::fullCube(),
        2,
        3,
        4);

    const auto boxes = query(
        world,
        {{2.25f, 3.25f, 4.25f}, {2.75f, 3.75f, 4.75f}});

    CHECK_EQ(boxes.size(), static_cast<size_t>(1));
    CHECK_EQ(
        boxes.front(),
        (BlockCollisionBox{{2.0f, 3.0f, 4.0f}, {3.0f, 4.0f, 5.0f}}));
    CHECK(query(
        world,
        {{64.0f, 5.0f, 5.0f}, {65.0f, 6.0f, 6.0f}}).empty());
}

TEST_CASE(WorldCollisionQuery_VisitsPartialAndMultipleBoxesInWorldSpace) {
    WorldResources resources;
    World world(resources);
    placeBlock(
        resources,
        world,
        "rigel:query_bottom_slab",
        BlockCollisionShape::boxes({
            {{0.0f, 0.0f, 0.0f}, {1.0f, 0.5f, 1.0f}},
        }),
        0,
        2,
        0);
    placeBlock(
        resources,
        world,
        "rigel:query_top_slab",
        BlockCollisionShape::boxes({
            {{0.0f, 0.5f, 0.0f}, {1.0f, 1.0f, 1.0f}},
        }),
        2,
        2,
        0);
    placeBlock(
        resources,
        world,
        "rigel:query_multiple",
        BlockCollisionShape::boxes({
            {{0.0f, 0.0f, 0.0f}, {0.25f, 1.0f, 1.0f}},
            {{0.75f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}},
        }),
        4,
        2,
        0);

    const auto boxes = query(
        world,
        {{-0.1f, 1.9f, -0.1f}, {5.1f, 3.1f, 1.1f}});

    CHECK_EQ(boxes.size(), static_cast<size_t>(4));
    CHECK_EQ(
        boxes[0],
        (BlockCollisionBox{{0.0f, 2.0f, 0.0f}, {1.0f, 2.5f, 1.0f}}));
    CHECK_EQ(
        boxes[1],
        (BlockCollisionBox{{2.0f, 2.5f, 0.0f}, {3.0f, 3.0f, 1.0f}}));
    CHECK_EQ(
        boxes[2],
        (BlockCollisionBox{{4.0f, 2.0f, 0.0f}, {4.25f, 3.0f, 1.0f}}));
    CHECK_EQ(
        boxes[3],
        (BlockCollisionBox{{4.75f, 2.0f, 0.0f}, {5.0f, 3.0f, 1.0f}}));
}

TEST_CASE(WorldCollisionQuery_UsesPreOrientedShapeForBlockState) {
    WorldResources resources;
    World world(resources);
    placeBlock(
        resources,
        world,
        "rigel:query_pre_oriented",
        BlockCollisionShape::boxes({
            {{0.0f, 0.0f, 0.25f}, {1.0f, 1.0f, 0.5f}},
        }),
        -4,
        1,
        -2,
        7);

    const auto boxes = query(
        world,
        {{-4.1f, 0.9f, -1.8f}, {-2.9f, 2.1f, -1.4f}});

    CHECK_EQ(boxes.size(), static_cast<size_t>(1));
    CHECK_EQ(
        boxes.front(),
        (BlockCollisionBox{
            {-4.0f, 1.0f, -1.75f},
            {-3.0f, 2.0f, -1.5f},
        }));
}

TEST_CASE(WorldCollisionQuery_HandlesEmptyAdjacentAndNegativeCells) {
    WorldResources resources;
    World world(resources);
    placeBlock(
        resources,
        world,
        "rigel:query_negative_cube",
        BlockCollisionShape::fullCube(),
        -1,
        0,
        0);
    placeBlock(
        resources,
        world,
        "rigel:query_adjacent_cube",
        BlockCollisionShape::fullCube(),
        0,
        0,
        0);
    placeBlock(
        resources,
        world,
        "rigel:query_empty",
        BlockCollisionShape::empty(),
        0,
        1,
        0);

    const auto touching = query(
        world,
        {{0.0f, 0.25f, 0.25f}, {0.0f, 0.75f, 0.75f}});

    CHECK_EQ(touching.size(), static_cast<size_t>(2));
    CHECK_EQ(
        touching[0],
        (BlockCollisionBox{{-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 1.0f}}));
    CHECK_EQ(
        touching[1],
        (BlockCollisionBox{{0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}}));
    CHECK(query(
        world,
        {{0.1f, 1.1f, 0.1f}, {0.9f, 1.9f, 0.9f}}).empty());
}

TEST_CASE(WorldCollisionQuery_ClipsResultsToSuppliedBounds) {
    WorldResources resources;
    World world(resources);
    placeBlock(
        resources,
        world,
        "rigel:query_inside_bounds",
        BlockCollisionShape::boxes({
            {{0.0f, 0.0f, 0.0f}, {0.4f, 1.0f, 1.0f}},
            {{0.6f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}},
        }),
        0,
        0,
        0);
    placeBlock(
        resources,
        world,
        "rigel:query_outside_bounds",
        BlockCollisionShape::fullCube(),
        1,
        0,
        0);

    const auto boxes = query(
        world,
        {{0.0f, 0.0f, 0.0f}, {0.5f, 1.0f, 1.0f}});

    CHECK_EQ(boxes.size(), static_cast<size_t>(1));
    CHECK_EQ(
        boxes.front(),
        (BlockCollisionBox{{0.0f, 0.0f, 0.0f}, {0.4f, 1.0f, 1.0f}}));
}

TEST_CASE(WorldCollisionQuery_FindsBothDirectionsOfSupportedOverhang) {
    WorldResources resources;
    World world(resources);
    placeBlock(
        resources,
        world,
        "rigel:query_negative_overhang",
        BlockCollisionShape::boxes({
            {{-0.25f, 0.0f, 0.0f}, {0.0f, 1.0f, 1.0f}},
        }),
        2,
        0,
        0);
    placeBlock(
        resources,
        world,
        "rigel:query_positive_overhang",
        BlockCollisionShape::boxes({
            {{1.0f, 0.0f, 0.0f}, {1.25f, 1.0f, 1.0f}},
        }),
        -2,
        0,
        0);

    const auto negativeOverhang = query(
        world,
        {{1.7f, 0.25f, 0.25f}, {1.8f, 0.75f, 0.75f}});
    const auto positiveOverhang = query(
        world,
        {{-0.8f, 0.25f, 0.25f}, {-0.7f, 0.75f, 0.75f}});

    CHECK_EQ(negativeOverhang.size(), static_cast<size_t>(1));
    CHECK_EQ(
        negativeOverhang.front(),
        (BlockCollisionBox{{1.75f, 0.0f, 0.0f}, {2.0f, 1.0f, 1.0f}}));
    CHECK_EQ(positiveOverhang.size(), static_cast<size_t>(1));
    CHECK_EQ(
        positiveOverhang.front(),
        (BlockCollisionBox{{-1.0f, 0.0f, 0.0f}, {-0.75f, 1.0f, 1.0f}}));
}
