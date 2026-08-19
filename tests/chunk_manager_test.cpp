#include "TestFramework.h"

#include "Rigel/Voxel/ChunkManager.h"

#include <utility>

using namespace Rigel::Voxel;

TEST_CASE(ChunkManager_BlockAccessAndDirty) {
    ChunkManager manager;
    BlockState state;
    state.id.type = 3;

    manager.setBlock(0, 0, 0, state);
    CHECK_EQ(manager.loadedChunkCount(), static_cast<size_t>(1));
    CHECK_EQ(manager.getBlock(0, 0, 0).id.type, static_cast<uint16_t>(3));

    auto dirty = manager.getDirtyChunks();
    CHECK_EQ(dirty.size(), static_cast<size_t>(1));

    manager.clearDirtyFlags();
    CHECK(manager.getDirtyChunks().empty());
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

TEST_CASE(ChunkManager_LoadAndUnload) {
    ChunkManager manager;
    ChunkCoord coord{2, 0, 0};
    Chunk chunk(coord);
    BlockState state;
    state.id.type = 5;
    chunk.setBlock(0, 0, 0, state);

    auto data = chunk.serialize();
    manager.loadChunk(coord, data);
    CHECK(manager.hasChunk(coord));
    CHECK_EQ(manager.getBlock(coord.x * ChunkSize, 0, 0).id.type, static_cast<uint16_t>(5));

    manager.unloadChunk(coord);
    CHECK(!manager.hasChunk(coord));
}

TEST_CASE(ChunkManager_MoveRetainsChangeTracking) {
    ChunkManager source;
    source.getOrCreateChunk({0, 0, 0});

    ChunkManager moved(std::move(source));
    const uint64_t beforeConstructedMutation = moved.meshChangeVersion();
    moved.getChunk({0, 0, 0})->markDirty();
    CHECK_EQ(moved.meshChangeVersion(), beforeConstructedMutation + 1);

    ChunkManager assigned;
    assigned = std::move(moved);
    const uint64_t beforeAssignedMutation = assigned.meshChangeVersion();
    assigned.getChunk({0, 0, 0})->markDirty();
    CHECK_EQ(assigned.meshChangeVersion(), beforeAssignedMutation + 1);
}
