#include "TestFramework.h"
#include "OpenGLFixture.h"
#include "WorldGenerationTestFixture.h"

#include "Rigel/Asset/AssetManager.h"
#include "Rigel/Persistence/AsyncChunkLoader.h"
#include "Rigel/Persistence/Backends/Memory/MemoryFormat.h"
#include "Rigel/Persistence/PersistenceService.h"
#include "Rigel/Persistence/Storage.h"
#include "Rigel/Voxel/World.h"
#include "Rigel/Voxel/WorldResources.h"
#include "Rigel/Voxel/WorldView.h"

#include <type_traits>
#include <utility>
#include <vector>

using namespace Rigel::Voxel;

template<typename T>
concept PubliclyClearable = requires(T& value) {
    value.clear();
};

static_assert(!std::is_move_constructible_v<ChunkManager>);
static_assert(!std::is_move_assignable_v<ChunkManager>);
static_assert(!std::is_move_constructible_v<World>);
static_assert(!std::is_move_assignable_v<World>);
static_assert(!std::is_move_constructible_v<WorldView>);
static_assert(!std::is_move_assignable_v<WorldView>);
static_assert(std::is_same_v<
              decltype(std::declval<WorldView&>().meshStore()),
              const WorldMeshStore&>);
static_assert(std::is_same_v<
              decltype(std::declval<const WorldView&>().meshStore()),
              const WorldMeshStore&>);
static_assert(!PubliclyClearable<ChunkManager>);
static_assert(!PubliclyClearable<World>);

TEST_CASE(WorldView_StreamingDiagnosticsConsumeLoaderRegionMetrics) {
    WorldResources resources;
    World world(resources);
    auto& registry = resources.registry();
    BlockType solid;
    solid.identifier = "rigel:diagnostic_stone";
    solid.isOpaque = true;
    solid.isSolid = true;
    registry.registerBlock(solid.identifier, solid);
    BlockType surface = solid;
    surface.identifier = "rigel:diagnostic_surface";
    registry.registerBlock(surface.identifier, surface);

    WorldGenConfig generation;
    generation.solidBlock = solid.identifier;
    generation.surfaceBlock = surface.identifier;
    generation.waterBlock = solid.identifier;
    generation.shoreBlock = surface.identifier;
    generation.terrain.baseHeight = 0.0f;
    generation.terrain.heightVariation = 0.0f;
    generation.terrain.surfaceDepth = 1;
    generation.biomes.entries.clear();
    WorldGenConfig::DensityNodeConfig density;
    density.id = "flat_height";
    density.type = "y";
    density.scale = -1.0f;
    density.offset = 0.0f;
    generation.densityGraph.nodes.push_back(std::move(density));
    generation.densityGraph.outputs["base_density"] = "flat_height";
    generation.stageEnabled["caves"] = false;
    generation.stageEnabled["structures"] = false;
    auto generator = Rigel::Test::makeWorldGeneratorFixture(registry, generation);
    world.setGenerator(generator);

    Rigel::Test::TemporaryDirectory directory("rigel_world_diagnostics");
    Rigel::Persistence::FormatRegistry formats;
    formats.registerFormat(
        Rigel::Persistence::Backends::Memory::descriptor(),
        Rigel::Persistence::Backends::Memory::factory(),
        Rigel::Persistence::Backends::Memory::probe());
    Rigel::Persistence::PersistenceService service(formats);
    Rigel::Persistence::PersistenceContext context;
    context.rootPath = directory.path().string();
    context.preferredFormat = "memory";
    context.storage =
        std::make_shared<Rigel::Persistence::FilesystemBackend>();
    auto settings = Rigel::Test::savedWorldSettingsFixture(
        "Diagnostics Test World");
    settings.seed = generation.seed;
    Rigel::Test::installSavedWorldGenerationFixture(
        service,
        context,
        settings,
        Rigel::Test::strictGeneratorDefinitionFixture(generation));
    auto loader = std::make_shared<Rigel::Persistence::AsyncChunkLoader>(
        service,
        context,
        world,
        generator->semanticsVersion(),
        0,
        0,
        0,
        generator);
    loader->setPrefetchRadius(0);

    WorldView view(world, resources);
    view.setGenerator(generator);
    StreamingConfig streaming;
    streaming.viewDistanceChunks = 0;
    streaming.unloadDistanceChunks = 0;
    streaming.genQueueLimit = 0;
    streaming.meshQueueLimit = 0;
    streaming.updateBudgetPerFrame = 0;
    streaming.applyBudgetPerFrame = 0;
    streaming.workerThreads = 0;
    view.setStreamConfig(streaming);
    view.setChunkLoader([loader](ChunkLoadRequest request) {
        return loader->request(request);
    });
    view.setChunkPendingCallback([loader](ChunkCoord coord) {
        return loader->isPending(coord);
    });
    view.setChunkLoadDrain([loader](size_t budget) {
        return loader->drainCompletions(budget);
    });
    view.setChunkLoadCancel([loader](ChunkCoord coord) {
        loader->cancel(coord);
    });
    view.setChunkLoadDiagnosticsCallback([loader]() {
        return loader->diagnostics();
    });
    view.setChunkLoadExecutionStateCallback([loader](ChunkCoord coord) {
        return loader->executionState(coord);
    });
    view.markSpawnDiscoveryComplete();

    view.updateStreaming(glm::vec3(0.0f));

    const auto& diagnostics = view.streamingDiagnostics();
    const auto& direct = diagnostics.regionScheduler.directOrigin;
    CHECK_EQ(direct.logicalAdmissions, static_cast<uint64_t>(1));
    CHECK_EQ(direct.inlineExecutions, static_cast<uint64_t>(1));
    CHECK_EQ(direct.resultsPublished, static_cast<uint64_t>(1));
    CHECK_EQ(direct.resultsDrained, static_cast<uint64_t>(0));
    CHECK_EQ(direct.missingProbes, static_cast<uint64_t>(1));
    CHECK(direct.admissionToWorkerStartNanoseconds > 0);
    CHECK_EQ(direct.maxAdmissionToWorkerStartNanoseconds,
             direct.admissionToWorkerStartNanoseconds);
    CHECK(direct.workerExecutionNanoseconds > 0);
    CHECK_EQ(direct.maxWorkerExecutionNanoseconds,
             direct.workerExecutionNanoseconds);
    CHECK_EQ(diagnostics.regionScheduler.demandOwnedDispatchedUndrained,
             static_cast<size_t>(1));

    view.updateMeshes();
    const auto& drained = view.streamingDiagnostics().regionScheduler;
    CHECK_EQ(drained.directOrigin.resultsPublished,
             static_cast<uint64_t>(1));
    CHECK_EQ(drained.directOrigin.resultsDrained,
             static_cast<uint64_t>(1));
    CHECK_EQ(drained.demandOwnedDispatchedUndrained,
             static_cast<size_t>(0));
}

