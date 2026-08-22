#include "TestFramework.h"

#include "Rigel/Asset/AssetManager.h"
#include "Rigel/Persistence/AsyncChunkLoader.h"
#include "Rigel/Persistence/Backends/Memory/MemoryFormat.h"
#include "Rigel/Persistence/PersistenceService.h"
#include "Rigel/Persistence/Storage.h"
#include "Rigel/Persistence/WorldPersistence.h"
#include "Rigel/Voxel/BlockType.h"
#include "Rigel/Voxel/World.h"
#include "Rigel/Voxel/WorldResources.h"

#include <string>

using namespace Rigel;

TEST_CASE(Persistence_WorldSaveAndAsyncLoad_MemoryFormat) {
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

    Persistence::loadBootstrapEntities(loaded, assets, service, context);
    CHECK_EQ(loaded.chunkManager().loadedChunkCount(), static_cast<size_t>(0));

    Voxel::WorldGenConfig generatorConfig;
    generatorConfig.solidBlock = testIdentifier;
    generatorConfig.surfaceBlock = testIdentifier;
    auto generator = std::make_shared<Voxel::WorldGenerator>(
        resources.registry(), std::move(generatorConfig));
    loaded.setGenerator(generator);
    Persistence::AsyncChunkLoader loader(
        service,
        context,
        loaded,
        generator->config().world.version,
        0,
        0,
        0,
        generator);
    loader.setPrefetchRadius(0);
    const Voxel::ChunkCoord loadedCoord{0, 0, 0};
    const Voxel::ChunkLoadRequest request{loadedCoord, 1};
    CHECK_EQ(
        loader.request(request),
        Voxel::ChunkLoadRequestResult::Queued);
    const auto completions = loader.drainCompletions(1);

    Voxel::BlockState loadedState = loaded.getBlock(0, 0, 0);
    CHECK_EQ(loadedState.id, testId);
    CHECK_EQ(completions.size(), static_cast<size_t>(1));
    CHECK_EQ(completions.front().coord, loadedCoord);
    CHECK_EQ(completions.front().requestId, request.requestId);
    CHECK_EQ(
        completions.front().outcome,
        Voxel::ChunkLoadOutcome::Loaded);
    CHECK(!loaded.chunkManager().getChunk(loadedCoord)->isPersistDirty());
}
