#include "TestFramework.h"

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
        std::make_shared<WorldGenerator>(resources.registry(), config);
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
    auto generator = std::make_shared<WorldGenerator>(
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
    chunk.setWorldGenVersion(generator->config().world.version);
    chunk.setLoadedFromDisk(true);

    view.updateStreaming(coord.toWorldCenter());
    view.updateMeshes();
    CHECK(world.chunkManager().hasChunk(coord));
    CHECK(view.meshStore().contains(coord));

    std::vector<ChunkStreamer::DebugChunkState> states;
    view.getChunkDebugStates(states);
    CHECK_EQ(states.size(), static_cast<size_t>(1));
    CHECK_EQ(states.front().coord, coord);
    CHECK_EQ(states.front().state, ChunkStreamer::DebugState::ReadyMesh);

    view.clear();

    CHECK(world.chunkManager().hasChunk(coord));
    CHECK(!view.meshStore().contains(coord));
    view.getChunkDebugStates(states);
    CHECK(states.empty());

    view.updateStreaming(coord.toWorldCenter());
    view.updateMeshes();

    CHECK(world.chunkManager().hasChunk(coord));
    CHECK(view.meshStore().contains(coord));
    view.getChunkDebugStates(states);
    CHECK_EQ(states.size(), static_cast<size_t>(1));
    CHECK_EQ(states.front().coord, coord);
    CHECK_EQ(states.front().state, ChunkStreamer::DebugState::ReadyMesh);
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
        auto generator = std::make_shared<WorldGenerator>(
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
        chunk.setWorldGenVersion(generator->config().world.version);
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
