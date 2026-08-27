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
#include "WorldGenerationTestFixture.h"

#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

using namespace Rigel;

namespace {

Persistence::WorldSettings testWorldSettings() {
    return Test::savedWorldSettingsFixture("Persistence Test World");
}

std::vector<uint8_t> readStorageBytes(
    Persistence::StorageBackend& storage,
    const std::string& path) {
    auto reader = storage.openRead(path);
    return reader->readAt(0, reader->size());
}

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
    Test::installSavedWorldGenerationFixture(
        service, context, testWorldSettings());

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

    Persistence::saveWorldToDisk(
        world, testWorldSettings(), service, context);

    CHECK_EQ(
        service.loadWorldMetadata(context).displayName,
        "Persistence Test World");

    CHECK_EQ(service.loadRegion(archivedChunkRegion.key, context), archivedChunkRegion);
    CHECK_EQ(service.loadEntities(archivedEntityRegion.key, context), archivedEntityRegion);

    Voxel::World loaded(resources);
    loaded.setId(1);
    Asset::AssetManager assets;

    Persistence::loadBootstrapEntities(loaded, assets, service, context);
    CHECK_EQ(loaded.chunkManager().loadedChunkCount(), static_cast<size_t>(0));

    const auto savedGeneration =
        Persistence::loadSavedWorldGeneration(context);
    auto generator = Test::makeWorldGeneratorFixture(
        resources.registry(),
        savedGeneration.definition,
        savedGeneration.settings.seed,
        savedGeneration.settings.generator.semanticsVersion);
    loaded.setGenerator(generator);
    Persistence::AsyncChunkLoader loader(
        service,
        context,
        loaded,
        generator->semanticsVersion(),
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

TEST_CASE(Persistence_CRReloadPreservesContentAndGeneratesFromSavedSnapshot) {
    Voxel::WorldResources resources;
    Voxel::BlockType existingBlock;
    existingBlock.identifier = "rigel:existing_cr_block";
    existingBlock.isOpaque = true;
    existingBlock.isSolid = true;
    const std::string existingIdentifier = existingBlock.identifier;
    const Voxel::BlockID existingId = resources.registry().registerBlock(
        existingIdentifier, std::move(existingBlock));

    Voxel::BlockType generatedBlock;
    generatedBlock.identifier = "rigel:snapshot_generated_block";
    generatedBlock.isOpaque = true;
    generatedBlock.isSolid = true;
    const std::string generatedIdentifier = generatedBlock.identifier;
    const Voxel::BlockID generatedId = resources.registry().registerBlock(
        generatedIdentifier, std::move(generatedBlock));

    Persistence::WorldSettings settings = testWorldSettings();
    settings.displayName = "CR Snapshot Continuity";
    Voxel::GeneratorDefinitionData definition =
        Test::savedGeneratorDefinitionFixture(settings);
    definition.terrain.solidMaterial = generatedIdentifier;
    definition.terrain.waterMaterial = generatedIdentifier;
    definition.biomes.entries.front().surface.front().material =
        generatedIdentifier;
    definition.densityGraph.nodes.front().value = 1.0f;

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
    Test::TemporaryDirectory directory("rigel_cr_snapshot_continuity");
    auto storage = std::make_shared<Persistence::FilesystemBackend>();
    Persistence::PersistenceContext context;
    context.rootPath = (directory.path() / "world_8").string();
    context.preferredFormat = "cr";
    context.storage = storage;

    Voxel::World source(resources);
    source.setId(8);
    context.providers = source.persistenceProvidersHandle();
    const Persistence::NewWorldGenerationFactory creationFactory = [creation =
        Persistence::NewWorldGeneration{
            settings.displayName,
            settings.seed,
            Test::preparedGeneratorFixture(
                definition,
                resources.registry(),
                settings.generator.sourceId,
                settings.generator.sourceRevision)}] {
        return creation;
    };
    const Persistence::BootstrappedWorldGeneration created =
        Persistence::bootstrapWorldGeneration(
            creationFactory,
            service,
            resources.registry(),
            context);
    CHECK_EQ(created.persistenceFormat, std::string("cr"));
    source.setGenerator(Test::makeWorldGeneratorFixture(
        resources.registry(),
        created.generation.definition,
        created.generation.settings.seed,
        created.generation.settings.generator.semanticsVersion));

    const Voxel::ChunkCoord existingCoord{0, 0, 0};
    source.setBlock(0, 0, 0, Voxel::BlockState{existingId});
    auto entity = std::make_unique<Entity::Entity>("rigel:cr_saved_entity");
    entity->setPosition(glm::vec3(2.0f, 3.0f, 4.0f));
    const Entity::EntityId savedEntityId =
        source.entities().spawn(std::move(entity));
    Persistence::saveWorldToDisk(source, settings, service, context);

    Voxel::World loaded(resources);
    loaded.setId(8);
    context.providers = loaded.persistenceProvidersHandle();
    const Persistence::BootstrappedWorldGeneration reopened =
        Persistence::bootstrapWorldGeneration(
            Persistence::NewWorldGenerationFactory{},
            service,
            resources.registry(),
            context);
    CHECK_EQ(reopened.persistenceFormat, std::string("cr"));
    auto savedGenerator = Test::makeWorldGeneratorFixture(
        resources.registry(),
        reopened.generation.definition,
        reopened.generation.settings.seed,
        reopened.generation.settings.generator.semanticsVersion);
    loaded.setGenerator(savedGenerator);
    Asset::AssetManager assets;
    Persistence::loadBootstrapEntities(
        loaded, assets, service, context);
    CHECK(loaded.entities().get(savedEntityId) != nullptr);

    Persistence::AsyncChunkLoader loader(
        service,
        context,
        loaded,
        reopened.generation.settings.generator.semanticsVersion,
        0,
        0,
        2,
        savedGenerator);
    loader.setPrefetchRadius(0);
    const Voxel::ChunkCoord unexploredCoord{1, 0, 0};
    CHECK_EQ(
        loader.request(Voxel::ChunkLoadRequest{existingCoord, 1}),
        Voxel::ChunkLoadRequestResult::Queued);
    CHECK_EQ(
        loader.request(Voxel::ChunkLoadRequest{unexploredCoord, 2}),
        Voxel::ChunkLoadRequestResult::Queued);

    std::vector<Voxel::ChunkLoadCompletion> completions;
    for (size_t attempt = 0; attempt < 8 && completions.size() < 2; ++attempt) {
        auto drained = loader.drainCompletions(2);
        completions.insert(
            completions.end(), drained.begin(), drained.end());
    }
    CHECK_EQ(completions.size(), static_cast<size_t>(2));
    CHECK_EQ(
        loaded.getBlock(0, 0, 0),
        Voxel::BlockState{existingId});
    const auto unexploredCompletion = std::find_if(
        completions.begin(),
        completions.end(),
        [&](const Voxel::ChunkLoadCompletion& completion) {
            return completion.coord == unexploredCoord;
        });
    CHECK(unexploredCompletion != completions.end());
    if (unexploredCompletion == completions.end()) {
        return;
    }
    CHECK_EQ(
        unexploredCompletion->outcome,
        Voxel::ChunkLoadOutcome::Missing);

    Voxel::ChunkBuffer generated;
    savedGenerator->generate(unexploredCoord, generated);
    CHECK_EQ(generated.at(0, 0, 0).id, generatedId);
}

TEST_CASE(Persistence_WorldReloadRetainsDiscoveredFormatForCloseSave) {
    for (const std::string persistedFormat : {
             std::string("memory"), std::string("cr")}) {
        const std::string configuredFormat =
            persistedFormat == "memory" ? "cr" : "memory";
        Test::TemporaryDirectory directory(
            "rigel_discovered_format_" + persistedFormat);
        auto storage =
            std::make_shared<Persistence::FilesystemBackend>();

        Voxel::WorldSet worldSet;
        worldSet.persistenceFormats().registerFormat(
            Persistence::Backends::Memory::descriptor(),
            Persistence::Backends::Memory::factory(),
            Persistence::Backends::Memory::probe());
        worldSet.persistenceFormats().registerFormat(
            Persistence::Backends::CR::descriptor(),
            Persistence::Backends::CR::factory(),
            Persistence::Backends::CR::probe());
        worldSet.setPersistenceRoot(directory.path().string());
        worldSet.setPersistenceStorage(storage);
        worldSet.setPersistencePreferredFormat(configuredFormat);
        Voxel::World& world = worldSet.createWorld(
            Voxel::WorldSet::defaultWorldId());

        Persistence::PersistenceContext creationContext =
            worldSet.persistenceContext(world.id());
        creationContext.preferredFormat = persistedFormat;
        Test::installSavedWorldGenerationFixture(
            worldSet.persistenceService(),
            creationContext,
            testWorldSettings());
        Persistence::saveWorldToDisk(
            world,
            testWorldSettings(),
            worldSet.persistenceService(),
            creationContext);

        Persistence::PersistenceContext wrongFormatContext =
            worldSet.persistenceContext(world.id());
        CHECK_EQ(wrongFormatContext.preferredFormat, configuredFormat);
        Persistence::saveWorldToDisk(
            world,
            testWorldSettings(),
            worldSet.persistenceService(),
            wrongFormatContext);
        CHECK(std::filesystem::exists(
            persistedFormat == "memory"
                ? directory.path() / "world.meta"
                : directory.path() / "worldInfo.json"));
        CHECK(!std::filesystem::exists(
            persistedFormat == "memory"
                ? directory.path() / "worldInfo.json"
                : directory.path() / "world.meta"));

        Persistence::PersistenceContext reloadContext =
            worldSet.persistenceContext(world.id());
        reloadContext.discoverExistingFormat = true;
        auto resolved = worldSet.persistenceService().openFormat(
            reloadContext);
        CHECK_EQ(resolved->descriptor().id, persistedFormat);
        worldSet.setPersistenceActiveFormat(
            world.id(), resolved->descriptor().id);

        Persistence::saveWorldToDisk(
            world,
            testWorldSettings(),
            worldSet.persistenceService(),
            worldSet.persistenceContext(world.id()));

        CHECK(std::filesystem::exists(
            persistedFormat == "memory"
                ? directory.path() / "world.meta"
                : directory.path() / "worldInfo.json"));
        CHECK(!std::filesystem::exists(
            persistedFormat == "memory"
                ? directory.path() / "worldInfo.json"
                : directory.path() / "world.meta"));
    }
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
    Test::installSavedWorldGenerationFixture(
        service, context, testWorldSettings());

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

    Persistence::saveWorldToDisk(
        unavailableWorld, testWorldSettings(), service, context);
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
    Persistence::saveWorldToDisk(
        availableWorld, testWorldSettings(), service, context);
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
        Test::installSavedWorldGenerationFixture(
            service, context, testWorldSettings());
        const Voxel::ChunkCoord targetCoord{0, 0, 0};
        const Voxel::ChunkCoord siblingCoord{1, 0, 0};
        const Voxel::ChunkCoord absentRegionCoord{32, 0, 0};
        world.setBlock(0, 0, 0, Voxel::BlockState{firstId});
        world.setBlock(
            Voxel::Chunk::SIZE, 0, 0, Voxel::BlockState{firstId});
        Persistence::saveWorldToDisk(
            world, testWorldSettings(), service, context);
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
        Persistence::saveWorldToDisk(
            world, testWorldSettings(), service, context);
        CHECK_EQ(
            observedStorage->chunkRegionEnumerationAttempts(),
            static_cast<size_t>(0));

        Voxel::World loaded(resources);
        loaded.setId(1);
        context.providers = loaded.persistenceProvidersHandle();
        const auto savedGeneration =
            Persistence::loadSavedWorldGeneration(context);
        auto generator = Test::makeWorldGeneratorFixture(
            resources.registry(),
            savedGeneration.definition,
            savedGeneration.settings.seed,
            savedGeneration.settings.generator.semanticsVersion);
        loaded.setGenerator(generator);
        Persistence::AsyncChunkLoader loader(
            service,
            context,
            loaded,
            generator->semanticsVersion(),
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
        auto completions = loader.drainCompletions(coords.size());
        auto remaining = loader.drainCompletions(coords.size());
        completions.insert(
            completions.end(), remaining.begin(), remaining.end());
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

TEST_CASE(Persistence_SaveAPIsRejectUnpublishedWorldWithoutMutation) {
    Persistence::FormatRegistry formats;
    formats.registerFormat(
        Persistence::Backends::Memory::descriptor(),
        Persistence::Backends::Memory::factory(),
        Persistence::Backends::Memory::probe());
    Persistence::PersistenceService service(formats);

    Test::TemporaryDirectory parent("rigel_unpublished_world_save");
    const std::filesystem::path root = parent.path() / "world";
    auto storage = std::make_shared<Persistence::FilesystemBackend>();
    Persistence::PersistenceContext context;
    context.rootPath = root.string();
    context.preferredFormat = "memory";
    context.storage = storage;

    Voxel::WorldResources resources;
    Voxel::World world(resources);
    world.setId(1);
    context.providers = world.persistenceProvidersHandle();
    Voxel::Chunk& chunk =
        world.chunkManager().getOrCreateChunk({0, 0, 0});
    chunk.markPersistDirty();

    CHECK_THROWS(Persistence::saveWorldToDisk(
        world, testWorldSettings(), service, context));
    CHECK(!std::filesystem::exists(root));
    CHECK(chunk.isPersistDirty());

    CHECK_THROWS(Persistence::saveChunkToDisk(
        world, service, context, {0, 0, 0}));
    CHECK(!std::filesystem::exists(root));
    CHECK(chunk.isPersistDirty());

    Test::installSavedWorldGenerationDocumentsFixture(
        *storage, context.rootPath, testWorldSettings());
    const std::string settingsPath =
        context.rootPath + "/world-settings.yaml";
    const std::string definitionPath =
        context.rootPath + "/generator-definition.yaml";
    const std::vector<uint8_t> settingsBefore =
        readStorageBytes(*storage, settingsPath);
    const std::vector<uint8_t> definitionBefore =
        readStorageBytes(*storage, definitionPath);
    const std::string abandonedPath =
        context.rootPath + ".staging.0/unowned-sentinel";
    const std::string cleanupPath =
        context.rootPath + ".staging.0.rigel-cleanup";
    Test::writeWorldGenerationFixtureDocument(
        *storage, abandonedPath, "leave staged sibling intact");
    Test::writeWorldGenerationFixtureDocument(
        *storage, cleanupPath, "leave cleanup sibling intact");
    const std::vector<uint8_t> abandonedBefore =
        readStorageBytes(*storage, abandonedPath);
    const std::vector<uint8_t> cleanupBefore =
        readStorageBytes(*storage, cleanupPath);

    CHECK_THROWS(Persistence::saveWorldToDisk(
        world, testWorldSettings(), service, context));
    CHECK_THROWS(Persistence::saveChunkToDisk(
        world, service, context, {0, 0, 0}));
    CHECK_EQ(readStorageBytes(*storage, settingsPath), settingsBefore);
    CHECK_EQ(readStorageBytes(*storage, definitionPath), definitionBefore);
    CHECK_EQ(readStorageBytes(*storage, abandonedPath), abandonedBefore);
    CHECK_EQ(readStorageBytes(*storage, cleanupPath), cleanupBefore);
    CHECK(!std::filesystem::exists(root / "world.meta"));
    CHECK(!std::filesystem::exists(root / "zones"));
    CHECK(chunk.isPersistDirty());

    Persistence::PersistenceContext legacyContext = context;
    const std::filesystem::path legacyRoot = parent.path() / "legacy";
    legacyContext.rootPath = legacyRoot.string();
    service.saveWorldMetadata(
        Persistence::WorldMetadata{
            legacyRoot.filename().string(), "Legacy Metadata Only"},
        legacyContext);
    const std::string legacyMetadataPath =
        legacyContext.rootPath + "/world.meta";
    const std::vector<uint8_t> legacyMetadataBefore =
        readStorageBytes(*storage, legacyMetadataPath);

    CHECK_THROWS(Persistence::saveWorldToDisk(
        world, testWorldSettings(), service, legacyContext));
    CHECK_THROWS(Persistence::saveChunkToDisk(
        world, service, legacyContext, {0, 0, 0}));
    CHECK_EQ(
        readStorageBytes(*storage, legacyMetadataPath),
        legacyMetadataBefore);
    CHECK(!std::filesystem::exists(
        legacyRoot / "world-settings.yaml"));
    CHECK(!std::filesystem::exists(
        legacyRoot / "generator-definition.yaml"));
    CHECK(!std::filesystem::exists(legacyRoot / "zones"));
    CHECK(chunk.isPersistDirty());
}
