#include "TestFramework.h"

#include "Rigel/Voxel/Chunk.h"

using namespace Rigel::Voxel;

TEST_CASE(Chunk_SetGetAndCounts) {
    Chunk chunk({0, 0, 0});
    CHECK(chunk.isEmpty());
    CHECK_EQ(chunk.nonAirCount(), 0u);

    BlockState state;
    state.id.type = 1;
    chunk.setBlock(1, 2, 3, state);

    CHECK(!chunk.isEmpty());
    CHECK_EQ(chunk.nonAirCount(), 1u);
    CHECK_EQ(chunk.getBlock(1, 2, 3).id.type, static_cast<uint16_t>(1));
}

TEST_CASE(Chunk_CopyFrom) {
    Chunk chunk({0, 0, 0});
    std::array<BlockState, Chunk::VOLUME> data{};
    data[0].id.type = 2;
    data[5].id.type = 2;

    chunk.copyFrom(data);
    CHECK_EQ(chunk.nonAirCount(), 2u);
    CHECK_EQ(chunk.getBlock(0, 0, 0).id.type, static_cast<uint16_t>(2));
}

TEST_CASE(Chunk_SerializeRoundTrip) {
    Chunk chunk({-1, 2, 3});
    BlockState state;
    state.id.type = 7;
    chunk.setBlock(0, 0, 0, state);
    chunk.setBlock(Chunk::SIZE - 1, 0, 0, state);

    auto data = chunk.serialize();
    Chunk loaded = Chunk::deserialize(data);

    CHECK_EQ(loaded.position().x, -1);
    CHECK_EQ(loaded.position().y, 2);
    CHECK_EQ(loaded.position().z, 3);
    CHECK_EQ(loaded.getBlock(0, 0, 0).id.type, static_cast<uint16_t>(7));
    CHECK_EQ(loaded.getBlock(Chunk::SIZE - 1, 0, 0).id.type, static_cast<uint16_t>(7));
}
