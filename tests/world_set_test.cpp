#include "TestFramework.h"

#include "Rigel/Voxel/BlockType.h"
#include "Rigel/Voxel/WorldSet.h"

using namespace Rigel::Voxel;

TEST_CASE(WorldSet_DefaultWorldUsesSharedRegistry) {
    WorldSet worldSet;
    World& world = worldSet.createWorld(WorldSet::defaultWorldId());

    CHECK_EQ(world.id(), WorldSet::defaultWorldId());
    CHECK_EQ(&world.blockRegistry(), &worldSet.resources().registry());
}

TEST_CASE(WorldSet_MultipleWorldsHaveIndependentChunks) {
    WorldSet worldSet;

    BlockType solid;
    solid.identifier = "rigel:stone";
    worldSet.resources().registry().registerBlock(solid.identifier, solid);
    auto solidId = worldSet.resources().registry().findByIdentifier(solid.identifier);
    CHECK(solidId.has_value());

    World& first = worldSet.createWorld(1);
    World& second = worldSet.createWorld(2);

    BlockState state;
    state.id = *solidId;
    first.setBlock(0, 0, 0, state);

    CHECK_EQ(first.getBlock(0, 0, 0).id, state.id);
    CHECK(second.getBlock(0, 0, 0).isAir());
}