TEST_CASE(World_StreamingPopulatesChunks) {
    WorldResources resources;
    World world(resources);
    WorldView view(world, resources);

    BlockType solid;
    solid.identifier = "rigel:stone";
    resources.registry().registerBlock(solid.identifier, solid);

    BlockType surface;
    surface.identifier = "rigel:grass";
    resources.registry().registerBlock(surface.identifier, surface);

    WorldGenConfig config;
    config.solidBlock = solid.identifier;
    config.surfaceBlock = surface.identifier;
    config.terrain.baseHeight = 0.0f;
    config.terrain.heightVariation = 0.0f;
    config.terrain.surfaceDepth = 1;
    StreamingConfig streaming;
    streaming.viewDistanceChunks = 0;
    streaming.unloadDistanceChunks = 0;
    streaming.genQueueLimit = 0;
    streaming.meshQueueLimit = 0;
    streaming.applyBudgetPerFrame = 0;
    streaming.workerThreads = 0;

    auto generator =
        Rigel::Test::makeWorldGeneratorFixture(resources.registry(), config);
    world.setGenerator(generator);
    view.setGenerator(generator);
    view.setStreamConfig(streaming);

    view.updateStreaming(glm::vec3(0.0f));
    view.updateMeshes();
    CHECK_EQ(world.chunkManager().loadedChunkCount(), static_cast<size_t>(1));
}

