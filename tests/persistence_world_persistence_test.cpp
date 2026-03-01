#include "TestFramework.h"

#include "Rigel/Asset/AssetManager.h"
#include "Rigel/Persistence/Backends/CR/CRFormat.h"
#include "Rigel/Persistence/Backends/CR/CRChunkMapping.h"
#include "Rigel/Persistence/Backends/Memory/MemoryFormat.h"
#include "Rigel/Persistence/ChunkSerializer.h"
#include "Rigel/Persistence/Providers.h"
#include "Rigel/Persistence/PersistenceService.h"
#include "Rigel/Persistence/Storage.h"
#include "Rigel/Persistence/WorldPersistence.h"
#include "Rigel/Voxel/Chunk.h"
#include "Rigel/Voxel/BlockType.h"
#include "Rigel/Voxel/World.h"
#include "Rigel/Voxel/WorldResources.h"

#include <chrono>
#include <filesystem>

using namespace Rigel;

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

    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("rigel_persist_test_" + std::to_string(now));
    std::filesystem::create_directories(root);

    auto storage = std::make_shared<Persistence::FilesystemBackend>();
    Persistence::PersistenceContext context;
    context.rootPath = root.string();
    context.preferredFormat = "memory";
    context.storage = storage;
    context.providers = world.persistenceProvidersHandle();

    Persistence::saveWorldToDisk(world, service, context);

    Voxel::World loaded(resources);
    loaded.setId(1);
    Asset::AssetManager assets;

    Persistence::loadWorldFromDisk(loaded, assets, service, context, 0);

    Voxel::BlockState loadedState = loaded.getBlock(0, 0, 0);
    CHECK_EQ(loadedState.id, testId);

    std::filesystem::remove_all(root);
}

TEST_CASE(Persistence_WorldPersistence_UsesMetadataDefaultZoneForChunkLoad) {
    Voxel::WorldResources resources;
    std::string testIdentifier = "base:test_zone";
    Voxel::BlockType testBlock;
    testBlock.identifier = testIdentifier;
    testBlock.model = "cube";
    testBlock.isOpaque = true;
    testBlock.isSolid = true;
    testBlock.textures = Voxel::FaceTextures::uniform("textures/blocks/test_zone.png");
    auto testId = resources.registry().registerBlock(testIdentifier, std::move(testBlock));

    Persistence::FormatRegistry formats;
    formats.registerFormat(
        Persistence::Backends::Memory::descriptor(),
        Persistence::Backends::Memory::factory(),
        Persistence::Backends::Memory::probe());
    Persistence::PersistenceService service(formats);

    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("rigel_zone_select_test_" + std::to_string(now));
    std::filesystem::create_directories(root);

    auto storage = std::make_shared<Persistence::FilesystemBackend>();
    Persistence::PersistenceContext context;
    context.rootPath = root.string();
    context.preferredFormat = "memory";
    context.storage = storage;

    Persistence::WorldSnapshot snapshot;
    snapshot.metadata.worldId = "world_1";
    snapshot.metadata.displayName = "World 1";
    snapshot.metadata.defaultZoneId = "rigel:metadata_zone";
    snapshot.zones.push_back(Persistence::ZoneMetadata{"rigel:metadata_zone", "Metadata Zone"});
    service.saveWorld(snapshot, Persistence::SaveScope::MetadataOnly, context);

    auto format = service.openFormat(context);
    Voxel::ChunkCoord coord{0, 0, 0};
    Persistence::RegionKey regionKey = format->regionLayout().regionForChunk("rigel:metadata_zone", coord);

    Voxel::Chunk chunk(coord);
    chunk.setBlock(0, 0, 0, Voxel::BlockState{testId}, resources.registry());

    Persistence::ChunkSnapshot chunkSnapshot;
    chunkSnapshot.key = Persistence::ChunkKey{"rigel:metadata_zone", coord.x, coord.y, coord.z};
    chunkSnapshot.data = Persistence::serializeChunk(chunk);

    Persistence::ChunkRegionSnapshot region;
    region.key = regionKey;
    region.chunks.push_back(chunkSnapshot);
    format->chunkContainer().saveRegion(region);

    Voxel::World loaded(resources);
    loaded.setId(1);
    bool loadedFromDisk = Persistence::loadChunkFromDisk(loaded, service, context, coord, 0);
    CHECK(loadedFromDisk);
    if (loadedFromDisk) {
        CHECK_EQ(loaded.getBlock(0, 0, 0).id, testId);
    }

    std::filesystem::remove_all(root);
}

