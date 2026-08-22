#include "TestFramework.h"

#include "Rigel/Asset/AssetManager.h"
#include "Rigel/Entity/EntityModelLoader.h"
#include "Rigel/Persistence/AsyncChunkLoader.h"
#include "Rigel/Persistence/Backends/CR/CRFormat.h"
#include "Rigel/Persistence/Backends/Memory/MemoryFormat.h"
#include "Rigel/Persistence/PersistenceService.h"
#include "Rigel/Persistence/Storage.h"
#include "Rigel/Persistence/WorldPersistence.h"
#include "Rigel/Voxel/BlockType.h"
#include "Rigel/Voxel/World.h"
#include "Rigel/Voxel/WorldResources.h"

#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

using namespace Rigel;

namespace {

class RejectChunkRegionEnumerationStorage final
    : public Persistence::FilesystemBackend {
public:
    void forEachEntry(
        const std::string& path,
        const Persistence::StorageEntryVisitor& visitor) override {
        if (std::filesystem::path(path).filename() == "regions") {
            ++m_chunkRegionEnumerationAttempts;
            throw std::runtime_error(
                "Chunk region enumeration is forbidden during targeted save");
        }
        FilesystemBackend::forEachEntry(path, visitor);
    }

    size_t chunkRegionEnumerationAttempts() const {
        return m_chunkRegionEnumerationAttempts;
    }

private:
    size_t m_chunkRegionEnumerationAttempts = 0;
};

} // namespace

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

TEST_CASE(Persistence_EntityModelIdentifierSurvivesUnavailableAsset) {
    Persistence::FormatRegistry formats;
    formats.registerFormat(
        Persistence::Backends::Memory::descriptor(),
        Persistence::Backends::Memory::factory(),
        Persistence::Backends::Memory::probe());
    Persistence::PersistenceService service(formats);

    Test::TemporaryDirectory directory("rigel_entity_model_persistence");
    auto storage = std::make_shared<Persistence::FilesystemBackend>();
    Persistence::PersistenceContext context;
    context.rootPath = directory.path().string();
    context.preferredFormat = "memory";
    context.storage = storage;

    const Entity::EntityId entityId{10, 20, 30};
    const std::string modelId = "entity_models/demo_cube";
    Persistence::EntityPersistedEntity savedEntity;
    savedEntity.typeId = "rigel:modelled_entity";
    savedEntity.id = entityId;
    savedEntity.position = glm::vec3(1.0f, 2.0f, 3.0f);
    savedEntity.modelId = modelId;

    Persistence::EntityPersistedChunk savedChunk;
    savedChunk.coord = Voxel::ChunkCoord{0, 0, 0};
    savedChunk.entities.push_back(savedEntity);

    const Persistence::EntityRegionKey regionKey{
        "rigel:default", 0, 0, 0};
    Persistence::EntityRegionSnapshot savedRegion;
    savedRegion.key = regionKey;
    savedRegion.chunks.push_back(savedChunk);
    service.saveEntities(savedRegion, context);

    Voxel::WorldResources resources;
    Voxel::World unavailableWorld(resources);
    unavailableWorld.setId(1);
    context.providers = unavailableWorld.persistenceProvidersHandle();
    Asset::AssetManager unavailableAssets;
    Persistence::loadBootstrapEntities(
        unavailableWorld, unavailableAssets, service, context);

    Entity::Entity* unavailableEntity =
        unavailableWorld.entities().get(entityId);
    CHECK(unavailableEntity != nullptr);
    CHECK(!unavailableEntity->model());
    CHECK_EQ(unavailableEntity->modelIdentifier(), modelId);

    Persistence::saveWorldToDisk(unavailableWorld, service, context);
    auto unavailableRoundTrip = service.loadEntities(regionKey, context);
    CHECK_EQ(unavailableRoundTrip.chunks.size(), static_cast<size_t>(1));
    CHECK_EQ(
        unavailableRoundTrip.chunks.front().entities.front().modelId,
        modelId);

    Voxel::World availableWorld(resources);
    availableWorld.setId(1);
    context.providers = availableWorld.persistenceProvidersHandle();
    Asset::AssetManager availableAssets;
    availableAssets.loadManifest("manifest.yaml");
    availableAssets.registerLoader(
        "entity_models", std::make_unique<Entity::EntityModelLoader>());
    availableAssets.registerLoader(
        "entity_anims", std::make_unique<Entity::EntityAnimationSetLoader>());
    Persistence::loadBootstrapEntities(
        availableWorld, availableAssets, service, context);

    Entity::Entity* availableEntity = availableWorld.entities().get(entityId);
    CHECK(availableEntity != nullptr);
    CHECK(availableEntity->model());
    CHECK_EQ(availableEntity->model().id(), modelId);
    CHECK_EQ(availableEntity->modelIdentifier(), modelId);

    availableEntity->setModel({});
    CHECK(availableEntity->modelIdentifier().empty());
    Persistence::saveWorldToDisk(availableWorld, service, context);
    auto clearedRoundTrip = service.loadEntities(regionKey, context);
    CHECK(clearedRoundTrip.chunks.front().entities.front().modelId.empty());
}

