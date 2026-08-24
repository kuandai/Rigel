#include "TestFramework.h"

#include "Rigel/Voxel/ChunkManager.h"

#include <type_traits>

using namespace Rigel::Voxel;

template<typename T>
concept HasPublicChunkUnload = requires(T& manager, ChunkCoord coord) {
    manager.unloadChunk(coord, true);
};

static_assert(!HasPublicChunkUnload<ChunkManager>);
static_assert(!std::is_move_constructible_v<ChunkManager>);
static_assert(!std::is_move_assignable_v<ChunkManager>);

TEST_CASE(ChunkManager_BlockAccess) {
    ChunkManager manager;
    BlockState state;
    state.id.type = 3;

    manager.setBlock(0, 0, 0, state);
    CHECK_EQ(manager.loadedChunkCount(), static_cast<size_t>(1));
    CHECK_EQ(manager.getBlock(0, 0, 0).id.type, static_cast<uint16_t>(3));

    CHECK(manager.getChunk({0, 0, 0})->isDirty());
}

TEST_CASE(ChunkManager_ChunkBoundary) {
    ChunkManager manager;
    BlockState state;
    state.id.type = 4;

    manager.setBlock(ChunkSize, 0, 0, state);
    CHECK_EQ(manager.loadedChunkCount(), static_cast<size_t>(1));
    CHECK_EQ(manager.getBlock(ChunkSize, 0, 0).id.type, static_cast<uint16_t>(4));

    BlockState missing = manager.getBlock(-999, 0, 0);
    CHECK(missing.isAir());
}

TEST_CASE(ChunkManager_DirtyStateCoalescesMeshRevision) {
    ChunkManager manager;
    Chunk& chunk = manager.getOrCreateChunk({0, 0, 0});
    chunk.clearDirty();

    const uint32_t revision = chunk.meshRevision();
    chunk.markDirty();
    CHECK(chunk.isDirty());
    CHECK_EQ(chunk.meshRevision(), revision);

    chunk.markDirty();
    CHECK_EQ(chunk.meshRevision(), revision);

    chunk.invalidateMesh();
    CHECK_EQ(chunk.meshRevision(), revision + 1);

    chunk.invalidateMesh();
    CHECK_EQ(chunk.meshRevision(), revision + 1);

    chunk.clearDirty();
    chunk.invalidateMesh();
    CHECK_EQ(chunk.meshRevision(), revision + 2);
}
