#include "TestFramework.h"

#include "Rigel/Voxel/ChunkManager.h"

#include <array>
#include <utility>

using namespace Rigel::Voxel;

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

TEST_CASE(ChunkManager_Unload) {
    ChunkManager manager;
    ChunkCoord coord{2, 0, 0};
    BlockState state;
    state.id.type = 5;
    manager.getOrCreateChunk(coord).setBlock(0, 0, 0, state);
    CHECK(manager.hasChunk(coord));
    CHECK_EQ(manager.getBlock(coord.x * ChunkSize, 0, 0).id.type, static_cast<uint16_t>(5));

    manager.unloadChunk(coord);
    CHECK(!manager.hasChunk(coord));
}

TEST_CASE(ChunkManager_UnloadInvalidatesSurvivingFaceNeighborsOnce) {
    ChunkManager manager;
    const ChunkCoord center{0, 0, 0};
    manager.getOrCreateChunk(center).clearDirty();

    std::array<ChunkCoord, DirectionCount> neighborCoords{};
    std::array<uint32_t, DirectionCount> neighborRevisions{};
    for (size_t i = 0; i < DirectionCount; ++i) {
        int dx = 0;
        int dy = 0;
        int dz = 0;
        directionOffset(static_cast<Direction>(i), dx, dy, dz);
        neighborCoords[i] = center.offset(dx, dy, dz);
        Chunk& neighbor = manager.getOrCreateChunk(neighborCoords[i]);
        neighbor.clearDirty();
        neighborRevisions[i] = neighbor.meshRevision();
    }

    manager.unloadChunk(center);

    CHECK(!manager.hasChunk(center));
    for (size_t i = 0; i < DirectionCount; ++i) {
        const Chunk* neighbor = manager.getChunk(neighborCoords[i]);
        CHECK(neighbor != nullptr);
        if (neighbor) {
            CHECK(neighbor->isDirty());
            CHECK_EQ(neighbor->meshRevision(), neighborRevisions[i] + 1);
        }
    }

    manager.unloadChunk(center);
    for (size_t i = 0; i < DirectionCount; ++i) {
        CHECK_EQ(
            manager.getChunk(neighborCoords[i])->meshRevision(),
            neighborRevisions[i] + 1);
    }
}

TEST_CASE(ChunkManager_MoveRetainsChunks) {
    ChunkManager source;
    source.getOrCreateChunk({0, 0, 0}).clearDirty();

    ChunkManager moved(std::move(source));
    const uint32_t beforeConstructedMutation =
        moved.getChunk({0, 0, 0})->meshRevision();
    moved.getChunk({0, 0, 0})->invalidateMesh();
    CHECK_EQ(
        moved.getChunk({0, 0, 0})->meshRevision(),
        beforeConstructedMutation + 1);

    moved.getChunk({0, 0, 0})->clearDirty();
    ChunkManager assigned;
    assigned = std::move(moved);
    const uint32_t beforeAssignedMutation =
        assigned.getChunk({0, 0, 0})->meshRevision();
    assigned.getChunk({0, 0, 0})->invalidateMesh();
    CHECK_EQ(
        assigned.getChunk({0, 0, 0})->meshRevision(),
        beforeAssignedMutation + 1);
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