TEST_CASE(Persistence_WorldPersistence_UsesMetadataDefaultZoneForChunkLoad_CR) {
    Voxel::WorldResources resources;
    std::string testIdentifier = "base:test_zone_cr";
    Voxel::BlockType testBlock;
    testBlock.identifier = testIdentifier;
    testBlock.model = "cube";
    testBlock.isOpaque = true;
    testBlock.isSolid = true;
    testBlock.textures = Voxel::FaceTextures::uniform("textures/blocks/test_zone_cr.png");
    auto testId = resources.registry().registerBlock(testIdentifier, std::move(testBlock));

    Persistence::FormatRegistry formats;
    formats.registerFormat(
        Persistence::Backends::CR::descriptor(),
        Persistence::Backends::CR::factory(),
        Persistence::Backends::CR::probe());
    Persistence::PersistenceService service(formats);

    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("rigel_zone_select_cr_test_" + std::to_string(now));
    std::filesystem::create_directories(root);

    auto storage = std::make_shared<Persistence::FilesystemBackend>();
    Persistence::PersistenceContext context;
    context.rootPath = root.string();
    context.preferredFormat = "cr";
    context.storage = storage;

    auto providers = std::make_shared<Persistence::ProviderRegistry>();
    providers->add(
        Persistence::kBlockRegistryProviderId,
        std::make_shared<Persistence::BlockRegistryProvider>(&resources.registry()));
    context.providers = providers;

    Persistence::WorldSnapshot snapshot;
    snapshot.metadata.worldId = "world_1";
    snapshot.metadata.displayName = "World 1";
    snapshot.metadata.defaultZoneId = "base:earth";
    snapshot.zones.push_back(Persistence::ZoneMetadata{"base:earth", "Earth"});
    service.saveWorld(snapshot, Persistence::SaveScope::MetadataOnly, context);

    auto format = service.openFormat(context);
    Voxel::ChunkCoord coord{0, 0, 0};
    Persistence::RegionKey regionKey = format->regionLayout().regionForChunk("base:earth", coord);
    auto crKey = Persistence::Backends::CR::toCRChunk({coord.x, coord.y, coord.z, 0});
    crKey.zoneId = "base:earth";

    Persistence::ChunkSnapshot chunkSnapshot;
    chunkSnapshot.key = crKey;
    chunkSnapshot.data.span.chunkX = coord.x;
    chunkSnapshot.data.span.chunkY = coord.y;
    chunkSnapshot.data.span.chunkZ = coord.z;
    chunkSnapshot.data.span.offsetX = 0;
    chunkSnapshot.data.span.offsetY = 0;
    chunkSnapshot.data.span.offsetZ = 0;
    chunkSnapshot.data.span.sizeX = 16;
    chunkSnapshot.data.span.sizeY = 16;
    chunkSnapshot.data.span.sizeZ = 16;
    chunkSnapshot.data.blocks.assign(static_cast<size_t>(16 * 16 * 16), Voxel::BlockState{});
    chunkSnapshot.data.blocks[0] = Voxel::BlockState{testId};

    Persistence::ChunkRegionSnapshot region;
    region.key = regionKey;
    region.chunks.push_back(chunkSnapshot);
    format->chunkContainer().saveRegion(region);

    Voxel::World loaded(resources);
    loaded.setId(1);
    loaded.setPersistenceProviders(providers);
    bool loadedFromDisk = Persistence::loadChunkFromDisk(loaded, service, context, coord, 0);
    CHECK(loadedFromDisk);
    if (loadedFromDisk) {
        CHECK_EQ(loaded.getBlock(0, 0, 0).id, testId);
    }

    std::filesystem::remove_all(root);
}