TEST_CASE(WorldView_ClearRestartsRetainedChunkAndMeshStateTogether) {
    WorldResources resources;
    World world(resources);
    WorldView view(world, resources);

    BlockType solid;
    solid.identifier = "rigel:view_clear_solid";
    solid.isOpaque = true;
    solid.isSolid = true;
    const BlockID solidId =
        resources.registry().registerBlock(solid.identifier, solid);

    WorldGenConfig generation;
    generation.solidBlock = solid.identifier;
    generation.surfaceBlock = solid.identifier;
    auto generator = Rigel::Test::makeWorldGeneratorFixture(
        resources.registry(), generation);
    world.setGenerator(generator);
    view.setGenerator(generator);

    StreamingConfig streaming;
    streaming.viewDistanceChunks = 0;
    streaming.unloadDistanceChunks = 0;
    streaming.genQueueLimit = 0;
    streaming.meshQueueLimit = 0;
    streaming.updateBudgetPerFrame = 0;
    streaming.applyBudgetPerFrame = 0;
    streaming.workerThreads = 0;
    streaming.maxResidentChunks = 0;
    view.setStreamConfig(streaming);

    const ChunkCoord coord{0, 0, 0};
    Chunk& chunk = world.chunkManager().getOrCreateChunk(coord);
    chunk.setBlock(0, 0, 0, BlockState{solidId}, resources.registry());
    chunk.setWorldGenVersion(generator->semanticsVersion());
    chunk.setLoadedFromDisk(true);

    view.updateStreaming(coord.toWorldCenter());
    view.updateMeshes();
    CHECK(world.chunkManager().hasChunk(coord));
    CHECK(view.meshStore().contains(coord));

    std::vector<ChunkStreamer::DebugChunkState> states;
    view.getChunkDebugStates(states, coord, 0);
    CHECK_EQ(states.size(), static_cast<size_t>(1));
    CHECK_EQ(states.front().coord, coord);
    CHECK_EQ(states.front().state,
             ChunkStreamer::DebugState::AcceptedNonemptyGeometry);

    view.clear();

    CHECK(world.chunkManager().hasChunk(coord));
    CHECK(!view.meshStore().contains(coord));
    view.getChunkDebugStates(states, coord, 0);
    CHECK(states.empty());

    view.updateStreaming(coord.toWorldCenter());
    view.updateMeshes();

    CHECK(world.chunkManager().hasChunk(coord));
    CHECK(view.meshStore().contains(coord));
    view.getChunkDebugStates(states, coord, 0);
    CHECK_EQ(states.size(), static_cast<size_t>(1));
    CHECK_EQ(states.front().coord, coord);
    CHECK_EQ(states.front().state,
             ChunkStreamer::DebugState::AcceptedNonemptyGeometry);
}

