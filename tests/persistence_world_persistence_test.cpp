#include "TestFramework.h"

#include "Rigel/Asset/AssetManager.h"
#include "Rigel/Persistence/Backends/Memory/MemoryFormat.h"
#include "Rigel/Persistence/PersistenceService.h"
#include "Rigel/Persistence/Storage.h"
#include "Rigel/Persistence/WorldPersistence.h"
#include "Rigel/Voxel/BlockType.h"
#include "Rigel/Voxel/World.h"
#include "Rigel/Voxel/WorldResources.h"

#include <limits>
#include <string>

using namespace Rigel;

namespace {

constexpr const char* kDefaultZoneId = "rigel:default";

Voxel::BlockID registerBlock(Voxel::WorldResources& resources,
                             const std::string& identifier) {
    Voxel::BlockType block;
    block.identifier = identifier;
    block.model = "cube";
    block.isOpaque = true;
    block.isSolid = true;
    return resources.registry().registerBlock(identifier, std::move(block));
}

Persistence::ChunkSnapshot makeUnitSnapshot(
    Voxel::ChunkCoord coord,
    int32_t offsetX,
    Voxel::BlockID blockId) {
    Persistence::ChunkSnapshot snapshot;
    snapshot.key = Persistence::ChunkKey{
        kDefaultZoneId, coord.x, coord.y, coord.z};
    snapshot.data.span.chunkX = coord.x;
    snapshot.data.span.chunkY = coord.y;
    snapshot.data.span.chunkZ = coord.z;
    snapshot.data.span.offsetX = offsetX;
    snapshot.data.span.sizeX = 1;
    snapshot.data.span.sizeY = 1;
    snapshot.data.span.sizeZ = 1;
    snapshot.data.blocks.push_back(Voxel::BlockState{blockId});
    return snapshot;
}

template <typename Fn>
void checkRuntimeError(Fn&& fn, const std::string& expected) {
    try {
        fn();
    } catch (const std::runtime_error& error) {
        CHECK_EQ(std::string(error.what()), expected);
        return;
    }
    throw Test::TestFailure("Expected std::runtime_error");
}

struct MemoryPersistenceFixture {
    MemoryPersistenceFixture()
        : service(formats),
          directory("rigel_world_persistence_validation") {
        formats.registerFormat(
            Persistence::Backends::Memory::descriptor(),
            Persistence::Backends::Memory::factory(),
            Persistence::Backends::Memory::probe());
        context.rootPath = directory.path().string();
        context.preferredFormat = "memory";
        context.storage = std::make_shared<Persistence::FilesystemBackend>();
    }

    void saveRegion(std::vector<Persistence::ChunkSnapshot> chunks) {
        Persistence::ChunkRegionSnapshot region;
        region.key = Persistence::RegionKey{kDefaultZoneId, 0, 0, 0};
        region.chunks = std::move(chunks);
        service.saveRegion(region, context);
    }

    Persistence::FormatRegistry formats;
    Persistence::PersistenceService service;
    Test::TemporaryDirectory directory;
    Persistence::PersistenceContext context;
    Asset::AssetManager assets;
};

void prepareDestinationChunk(Voxel::World& world,
                             Voxel::ChunkCoord coord,
                             Voxel::BlockID blockId,
                             uint32_t worldGenVersion) {
    Voxel::Chunk& chunk = world.chunkManager().getOrCreateChunk(coord);
    chunk.fill(Voxel::BlockState{blockId}, world.blockRegistry());
    chunk.setWorldGenVersion(worldGenVersion);
    chunk.clearDirty();
    chunk.clearPersistDirty();
}

void checkDestinationChunk(const Voxel::World& world,
                           Voxel::ChunkCoord coord,
                           Voxel::BlockID blockId,
                           uint32_t worldGenVersion) {
    const Voxel::Chunk* chunk = world.chunkManager().getChunk(coord);
    CHECK(chunk != nullptr);
    CHECK_EQ(chunk->getBlock(0, 0, 0).id, blockId);
    CHECK_EQ(chunk->getBlock(1, 0, 0).id, blockId);
    CHECK_EQ(chunk->worldGenVersion(), worldGenVersion);
    CHECK(!chunk->isDirty());
    CHECK(!chunk->isPersistDirty());
}

} // namespace

