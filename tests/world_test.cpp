#include "TestFramework.h"
#include "OpenGLFixture.h"
#include "WorldGenerationTestFixture.h"

#include "ApplicationPreferences.h"
#include "ApplicationTestAccess.h"
#include "Rigel/Asset/AssetManager.h"
#include "Rigel/Persistence/AsyncChunkLoader.h"
#include "Rigel/Persistence/Backends/Memory/MemoryFormat.h"
#include "Rigel/Persistence/PersistenceService.h"
#include "Rigel/Persistence/Storage.h"
#include "Rigel/Preferences/UserPreferences.h"
#include "Rigel/Render/FrameRenderer.h"
#include "Rigel/Voxel/World.h"
#include "Rigel/Voxel/WorldResources.h"
#include "Rigel/Voxel/WorldView.h"

#include <array>
#include <type_traits>
#include <utility>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

using namespace Rigel::Voxel;

template<typename T>
concept PubliclyClearable = requires(T& value) {
    value.clear();
};

template<typename T>
concept PubliclyAppliesViewDistance = requires(T& value) {
    value.applyViewDistanceChunks(7);
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
static_assert(!PubliclyAppliesViewDistance<WorldView>);

namespace {

GeneratorDefinitionData testDefinition(
    const std::string& solid,
    const std::string& surface) {
    return Rigel::Test::generatorDefinitionFixture(solid, surface, solid);
}

} // namespace

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

    GeneratorDefinitionData generation =
        testDefinition(solid.identifier, surface.identifier);
    generation.terrain.densityOutput = "base_density";
    generation.densityGraph.nodes.clear();
    generation.densityGraph.outputs.clear();
    GeneratorDefinitionData::DensityNode density;
    density.id = "flat_height";
    density.type = "y";
    density.scale = -1.0f;
    density.offset = 0.0f;
    generation.densityGraph.nodes.push_back(std::move(density));
    generation.densityGraph.outputs.push_back(
        {"base_density", "flat_height"});
    auto generator = Rigel::Test::makeWorldGeneratorFixture(
        registry, generation, 1u);
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
    settings.seed = generator->seed();
    Rigel::Test::installSavedWorldGenerationFixture(
        service,
        context,
        settings,
        generation);
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

    GeneratorDefinitionData config =
        testDefinition(solid.identifier, surface.identifier);
    StreamingConfig streaming;
    streaming.viewDistanceChunks = 0;
    streaming.unloadDistanceChunks = 0;
    streaming.genQueueLimit = 0;
    streaming.meshQueueLimit = 0;
    streaming.applyBudgetPerFrame = 0;
    streaming.workerThreads = 0;

    auto generator =
        Rigel::Test::makeWorldGeneratorFixture(
            resources.registry(), config, 1u);
    world.setGenerator(generator);
    view.setGenerator(generator);
    view.setStreamConfig(streaming);

    view.updateStreaming(glm::vec3(0.0f));
    view.updateMeshes();
    CHECK_EQ(world.chunkManager().loadedChunkCount(), static_cast<size_t>(1));
}

TEST_CASE(World_GeneratorOwnershipRejectsRuntimeReplacement) {
    WorldResources resources;
    World world(resources);

    GeneratorDefinitionData originalDefinition =
        testDefinition("rigel:owned_solid", "rigel:owned_surface");
    auto original = Rigel::Test::makeWorldGeneratorFixture(
        resources.registry(), originalDefinition, 1u);
    originalDefinition.densityGraph.nodes.front().value = -0.75f;
    auto replacement = Rigel::Test::makeWorldGeneratorFixture(
        resources.registry(),
        std::move(originalDefinition),
        original->seed(),
        original->semanticsVersion());

    world.setGenerator(original);

    CHECK_THROWS(world.setGenerator(replacement));
    CHECK_EQ(world.generator(), original);
}

TEST_CASE(WorldView_UsesWorldOwnedGeneratorIdentity) {
    WorldResources resources;
    World world(resources);
    WorldView view(world, resources);

    GeneratorDefinitionData originalDefinition =
        testDefinition("rigel:view_owned_solid", "rigel:view_owned_surface");
    auto original = Rigel::Test::makeWorldGeneratorFixture(
        resources.registry(), originalDefinition, 1u);
    originalDefinition.densityGraph.nodes.front().value = -0.75f;
    auto replacement = Rigel::Test::makeWorldGeneratorFixture(
        resources.registry(),
        std::move(originalDefinition),
        original->seed(),
        original->semanticsVersion());

    world.setGenerator(original);
    view.setGenerator(original);

    CHECK_THROWS(view.setGenerator(replacement));
    CHECK_EQ(view.generator(), original);
}