TEST_CASE(WorldView_DebugDrawEvidenceTracksRenderedMeshRevision) {
    Rigel::Test::HiddenOpenGLContext context;
    context.require();

    Rigel::Asset::AssetManager assets;
    assets.loadManifest("manifest.yaml");

    WorldResources resources;
    World world(resources);
    WorldView view(world, resources);
    view.initialize(assets);

    BlockType solid;
    solid.identifier = "rigel:world_view_draw_evidence_solid";
    solid.isOpaque = true;
    solid.isSolid = true;
    const BlockID solidId =
        resources.registry().registerBlock(solid.identifier, solid);

    WorldGenConfig generation;
    generation.solidBlock = solid.identifier;
    generation.surfaceBlock = solid.identifier;
    auto generator = Rigel::Test::makeWorldGeneratorFixture(
        resources.registry(), generation);
    world.setGenerator(generator);
    view.setGenerator(generator);

    StreamingConfig streaming;
    streaming.viewDistanceChunks = 0;
    streaming.unloadDistanceChunks = 0;
    streaming.genQueueLimit = 0;
    streaming.meshQueueLimit = 0;
    streaming.updateBudgetPerFrame = 0;
    streaming.applyBudgetPerFrame = 0;
    streaming.workerThreads = 0;
    streaming.maxResidentChunks = 0;
    view.setStreamConfig(streaming);

    const ChunkCoord coord{0, 0, 0};
    Chunk& chunk = world.chunkManager().getOrCreateChunk(coord);
    chunk.setBlock(0, 0, 0, BlockState{solidId}, resources.registry());
    chunk.setWorldGenVersion(generator->semanticsVersion());
    chunk.setLoadedFromDisk(true);

    view.updateStreaming(coord.toWorldCenter());
    view.updateMeshes();

    std::vector<ChunkStreamer::DebugChunkState> states;
    const auto snapshot = [&]() {
        view.getChunkDebugStates(states, coord, 0);
        CHECK_EQ(states.size(), static_cast<size_t>(1));
        CHECK_EQ(states.front().coord, coord);
        CHECK_EQ(states.front().state,
                 ChunkStreamer::DebugState::AcceptedNonemptyGeometry);
        return states.front();
    };
    const auto render = [&](const glm::vec3& cameraPos) {
        view.render(
            glm::mat4(1.0f),
            glm::mat4(1.0f),
            cameraPos,
            0.1f,
            100.0f);
    };

    const auto beforeFirstDraw = snapshot();
    CHECK(beforeFirstDraw.installedGeometryRevision > 0);
    CHECK_EQ(beforeFirstDraw.drawEvidence,
             ChunkStreamer::DebugDrawEvidence::NotDrawn);

    render(coord.toWorldCenter());
    const auto afterFirstDraw = snapshot();
    CHECK_EQ(afterFirstDraw.installedGeometryRevision,
             beforeFirstDraw.installedGeometryRevision);
    CHECK_EQ(afterFirstDraw.drawEvidence,
             ChunkStreamer::DebugDrawEvidence::Drawn);

    world.setBlock(1, 0, 0, BlockState{solidId});
    view.prioritizeChunkMesh(coord);
    view.updateStreaming(coord.toWorldCenter());
    view.updateMeshes();

    const auto beforeReplacementDraw = snapshot();
    CHECK(beforeReplacementDraw.installedGeometryRevision >
          beforeFirstDraw.installedGeometryRevision);
    CHECK_EQ(beforeReplacementDraw.drawEvidence,
             ChunkStreamer::DebugDrawEvidence::NotDrawn);

    const glm::vec3 outsideRenderDistance =
        coord.toWorldCenter() + glm::vec3(
            view.renderConfig().renderDistance +
                static_cast<float>(Chunk::SIZE),
            0.0f,
            0.0f);
    render(outsideRenderDistance);
    const auto afterReplacementUpload = snapshot();
    CHECK_EQ(afterReplacementUpload.installedGeometryRevision,
             beforeReplacementDraw.installedGeometryRevision);
    CHECK_EQ(afterReplacementUpload.drawEvidence,
             ChunkStreamer::DebugDrawEvidence::NotDrawn);

    render(coord.toWorldCenter());
    const auto afterReplacementDraw = snapshot();
    CHECK_EQ(afterReplacementDraw.installedGeometryRevision,
             beforeReplacementDraw.installedGeometryRevision);
    CHECK_EQ(afterReplacementDraw.drawEvidence,
             ChunkStreamer::DebugDrawEvidence::Drawn);
}

TEST_CASE(WorldView_StreamAndRenderDistancesRemainIndependent) {
    WorldResources resources;
    World world(resources);
    WorldView view(world, resources);

    view.renderConfig().renderDistance = 300.0f;

    StreamingConfig streamConfig;
    streamConfig.viewDistanceChunks = 3;
    view.setStreamConfig(streamConfig);

    CHECK_EQ(view.viewDistanceChunks(), 3);
    CHECK_NEAR(view.renderConfig().renderDistance, 300.0f, 0.0001f);
}