TEST_CASE(Persistence_WorldSaveLoad_MemoryFormat) {
    Voxel::WorldResources resources;
    std::string testIdentifier = "base:test";
    Voxel::BlockType testBlock;
    testBlock.identifier = testIdentifier;
    testBlock.model = "cube";
    testBlock.isOpaque = true;
    testBlock.isSolid = true;
    testBlock.textures = Voxel::FaceTextures::uniform("textures/blocks/test.png");
    auto testId = resources.registry().registerBlock(testIdentifier, std::move(testBlock));

    Voxel::World world(resources);
    world.setId(1);
    world.setBlock(0, 0, 0, Voxel::BlockState{testId});

    Persistence::FormatRegistry formats;
    formats.registerFormat(
        Persistence::Backends::Memory::descriptor(),
        Persistence::Backends::Memory::factory(),
        Persistence::Backends::Memory::probe());
    Persistence::PersistenceService service(formats);

    Test::TemporaryDirectory directory("rigel_world_persistence");

    auto storage = std::make_shared<Persistence::FilesystemBackend>();
    Persistence::PersistenceContext context;
    context.rootPath = directory.path().string();
    context.preferredFormat = "memory";
    context.storage = storage;
    context.providers = world.persistenceProvidersHandle();

    Persistence::ChunkSnapshot archivedChunk;
    archivedChunk.key = Persistence::ChunkKey{"rigel:archive", 20, 0, 0};
    archivedChunk.data.span.chunkX = 20;
    archivedChunk.data.span.sizeX = 1;
    archivedChunk.data.span.sizeY = 1;
    archivedChunk.data.span.sizeZ = 1;
    archivedChunk.data.blocks.push_back(Voxel::BlockState{testId, 2, 3});

    Persistence::ChunkRegionSnapshot archivedChunkRegion;
    archivedChunkRegion.key = Persistence::RegionKey{"rigel:archive", 1, 0, 0};
    archivedChunkRegion.chunks.push_back(archivedChunk);
    service.saveRegion(archivedChunkRegion, context);

    Persistence::EntityPersistedEntity archivedEntity;
    archivedEntity.typeId = "rigel:archived_entity";
    archivedEntity.id = Entity::EntityId{1, 2, 3};
    archivedEntity.position = glm::vec3(320.0f, 1.0f, 2.0f);

    Persistence::EntityPersistedChunk archivedEntityChunk;
    archivedEntityChunk.coord = Voxel::ChunkCoord{20, 0, 0};
    archivedEntityChunk.entities.push_back(archivedEntity);

    Persistence::EntityRegionSnapshot archivedEntityRegion;
    archivedEntityRegion.key = Persistence::EntityRegionKey{"rigel:archive", 1, 0, 0};
    archivedEntityRegion.chunks.push_back(archivedEntityChunk);
    service.saveEntities(archivedEntityRegion, context);

    Persistence::saveWorldToDisk(world, service, context);

    CHECK_EQ(service.loadRegion(archivedChunkRegion.key, context), archivedChunkRegion);
    CHECK_EQ(service.loadEntities(archivedEntityRegion.key, context), archivedEntityRegion);

    Voxel::World loaded(resources);
    loaded.setId(1);
    Asset::AssetManager assets;

    Persistence::loadWorldFromDisk(loaded, assets, service, context, 0);

    Voxel::BlockState loadedState = loaded.getBlock(0, 0, 0);
    CHECK_EQ(loadedState.id, testId);
}

TEST_CASE(Persistence_LoadChunkValidatesAllSnapshotsBeforeMutation) {
    MemoryPersistenceFixture fixture;
    Voxel::WorldResources resources;
    const Voxel::BlockID originalId = registerBlock(resources, "base:original");
    const Voxel::BlockID replacementId = registerBlock(resources, "base:replacement");
    const Voxel::BlockID invalidId{std::numeric_limits<uint16_t>::max()};
    const Voxel::ChunkCoord coord{0, 0, 0};

    fixture.saveRegion({
        makeUnitSnapshot(coord, 0, replacementId),
        makeUnitSnapshot(coord, 1, invalidId)});

    Voxel::World world(resources);
    prepareDestinationChunk(world, coord, originalId, 7);
    const uint64_t meshChangeVersion =
        world.chunkManager().meshChangeVersion();

    checkRuntimeError(
        [&]() {
            Persistence::loadChunkFromDisk(
                world, fixture.service, fixture.context, coord, 99);
        },
        "ChunkSerializer: invalid block ID 65535");

    CHECK_EQ(world.chunkManager().loadedChunkCount(), 1u);
    CHECK_EQ(world.chunkManager().meshChangeVersion(), meshChangeVersion);
    checkDestinationChunk(world, coord, originalId, 7);
}