TEST_CASE(WorldView_RejectsGeneratorBeforeWorldOwner) {
    WorldResources resources;
    World world(resources);
    WorldView view(world, resources);
    auto generator = Rigel::Test::makeWorldGeneratorFixture(
        resources.registry(),
        testDefinition(
            "rigel:unowned_view_solid",
            "rigel:unowned_view_surface"),
        1u);

    CHECK_THROWS(view.setGenerator(generator));
    CHECK(world.generator() == nullptr);
    CHECK(view.generator() == nullptr);
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

    GeneratorDefinitionData generation =
        testDefinition(solid.identifier, solid.identifier);
    auto generator = Rigel::Test::makeWorldGeneratorFixture(
        resources.registry(), generation, 1u);
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

TEST_CASE(WorldView_ViewPolicyDrivesFrameProjectionAndShadowCeiling) {
    Rigel::Test::HiddenOpenGLContext context;
    context.require();

    Rigel::Asset::AssetManager assets;
    assets.loadManifest("manifest.yaml");
    WorldResources resources;
    resources.initialize(assets);
    World world(resources);
    WorldView view(world, resources);
    view.initialize(assets);

    const auto solidId =
        resources.registry().findByIdentifier("base:stone_shale");
    CHECK(solidId.has_value());
    GeneratorDefinitionData generation =
        testDefinition("base:stone_shale", "base:stone_shale");
    auto generator = Rigel::Test::makeWorldGeneratorFixture(
        resources.registry(), generation, 1u);
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
    chunk.setBlock(
        0, 0, 0, BlockState{*solidId}, resources.registry());
    chunk.setWorldGenVersion(generator->semanticsVersion());
    chunk.setLoadedFromDisk(true);
    view.updateStreaming(coord.toWorldCenter());
    view.updateMeshes();
    CHECK(view.meshStore().contains(coord));

    Rigel::Test::TemporaryDirectory directory(
        "rigel_world_view_policy_render");
    const auto preferencePath =
        directory.path() / "user-preferences.yaml";
    Rigel::Preferences::UserPreferences requested;
    requested.graphics.viewDistanceChunks = 7;
    Rigel::Preferences::UserPreferencesStore(preferencePath)
        .saveRequested(requested);
    Rigel::ApplicationPreferences preferences(preferencePath);
    preferences.load();
    preferences.initializeViewDistance(view);
    const auto startupPolicy = view.viewDistancePolicy();
    CHECK_EQ(
        preferences.requestViewDistance(2).status,
        Rigel::PreferenceApplyStatus::Applied);
    const auto applied =
        Rigel::ApplicationTestAccess::consumeViewDistanceOwnerForTesting(
            preferences, view);
    CHECK(applied.has_value());
    CHECK_EQ(applied->status, Rigel::PreferenceApplyStatus::Applied);
    CHECK_NE(view.viewDistancePolicy(), startupPolicy);
    CHECK_EQ(view.viewDistancePolicy()->generation(), static_cast<uint64_t>(2));

    RenderProfile renderProfile = view.renderProfile();
    renderProfile.shadow.cascades = 1;
    renderProfile.shadow.mapSize = 16;
    renderProfile.shadow.maximumDistanceWorldUnits = 10000.0f;
    view.setRenderProfileForDiagnostics(renderProfile);
    const auto shadowsInitialized = preferences.initializeShadows(view);
    CHECK_EQ(
        shadowsInitialized.status,
        Rigel::PreferenceApplyStatus::Applied);

    constexpr float verticalFovDegrees = 60.0f;
    constexpr float nearPlane = 0.1f;
    const glm::vec3 cameraPosition(8.0f, 8.0f, 24.0f);
    const glm::vec3 cameraTarget(8.0f, 8.0f, 8.0f);
    Rigel::Render::FrameRenderer renderer;
    renderer.setVerticalFovDegrees(verticalFovDegrees);
    renderer.render({
        world,
        view,
        cameraPosition,
        cameraTarget,
        glm::normalize(cameraTarget - cameraPosition),
        64,
        64,
        0.0f,
    });

    const auto shader =
        assets.get<Rigel::Asset::ShaderAsset>("shaders/voxel");
    const GLint projectionLocation = shader->uniform("u_viewProjection");
    CHECK(projectionLocation >= 0);
    std::array<float, 16> actualViewProjection{};
    glGetUniformfv(
        shader->program,
        projectionLocation,
        actualViewProjection.data());
    const glm::mat4 expectedViewProjection =
        glm::perspective(
            glm::radians(verticalFovDegrees),
            1.0f,
            nearPlane,
            view.viewDistancePolicy()->projectionFarPlaneWorldUnits()) *
        glm::lookAt(
            cameraPosition,
            cameraTarget,
            glm::vec3(0.0f, 1.0f, 0.0f));
    const float* expected = glm::value_ptr(expectedViewProjection);
    for (size_t index = 0; index < actualViewProjection.size(); ++index) {
        CHECK_NEAR(actualViewProjection[index], expected[index], 0.0001f);
    }

    const GLint cascadeCountLocation =
        shader->uniform("u_shadowCascadeCount");
    GLint cascadeCount = 0;
    glGetUniformiv(shader->program, cascadeCountLocation, &cascadeCount);
    CHECK_EQ(cascadeCount, 1);
    const GLint shadowFadeLocation = shader->uniform("u_shadowFadeStart");
    GLfloat shadowFadeStart = 0.0f;
    glGetUniformfv(shader->program, shadowFadeLocation, &shadowFadeStart);
    CHECK_NEAR(
        shadowFadeStart,
        view.viewDistancePolicy()->shadowDistanceCeilingWorldUnits(),
        0.0001f);

    view.releaseRenderResources();
    resources.releaseRenderResources();
    assets.clearCache();
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

    GeneratorDefinitionData generation =
        testDefinition(solid.identifier, solid.identifier);
    auto generator = Rigel::Test::makeWorldGeneratorFixture(
        resources.registry(), generation, 1u);
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
            view.renderDistanceWorldUnits() +
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

        GeneratorDefinitionData generation =
            testDefinition(solid.identifier, solid.identifier);
        auto generator = Rigel::Test::makeWorldGeneratorFixture(
            resources.registry(), std::move(generation), 1u);
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