TEST_CASE(Persistence_WorldSaveTargetsDirtyRegionsWithoutGlobalEnumeration) {
    for (const std::string formatId : {std::string("memory"), std::string("cr")}) {
        Voxel::WorldResources resources;
        Voxel::BlockType firstBlock;
        firstBlock.identifier = "base:first";
        firstBlock.model = "cube";
        firstBlock.isOpaque = true;
        firstBlock.isSolid = true;
        const std::string firstIdentifier = firstBlock.identifier;
        const auto firstId = resources.registry().registerBlock(
            firstIdentifier, std::move(firstBlock));

        Voxel::BlockType secondBlock;
        secondBlock.identifier = "base:second";
        secondBlock.model = "cube";
        secondBlock.isOpaque = true;
        secondBlock.isSolid = true;
        const std::string secondIdentifier = secondBlock.identifier;
        const auto secondId = resources.registry().registerBlock(
            secondIdentifier, std::move(secondBlock));

        Voxel::BlockType fillerBlock;
        fillerBlock.identifier = "base:filler";
        fillerBlock.model = "cube";
        fillerBlock.isOpaque = true;
        fillerBlock.isSolid = true;
        const std::string fillerIdentifier = fillerBlock.identifier;
        resources.registry().registerBlock(
            fillerIdentifier, std::move(fillerBlock));

        Persistence::FormatRegistry formats;
        formats.registerFormat(
            Persistence::Backends::Memory::descriptor(),
            Persistence::Backends::Memory::factory(),
            Persistence::Backends::Memory::probe());
        formats.registerFormat(
            Persistence::Backends::CR::descriptor(),
            Persistence::Backends::CR::factory(),
            Persistence::Backends::CR::probe());
        Persistence::PersistenceService service(formats);

        Test::TemporaryDirectory directory(
            "rigel_targeted_chunk_save_" + formatId);
        Persistence::PersistenceContext context;
        context.rootPath = directory.path().string();
        context.preferredFormat = formatId;
        context.storage = std::make_shared<Persistence::FilesystemBackend>();

        Voxel::World world(resources);
        world.setId(1);
        context.providers = world.persistenceProvidersHandle();
        const Voxel::ChunkCoord targetCoord{0, 0, 0};
        const Voxel::ChunkCoord siblingCoord{1, 0, 0};
        const Voxel::ChunkCoord absentRegionCoord{32, 0, 0};
        world.setBlock(0, 0, 0, Voxel::BlockState{firstId});
        world.setBlock(
            Voxel::Chunk::SIZE, 0, 0, Voxel::BlockState{firstId});
        Persistence::saveWorldToDisk(world, service, context);
        world.chunkManager().getChunk(targetCoord)->clearPersistDirty();
        world.chunkManager().getChunk(siblingCoord)->clearPersistDirty();
        CHECK(!world.chunkManager().getChunk(targetCoord)->isPersistDirty());
        CHECK(!world.chunkManager().getChunk(siblingCoord)->isPersistDirty());

        auto observedStorage =
            std::make_shared<RejectChunkRegionEnumerationStorage>();
        context.storage = observedStorage;
        world.setBlock(0, 0, 0, Voxel::BlockState{secondId});
        world.setBlock(
            absentRegionCoord.x * Voxel::Chunk::SIZE,
            0,
            0,
            Voxel::BlockState{secondId});
        Persistence::saveWorldToDisk(world, service, context);
        CHECK_EQ(
            observedStorage->chunkRegionEnumerationAttempts(),
            static_cast<size_t>(0));

        Voxel::World loaded(resources);
        loaded.setId(1);
        context.providers = loaded.persistenceProvidersHandle();
        Voxel::WorldGenConfig generatorConfig;
        generatorConfig.solidBlock = fillerIdentifier;
        generatorConfig.surfaceBlock = fillerIdentifier;
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

        const std::vector<Voxel::ChunkCoord> coords{
            targetCoord, siblingCoord, absentRegionCoord};
        for (size_t i = 0; i < coords.size(); ++i) {
            CHECK_EQ(
                loader.request(Voxel::ChunkLoadRequest{coords[i], i + 1}),
                Voxel::ChunkLoadRequestResult::Queued);
        }
        const auto completions = loader.drainCompletions(coords.size());
        CHECK_EQ(completions.size(), coords.size());
        for (const auto& completion : completions) {
            CHECK_EQ(completion.outcome, Voxel::ChunkLoadOutcome::Loaded);
        }

        CHECK_EQ(
            loaded.getBlock(0, 0, 0),
            Voxel::BlockState(secondId));
        CHECK_EQ(
            loaded.getBlock(Voxel::Chunk::SIZE, 0, 0),
            Voxel::BlockState(firstId));
        CHECK_EQ(
            loaded.getBlock(
                absentRegionCoord.x * Voxel::Chunk::SIZE, 0, 0),
            Voxel::BlockState(secondId));
    }
}