TEST_CASE(Persistence_LoadChunkRejectsInvalidSnapshotBeforeCreation) {
    MemoryPersistenceFixture fixture;
    Voxel::WorldResources resources;
    const Voxel::BlockID invalidId{std::numeric_limits<uint16_t>::max()};
    const Voxel::ChunkCoord coord{1, 0, 0};

    fixture.saveRegion({makeUnitSnapshot(coord, 0, invalidId)});

    Voxel::World world(resources);
    const uint64_t meshChangeVersion =
        world.chunkManager().meshChangeVersion();

    checkRuntimeError(
        [&]() {
            Persistence::loadChunkFromDisk(
                world, fixture.service, fixture.context, coord, 99);
        },
        "ChunkSerializer: invalid block ID 65535");

    CHECK(!world.chunkManager().hasChunk(coord));
    CHECK_EQ(world.chunkManager().loadedChunkCount(), 0u);
    CHECK_EQ(world.chunkManager().meshChangeVersion(), meshChangeVersion);
}

TEST_CASE(Persistence_LoadWorldValidatesAllSnapshotsBeforeMutation) {
    MemoryPersistenceFixture fixture;
    Voxel::WorldResources resources;
    const Voxel::BlockID originalId = registerBlock(resources, "base:original");
    const Voxel::BlockID replacementId = registerBlock(resources, "base:replacement");
    const Voxel::BlockID invalidId{std::numeric_limits<uint16_t>::max()};
    const Voxel::ChunkCoord coord{0, 0, 0};

    fixture.saveRegion({
        makeUnitSnapshot(coord, 0, replacementId),
        makeUnitSnapshot(coord, 1, invalidId)});

    Voxel::World world(resources);
    prepareDestinationChunk(world, coord, originalId, 7);
    const uint64_t meshChangeVersion =
        world.chunkManager().meshChangeVersion();

    checkRuntimeError(
        [&]() {
            Persistence::loadWorldFromDisk(
                world,
                fixture.assets,
                fixture.service,
                fixture.context,
                99,
                Persistence::LoadScope::ChunksOnly);
        },
        "ChunkSerializer: invalid block ID 65535");

    CHECK_EQ(world.chunkManager().loadedChunkCount(), 1u);
    CHECK_EQ(world.chunkManager().meshChangeVersion(), meshChangeVersion);
    checkDestinationChunk(world, coord, originalId, 7);
}

TEST_CASE(Persistence_LoadWorldRejectsInvalidSnapshotBeforeCreation) {
    MemoryPersistenceFixture fixture;
    Voxel::WorldResources resources;
    const Voxel::BlockID originalId = registerBlock(resources, "base:original");
    const Voxel::BlockID invalidId{std::numeric_limits<uint16_t>::max()};
    const Voxel::ChunkCoord existingCoord{0, 0, 0};
    const Voxel::ChunkCoord savedCoord{1, 0, 0};

    fixture.saveRegion({makeUnitSnapshot(savedCoord, 0, invalidId)});

    Voxel::World world(resources);
    prepareDestinationChunk(world, existingCoord, originalId, 7);
    const uint64_t meshChangeVersion =
        world.chunkManager().meshChangeVersion();

    checkRuntimeError(
        [&]() {
            Persistence::loadWorldFromDisk(
                world,
                fixture.assets,
                fixture.service,
                fixture.context,
                99,
                Persistence::LoadScope::ChunksOnly);
        },
        "ChunkSerializer: invalid block ID 65535");

    CHECK(!world.chunkManager().hasChunk(savedCoord));
    CHECK_EQ(world.chunkManager().loadedChunkCount(), 1u);
    CHECK_EQ(world.chunkManager().meshChangeVersion(), meshChangeVersion);
    checkDestinationChunk(world, existingCoord, originalId, 7);
}
