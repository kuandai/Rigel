#include "TestFramework.h"

#include "Rigel/Voxel/Chunk.h"
#include "Rigel/Voxel/BlockRegistry.h"

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

TEST_CASE(Chunk_CopyFromWithRegistry) {
    BlockRegistry registry;
    BlockType solid;
    solid.identifier = "rigel:test_solid2";
    solid.isOpaque = true;
    BlockType glass;
    glass.identifier = "rigel:test_glass2";
    glass.isOpaque = false;

    BlockID solidId = registry.registerBlock(solid.identifier, solid);
    BlockID glassId = registry.registerBlock(glass.identifier, glass);

    std::array<BlockState, Chunk::VOLUME> data{};
    data[0].id = solidId;
    data[5].id = glassId;

    Chunk chunk({0, 0, 0});
    chunk.copyFrom(data, registry);

    CHECK_EQ(chunk.nonAirCount(), 2u);
    CHECK_EQ(chunk.opaqueCount(), 1u);
}

TEST_CASE(Chunk_OpacityCounts) {
    BlockRegistry registry;
    BlockType solid;
    solid.identifier = "rigel:test_solid";
    solid.isOpaque = true;
    BlockType glass;
    glass.identifier = "rigel:test_glass";
    glass.isOpaque = false;

    BlockID solidId = registry.registerBlock(solid.identifier, solid);
    BlockID glassId = registry.registerBlock(glass.identifier, glass);

    Chunk chunk({0, 0, 0});
    BlockState solidState;
    solidState.id = solidId;
    BlockState glassState;
    glassState.id = glassId;

    chunk.setBlock(0, 0, 0, solidState, registry);
    chunk.setBlock(1, 0, 0, glassState, registry);

    CHECK_EQ(chunk.nonAirCount(), 2u);
    CHECK_EQ(chunk.opaqueCount(), 1u);

    chunk.setBlock(0, 0, 0, BlockState{}, registry);
    CHECK_EQ(chunk.nonAirCount(), 1u);
    CHECK_EQ(chunk.opaqueCount(), 0u);
}

TEST_CASE(Chunk_SerializeRoundTrip) {
    Chunk chunk({-1, 2, 3});
    chunk.setWorldGenVersion(42);
    BlockState state;
    state.id.type = 7;
    chunk.setBlock(0, 0, 0, state);
    chunk.setBlock(Chunk::SIZE - 1, 0, 0, state);

    auto data = chunk.serialize();
    Chunk loaded = Chunk::deserialize(data);

    CHECK_EQ(loaded.position().x, -1);
    CHECK_EQ(loaded.position().y, 2);
    CHECK_EQ(loaded.position().z, 3);
    CHECK_EQ(loaded.worldGenVersion(), static_cast<uint32_t>(42));
    CHECK_EQ(loaded.getBlock(0, 0, 0).id.type, static_cast<uint16_t>(7));
    CHECK_EQ(loaded.getBlock(Chunk::SIZE - 1, 0, 0).id.type, static_cast<uint16_t>(7));
}

TEST_CASE(Chunk_CopyBlocks) {
    Chunk chunk({0, 0, 0});
    BlockState state;
    state.id.type = 9;
    chunk.setBlock(0, 0, 0, state);
    chunk.setBlock(Chunk::SIZE - 1, Chunk::SIZE - 1, Chunk::SIZE - 1, state);

    std::array<BlockState, Chunk::VOLUME> out{};
    chunk.copyBlocks(out);

    CHECK_EQ(out[0].id.type, static_cast<uint16_t>(9));
    int maxIndex = (Chunk::SIZE - 1)
        + (Chunk::SIZE - 1) * Chunk::SIZE
        + (Chunk::SIZE - 1) * Chunk::SIZE * Chunk::SIZE;
    CHECK_EQ(out[static_cast<size_t>(maxIndex)].id.type, static_cast<uint16_t>(9));
}

TEST_CASE(Chunk_PersistDirty) {
    Chunk chunk({0, 0, 0});
    CHECK(!chunk.isPersistDirty());

    BlockState state;
    state.id.type = 3;
    chunk.setBlock(0, 0, 0, state);
    CHECK(chunk.isPersistDirty());

    chunk.clearPersistDirty();
    CHECK(!chunk.isPersistDirty());
}
