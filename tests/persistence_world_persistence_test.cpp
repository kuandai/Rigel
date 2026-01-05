#include "TestFramework.h"

#include "Rigel/Asset/AssetManager.h"
#include "Rigel/Persistence/Backends/Memory/MemoryFormat.h"
#include "Rigel/Persistence/PersistenceService.h"
#include "Rigel/Persistence/Storage.h"
#include "Rigel/Persistence/WorldPersistence.h"
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
