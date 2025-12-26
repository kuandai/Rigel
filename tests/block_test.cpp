#include "TestFramework.h"

#include "Rigel/Voxel/Block.h"

using namespace Rigel::Voxel;

TEST_CASE(BlockState_LightLevels) {
    BlockState state;
    CHECK(state.isAir());

    state.setSkyLight(7);
    state.setBlockLight(3);

    CHECK_EQ(state.skyLight(), 7);
    CHECK_EQ(state.blockLight(), 3);
}

TEST_CASE(Direction_OppositeAndOffset) {
    CHECK_EQ(opposite(Direction::PosX), Direction::NegX);
    CHECK_EQ(opposite(Direction::PosY), Direction::NegY);
    CHECK_EQ(opposite(Direction::PosZ), Direction::NegZ);

    int dx = 0;
    int dy = 0;
    int dz = 0;
    directionOffset(Direction::NegX, dx, dy, dz);
    CHECK_EQ(dx, -1);
    CHECK_EQ(dy, 0);
    CHECK_EQ(dz, 0);
}
