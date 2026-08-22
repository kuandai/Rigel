#include "TestFramework.h"

#include "Rigel/Voxel/BlockType.h"
#include "Rigel/Voxel/WorldSet.h"

using namespace Rigel::Voxel;

template<typename T>
concept HasPublicWorldRemoval = requires(T& worlds, WorldId id) {
    worlds.removeWorld(id);
};

static_assert(!HasPublicWorldRemoval<WorldSet>);

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

TEST_CASE(WorldSet_ClearDestroysAllWorldsAndCanRepeatDuringTeardown) {
    WorldSet worldSet;
    worldSet.createWorld(1).setBlock(0, 0, 0, BlockState{});
    worldSet.createWorld(2).setBlock(ChunkSize, 0, 0, BlockState{});

    worldSet.clear();

    CHECK(!worldSet.hasWorld(1));
    CHECK(!worldSet.hasWorld(2));
    CHECK(worldSet.findView(1) == nullptr);
    CHECK(worldSet.findView(2) == nullptr);
    CHECK_NO_THROW(worldSet.clear());
}