TEST_CASE(WorldView_EditDrivenMeshingMatchesInitialStreaming) {
    struct Result {
        ChunkMesh mesh;
        ChunkStreamer::WorkMetrics metrics;
    };

    auto run = [](bool editAfterInitialMesh) {
        WorldResources resources;
        World world(resources);
        WorldView view(world, resources);

        BlockType solid;
        solid.identifier = "rigel:world_view_mesh_solid";
        BlockID solidId = resources.registry().registerBlock(solid.identifier, solid);

        WorldGenConfig generation;
        generation.solidBlock = solid.identifier;
        generation.surfaceBlock = solid.identifier;
        auto generator = Rigel::Test::makeWorldGeneratorFixture(
            resources.registry(), std::move(generation));
        world.setGenerator(generator);
        view.setGenerator(generator);

        StreamingConfig streaming;
        streaming.viewDistanceChunks = 0;
        streaming.unloadDistanceChunks = 0;
        streaming.genQueueLimit = 0;
        streaming.meshQueueLimit = 0;
        streaming.updateBudgetPerFrame = 0;
        streaming.applyBudgetPerFrame = 0;
        streaming.workerThreads = 0;
        streaming.maxResidentChunks = 0;
        view.setStreamConfig(streaming);

        const ChunkCoord coord{0, 0, 0};
        Chunk& chunk = world.chunkManager().getOrCreateChunk(coord);
        chunk.setBlock(0, 0, 0, BlockState{solidId}, resources.registry());
        if (!editAfterInitialMesh) {
            chunk.setBlock(1, 0, 0, BlockState{solidId}, resources.registry());
        }
        chunk.setWorldGenVersion(generator->semanticsVersion());
        chunk.setLoadedFromDisk(true);

        view.updateStreaming(glm::vec3(0.0f));
        view.updateMeshes();

        if (editAfterInitialMesh) {
            const uint64_t storeVersion = view.meshStore().version();
            const uint64_t jobsStarted = view.streamingMetrics().meshJobsStarted;
            const uint64_t jobsAccepted = view.streamingMetrics().meshJobsAccepted;

            world.setBlock(1, 0, 0, BlockState{solidId});
            view.prioritizeChunkMesh(coord);

            CHECK_EQ(view.meshStore().version(), storeVersion);
            CHECK_EQ(view.streamingMetrics().meshJobsStarted, jobsStarted);
            CHECK_EQ(view.streamingMetrics().meshJobsAccepted, jobsAccepted);

            view.updateStreaming(glm::vec3(0.0f));
            CHECK_EQ(view.meshStore().version(), storeVersion);
            CHECK_EQ(view.streamingMetrics().meshJobsStarted, jobsStarted + 1);
            CHECK_EQ(view.streamingMetrics().meshJobsAccepted, jobsAccepted);
            view.updateMeshes();
            CHECK_EQ(view.streamingMetrics().meshJobsAccepted, jobsAccepted + 1);
        }

        Result result;
        result.metrics = view.streamingMetrics();
        bool found = false;
        view.meshStore().forEach([&](const WorldMeshEntry& entry) {
            if (entry.coord == coord) {
                result.mesh = entry.mesh;
                found = true;
            }
        });
        CHECK(found);

        if (editAfterInitialMesh) {
            const uint64_t storeVersion = view.meshStore().version();
            const uint64_t jobsStarted = view.streamingMetrics().meshJobsStarted;
            const uint64_t jobsAccepted = view.streamingMetrics().meshJobsAccepted;

            world.setBlock(0, 0, 0, BlockState{});
            world.setBlock(1, 0, 0, BlockState{});
            view.prioritizeChunkMesh(coord);

            CHECK(!view.meshStore().contains(coord));
            CHECK_EQ(view.meshStore().version(), storeVersion + 1);
            view.updateStreaming(glm::vec3(0.0f));
            CHECK_EQ(view.meshStore().version(), storeVersion + 1);
            CHECK_EQ(view.streamingMetrics().meshJobsStarted, jobsStarted);
            CHECK_EQ(view.streamingMetrics().meshJobsAccepted, jobsAccepted);
            view.updateMeshes();
            CHECK(!view.meshStore().contains(coord));
            CHECK_EQ(view.streamingMetrics().meshJobsAccepted, jobsAccepted);
        }
        return result;
    };

    Result edited = run(true);
    Result streamed = run(false);

    CHECK_EQ(edited.metrics.meshJobsStarted, static_cast<uint64_t>(2));
    CHECK_EQ(edited.metrics.meshJobsAccepted, static_cast<uint64_t>(2));
    CHECK_EQ(streamed.metrics.meshJobsStarted, static_cast<uint64_t>(1));
    CHECK_EQ(streamed.metrics.meshJobsAccepted, static_cast<uint64_t>(1));
    CHECK_EQ(edited.mesh.vertices.size(), streamed.mesh.vertices.size());
    CHECK_EQ(edited.mesh.indices, streamed.mesh.indices);
    for (size_t i = 0; i < edited.mesh.vertices.size(); ++i) {
        const VoxelVertex& lhs = edited.mesh.vertices[i];
        const VoxelVertex& rhs = streamed.mesh.vertices[i];
        CHECK(lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z);
        CHECK(lhs.u == rhs.u && lhs.v == rhs.v);
        CHECK(lhs.normalIndex == rhs.normalIndex && lhs.aoLevel == rhs.aoLevel);
        CHECK(lhs.textureLayer == rhs.textureLayer && lhs.flags == rhs.flags);
    }
    for (size_t i = 0; i < edited.mesh.layers.size(); ++i) {
        CHECK_EQ(edited.mesh.layers[i].indexStart, streamed.mesh.layers[i].indexStart);
        CHECK_EQ(edited.mesh.layers[i].indexCount, streamed.mesh.layers[i].indexCount);
    }
}
