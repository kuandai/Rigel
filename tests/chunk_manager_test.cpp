#include "TestFramework.h"

#include "Rigel/Voxel/ChunkManager.h"

#include <array>
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

    const uint64_t changeVersion = manager.meshChangeVersion();
    manager.unloadChunk(center);

    CHECK(!manager.hasChunk(center));
    CHECK_EQ(
        manager.meshChangeVersion(),
        changeVersion + static_cast<uint64_t>(DirectionCount) + 1);
    for (size_t i = 0; i < DirectionCount; ++i) {
        const Chunk* neighbor = manager.getChunk(neighborCoords[i]);
        CHECK(neighbor != nullptr);
        if (neighbor) {
            CHECK(neighbor->isDirty());
            CHECK_EQ(neighbor->meshRevision(), neighborRevisions[i] + 1);
        }
    }

    const uint64_t versionAfterRemoval = manager.meshChangeVersion();
    manager.unloadChunk(center);
    CHECK_EQ(manager.meshChangeVersion(), versionAfterRemoval);
    for (size_t i = 0; i < DirectionCount; ++i) {
        CHECK_EQ(
            manager.getChunk(neighborCoords[i])->meshRevision(),
            neighborRevisions[i] + 1);
    }
}

TEST_CASE(ChunkManager_ReplacementInvalidatesSurvivingFaceNeighborsOnce) {
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

    Chunk replacement(center);
    BlockState replacementState;
    replacementState.id.type = 6;
    replacement.setBlock(1, 1, 1, replacementState);
    auto replacementData = replacement.serialize();

    manager.loadChunk(center, replacementData);
    manager.loadChunk(center, replacementData);

    CHECK_EQ(manager.loadedChunkCount(), static_cast<size_t>(DirectionCount + 1));
    CHECK_EQ(manager.getBlock(1, 1, 1), replacementState);
    for (size_t i = 0; i < DirectionCount; ++i) {
        const Chunk* neighbor = manager.getChunk(neighborCoords[i]);
        CHECK(neighbor != nullptr);
        if (neighbor) {
            CHECK(neighbor->isDirty());
            CHECK_EQ(neighbor->meshRevision(), neighborRevisions[i] + 1);
        }
    }
}

TEST_CASE(ChunkManager_MoveRetainsChangeTracking) {
    ChunkManager source;
    source.getOrCreateChunk({0, 0, 0}).clearDirty();

    ChunkManager moved(std::move(source));
    const uint64_t beforeConstructedMutation = moved.meshChangeVersion();
    moved.getChunk({0, 0, 0})->invalidateMesh();
    CHECK_EQ(moved.meshChangeVersion(), beforeConstructedMutation + 1);

    moved.getChunk({0, 0, 0})->clearDirty();
    ChunkManager assigned;
    assigned = std::move(moved);
    const uint64_t beforeAssignedMutation = assigned.meshChangeVersion();
    assigned.getChunk({0, 0, 0})->invalidateMesh();
    CHECK_EQ(assigned.meshChangeVersion(), beforeAssignedMutation + 1);
}

TEST_CASE(ChunkManager_DirtyNotificationsCoalesceWithoutChangingMeshRevision) {
    ChunkManager manager;
    Chunk& chunk = manager.getOrCreateChunk({0, 0, 0});
    chunk.clearDirty();

    const uint32_t revision = chunk.meshRevision();
    const uint64_t changeVersion = manager.meshChangeVersion();

    chunk.markDirty();
    CHECK(chunk.isDirty());
    CHECK_EQ(chunk.meshRevision(), revision);
    CHECK_EQ(manager.meshChangeVersion(), changeVersion + 1);

    chunk.markDirty();
    CHECK_EQ(chunk.meshRevision(), revision);
    CHECK_EQ(manager.meshChangeVersion(), changeVersion + 1);

    chunk.invalidateMesh();
    CHECK_EQ(chunk.meshRevision(), revision + 1);
    CHECK_EQ(manager.meshChangeVersion(), changeVersion + 2);

    chunk.invalidateMesh();
    CHECK_EQ(chunk.meshRevision(), revision + 1);
    CHECK_EQ(manager.meshChangeVersion(), changeVersion + 2);

    chunk.clearDirty();
    chunk.invalidateMesh();
    CHECK_EQ(chunk.meshRevision(), revision + 2);
    CHECK_EQ(manager.meshChangeVersion(), changeVersion + 3);
}
