#include "TestFramework.h"

#include "Rigel/Persistence/AsyncChunkLoader.h"
#include "Rigel/Persistence/Backends/Memory/MemoryFormat.h"
#include "Rigel/Persistence/ChunkSerializer.h"
#include "Rigel/Persistence/PersistenceService.h"
#include "Rigel/Persistence/Storage.h"
#include "Rigel/Voxel/WorldResources.h"
#include "Rigel/Voxel/World.h"
#include "Rigel/Voxel/ChunkStreamer.h"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <random>
#include <thread>
#include <vector>

using namespace Rigel::Voxel;
using namespace Rigel::Persistence;

namespace Rigel::Persistence::detail {
struct AsyncChunkLoaderTestAccess {
    static void setRegionLoadStartCallback(AsyncChunkLoader& loader,
                                           std::function<void()> callback) {
        loader.m_regionLoadStartCallback = std::move(callback);
    }

    static void setPayloadBuildStartCallback(AsyncChunkLoader& loader,
                                             std::function<void()> callback) {
        loader.m_payloadBuildStartCallback = std::move(callback);
    }

    static void setIoPoolStopStartCallback(AsyncChunkLoader& loader,
                                           std::function<void()> callback) {
        loader.m_ioPoolStopStartCallback = std::move(callback);
    }

    static void setWorkerPoolStopStartCallback(AsyncChunkLoader& loader,
                                               std::function<void()> callback) {
        loader.m_workerPoolStopStartCallback = std::move(callback);
    }

    static size_t regionCompletionCount(const AsyncChunkLoader& loader) {
        return loader.m_regionComplete.size();
    }

    static void retainInRegionCompletionQueue(
        AsyncChunkLoader& loader,
        std::shared_ptr<ChunkRegionSnapshot> lifetimeProbe) {
        AsyncChunkLoader::RegionResult result;
        result.entry.region = std::move(lifetimeProbe);
        loader.m_regionComplete.push(std::move(result));
    }
};
}

namespace {
class LoaderWorkGate {
public:
    void enterAndWait() {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_entered = true;
        m_condition.notify_all();
        m_condition.wait(lock, [this]() { return m_released; });
    }

    bool waitUntilEntered() {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_condition.wait_for(
            lock,
            std::chrono::seconds(5),
            [this]() { return m_entered; });
    }

    void release() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_released = true;
        m_condition.notify_all();
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_condition;
    bool m_entered = false;
    bool m_released = false;
};

class LoaderWorkRelease {
public:
    LoaderWorkRelease(std::shared_ptr<LoaderWorkGate> regionGate,
                      std::shared_ptr<LoaderWorkGate> payloadGate)
        : m_regionGate(std::move(regionGate)),
          m_payloadGate(std::move(payloadGate)) {}

    ~LoaderWorkRelease() {
        m_regionGate->release();
        m_payloadGate->release();
    }

private:
    std::shared_ptr<LoaderWorkGate> m_regionGate;
    std::shared_ptr<LoaderWorkGate> m_payloadGate;
};

bool waitForRegionCompletion(const AsyncChunkLoader& loader) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        size_t completionCount = Rigel::Persistence::detail::
            AsyncChunkLoaderTestAccess::regionCompletionCount(loader);
        if (completionCount > 0) {
            return true;
        }
        std::this_thread::yield();
    }
    return Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
        regionCompletionCount(loader) > 0;
}

std::shared_ptr<WorldGenerator> makeGenerator(BlockRegistry& registry) {
    BlockType solid;
    solid.identifier = "rigel:test_solid";
    solid.isOpaque = true;
    solid.isSolid = true;
    registry.registerBlock(solid.identifier, solid);

    BlockType surface;
    surface.identifier = "rigel:test_surface";
    surface.isOpaque = true;
    surface.isSolid = true;
    registry.registerBlock(surface.identifier, surface);

    auto generator = std::make_shared<WorldGenerator>(registry);
    WorldGenConfig config;
    config.seed = 1;
    config.solidBlock = solid.identifier;
    config.surfaceBlock = surface.identifier;
    config.terrain.baseHeight = 64.0f;
    config.terrain.heightVariation = 0.0f;
    config.terrain.surfaceDepth = 1;
    generator->setConfig(config);
    return generator;
}

BlockID registerTestBlock(BlockRegistry& registry, const std::string& identifier) {
    BlockType block;
    block.identifier = identifier;
    block.isOpaque = true;
    block.isSolid = true;
    return registry.registerBlock(identifier, std::move(block));
}

ChunkData buildPayload(ChunkCoord coord,
                       BlockRegistry& registry,
                       const std::vector<BlockID>& palette,
                       bool random,
                       std::optional<ChunkSpan> spanOpt,
                       bool includeMetadata) {
    Chunk chunk(coord);
    ChunkSpan span;
    if (spanOpt) {
        span = *spanOpt;
    } else {
        span.chunkX = coord.x;
        span.chunkY = coord.y;
        span.chunkZ = coord.z;
        span.offsetX = 0;
        span.offsetY = 0;
        span.offsetZ = 0;
        span.sizeX = Chunk::SIZE;
        span.sizeY = Chunk::SIZE;
        span.sizeZ = Chunk::SIZE;
    }

    std::mt19937 rng(1337);
    std::uniform_int_distribution<size_t> blockDist(0, palette.size() - 1);

    for (int z = 0; z < span.sizeZ; ++z) {
        for (int y = 0; y < span.sizeY; ++y) {
            for (int x = 0; x < span.sizeX; ++x) {
                int localX = span.offsetX + x;
                int localY = span.offsetY + y;
                int localZ = span.offsetZ + z;
                size_t paletteIndex = random
                    ? blockDist(rng)
                    : static_cast<size_t>((localX * 3 + localY * 5 + localZ * 7) % palette.size());
                BlockState state;
                state.id = palette[paletteIndex];
                if (includeMetadata && !state.isAir()) {
                    state.metadata = static_cast<uint8_t>((localX + localY * 3 + localZ * 5) & 0xFF);
                    state.lightLevel = static_cast<uint8_t>((localX * 11 + localY * 13 + localZ * 17) & 0xFF);
                }
                chunk.setBlock(localX, localY, localZ, state, registry);
            }
        }
    }

    if (spanOpt) {
        return serializeChunkSpan(chunk, span);
    }
    return serializeChunk(chunk);
}

void verifyPayloadMatches(const Chunk& chunk,
                          const ChunkData& payload) {
    ChunkData decoded;
    bool fullChunk =
        payload.span.offsetX == 0 &&
        payload.span.offsetY == 0 &&
        payload.span.offsetZ == 0 &&
        payload.span.sizeX == Chunk::SIZE &&
        payload.span.sizeY == Chunk::SIZE &&
        payload.span.sizeZ == Chunk::SIZE;
    if (fullChunk) {
        decoded = serializeChunk(chunk);
    } else {
        decoded = serializeChunkSpan(chunk, payload.span);
    }
    CHECK_EQ(decoded.span, payload.span);
    CHECK_EQ(decoded.blocks, payload.blocks);
}

struct MemoryContext {
    FormatRegistry formats;
    PersistenceService service;
    PersistenceContext context;
    std::filesystem::path root;

    MemoryContext()
        : service(formats) {
        formats.registerFormat(
            Backends::Memory::descriptor(),
            Backends::Memory::factory(),
            Backends::Memory::probe());

        auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        root = std::filesystem::temp_directory_path() /
            ("rigel_async_loader_test_" + std::to_string(now));
        std::filesystem::create_directories(root);

        context.rootPath = root.string();
        context.preferredFormat = "memory";
        context.storage = std::make_shared<FilesystemBackend>();
    }

    ~MemoryContext() {
        std::filesystem::remove_all(root);
    }
};

ChunkRegionSnapshot buildRegionSnapshot(const std::string& zoneId,
                                        const ChunkData& payload) {
    ChunkRegionSnapshot region;
    ChunkSnapshot snapshot;
    snapshot.key = ChunkKey{zoneId,
                            payload.span.chunkX,
                            payload.span.chunkY,
                            payload.span.chunkZ};
    snapshot.data = payload;
    region.key = RegionKey{zoneId, 0, 0, 0};
    region.chunks.push_back(snapshot);
    return region;
}

void saveRegionForPayload(PersistenceService& service,
                          PersistenceContext& context,
                          const std::string& zoneId,
                          ChunkCoord coord,
                          const ChunkData& payload) {
    auto format = service.openFormat(context);
    RegionKey regionKey = format->regionLayout().regionForChunk(zoneId, coord);
    ChunkRegionSnapshot region;
    region.key = regionKey;
    ChunkSnapshot snapshot;
    snapshot.key = ChunkKey{zoneId, coord.x, coord.y, coord.z};
    snapshot.data = payload;
    region.chunks.push_back(snapshot);
    format->chunkContainer().saveRegion(region);
}

void saveRegionForPayloads(PersistenceService& service,
                           PersistenceContext& context,
                           const std::string& zoneId,
                           const std::vector<std::pair<ChunkCoord, ChunkData>>& payloads) {
    if (payloads.empty()) {
        return;
    }
    auto format = service.openFormat(context);
    RegionKey regionKey = format->regionLayout().regionForChunk(zoneId, payloads.front().first);
    ChunkRegionSnapshot region;
    region.key = regionKey;
    for (const auto& entry : payloads) {
        ChunkSnapshot snapshot;
        snapshot.key = ChunkKey{zoneId, entry.first.x, entry.first.y, entry.first.z};
        snapshot.data = entry.second;
        region.chunks.push_back(snapshot);
    }
    format->chunkContainer().saveRegion(region);
}
}

TEST_CASE(AsyncChunkLoader_Request_Completes_Deterministic) {
    WorldResources resources;
    World world;
    world.initialize(resources);
    auto& registry = resources.registry();

    auto generator = makeGenerator(registry);
    world.setGenerator(generator);

    BlockID testA = registerTestBlock(registry, "rigel:test_a");
    BlockID testB = registerTestBlock(registry, "rigel:test_b");
    std::vector<BlockID> palette = {BlockRegistry::airId(), testA, testB};

    ChunkCoord coord{0, 0, 0};
    ChunkData payload = buildPayload(coord, registry, palette, false, std::nullopt, true);

    MemoryContext ctx;
    saveRegionForPayload(ctx.service, ctx.context, "rigel:default", coord, payload);

    AsyncChunkLoader loader(
        ctx.service,
        ctx.context,
        world,
        generator->config().world.version,
        0,
        0,
        1,
        generator);

    CHECK_EQ(loader.request(coord), ChunkLoadRequestResult::Queued);
    CHECK(loader.isPending(coord));

    auto resolved = loader.drainCompletions(1);

    Chunk* loaded = world.chunkManager().getChunk(coord);
    CHECK(loaded != nullptr);
    if (loaded) {
        verifyPayloadMatches(*loaded, payload);
    }
    CHECK(!loader.isPending(coord));
    CHECK_EQ(resolved.size(), static_cast<size_t>(1));
    CHECK_EQ(resolved.front(), coord);
}

TEST_CASE(AsyncChunkLoader_Request_Completes_Random) {
    WorldResources resources;
    World world;
    world.initialize(resources);
    auto& registry = resources.registry();

    auto generator = makeGenerator(registry);
    world.setGenerator(generator);

    BlockID testA = registerTestBlock(registry, "rigel:test_rand_a");
    BlockID testB = registerTestBlock(registry, "rigel:test_rand_b");
    std::vector<BlockID> palette = {BlockRegistry::airId(), testA, testB};

    ChunkCoord coord{1, 0, 0};
    ChunkData payload = buildPayload(coord, registry, palette, true, std::nullopt, true);

    MemoryContext ctx;
    saveRegionForPayload(ctx.service, ctx.context, "rigel:default", coord, payload);

    AsyncChunkLoader loader(
        ctx.service,
        ctx.context,
        world,
        generator->config().world.version,
        0,
        0,
        1,
        generator);

    CHECK_EQ(loader.request(coord), ChunkLoadRequestResult::Queued);
    loader.drainCompletions(1);

    Chunk* loaded = world.chunkManager().getChunk(coord);
    CHECK(loaded != nullptr);
    if (loaded) {
        verifyPayloadMatches(*loaded, payload);
    }
}

TEST_CASE(AsyncChunkLoader_ApplyBudget) {
    WorldResources resources;
    World world;
    world.initialize(resources);
    auto& registry = resources.registry();

    auto generator = makeGenerator(registry);
    world.setGenerator(generator);

    BlockID testA = registerTestBlock(registry, "rigel:test_budget_a");
    std::vector<BlockID> palette = {BlockRegistry::airId(), testA};

    ChunkCoord coordA{2, 0, 0};
    ChunkCoord coordB{3, 0, 0};
    ChunkData payloadA = buildPayload(coordA, registry, palette, false, std::nullopt, false);
    ChunkData payloadB = buildPayload(coordB, registry, palette, false, std::nullopt, false);

    MemoryContext ctx;
    saveRegionForPayloads(ctx.service,
                          ctx.context,
                          "rigel:default",
                          {{coordA, payloadA}, {coordB, payloadB}});

    AsyncChunkLoader loader(
        ctx.service,
        ctx.context,
        world,
        generator->config().world.version,
        0,
        0,
        1,
        generator);

    CHECK_EQ(loader.request(coordA), ChunkLoadRequestResult::Queued);
    CHECK_EQ(loader.request(coordB), ChunkLoadRequestResult::Queued);

    loader.drainCompletions(1);

    size_t loadedCount = world.chunkManager().loadedChunkCount();
    CHECK_EQ(loadedCount, static_cast<size_t>(1));

    loader.drainCompletions(4);
    loadedCount = world.chunkManager().loadedChunkCount();
    CHECK_EQ(loadedCount, static_cast<size_t>(2));
}

TEST_CASE(ChunkStreamer_SaturatedLoaderPreservesPersistedChunk) {
    WorldResources resources;
    World world;
    world.initialize(resources);
    auto& registry = resources.registry();

    auto generator = makeGenerator(registry);
    world.setGenerator(generator);

    BlockID persisted = registerTestBlock(registry, "rigel:test_saturated_persisted");
    std::vector<BlockID> palette = {persisted};
    ChunkCoord blocker{0, 0, 0};
    ChunkCoord target{1, 0, 0};
    ChunkData blockerPayload =
        buildPayload(blocker, registry, palette, false, std::nullopt, false);
    ChunkData targetPayload =
        buildPayload(target, registry, palette, false, std::nullopt, false);

    MemoryContext ctx;
    saveRegionForPayloads(ctx.service,
                          ctx.context,
                          "rigel:default",
                          {{blocker, blockerPayload}, {target, targetPayload}});

    auto loader = std::make_shared<AsyncChunkLoader>(
        ctx.service,
        ctx.context,
        world,
        generator->config().world.version,
        0,
        0,
        1,
        generator);
    loader->setLoadQueueLimit(1);
    CHECK_EQ(loader->request(blocker), ChunkLoadRequestResult::Queued);

    WorldMeshStore meshStore;
    ChunkStreamer streamer;
    WorldGenConfig::StreamConfig stream;
    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 0;
    stream.genQueueLimit = 0;
    stream.meshQueueLimit = 0;
    stream.updateBudgetPerFrame = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 0;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    streamer.bind(&world.chunkManager(), &meshStore, &registry, nullptr, generator);
    std::optional<ChunkLoadRequestResult> targetRequest;
    size_t targetRequestCount = 0;
    streamer.setChunkLoader([&, loader](ChunkCoord coord) {
        ChunkLoadRequestResult result = loader->request(coord);
        if (coord == target) {
            targetRequest = result;
            ++targetRequestCount;
        }
        return result;
    });
    streamer.setChunkPendingCallback([loader](ChunkCoord coord) {
        return loader->isPending(coord);
    });
    streamer.setChunkLoadDrain([loader](size_t budget) {
        return loader->drainCompletions(budget);
    });
    streamer.setChunkLoadCancel([loader](ChunkCoord coord) {
        loader->cancel(coord);
    });

    streamer.update(target.toWorldCenter());
    CHECK_EQ(targetRequest, ChunkLoadRequestResult::Deferred);
    CHECK_EQ(streamer.workMetrics().generationJobsStarted,
             static_cast<uint64_t>(0));
    for (int update = 0; update < 3; ++update) {
        streamer.update(target.toWorldCenter());
        CHECK_EQ(targetRequestCount, static_cast<size_t>(1));
        CHECK_EQ(streamer.workMetrics().lastUpdateDesiredBuildCoordinatesInspected,
                 static_cast<uint64_t>(0));
        CHECK_EQ(streamer.workMetrics().lastUpdateSchedulerCoordinatesInspected,
                 static_cast<uint64_t>(0));
    }

    streamer.processCompletions();
    streamer.update(target.toWorldCenter());
    streamer.processCompletions();

    Chunk* loaded = world.chunkManager().getChunk(target);
    CHECK(loaded != nullptr);
    if (!loaded) {
        return;
    }
    CHECK(loaded->loadedFromDisk());
    verifyPayloadMatches(*loaded, targetPayload);
    CHECK_EQ(streamer.workMetrics().generationJobsStarted,
             static_cast<uint64_t>(0));
}

TEST_CASE(AsyncChunkLoader_RegionCapacityStartsDeferredRequests) {
    WorldResources resources;
    World world;
    world.initialize(resources);
    auto& registry = resources.registry();

    auto generator = makeGenerator(registry);
    world.setGenerator(generator);

    BlockID testBlock = registerTestBlock(registry, "rigel:test_region_capacity");
    std::vector<BlockID> palette = {BlockRegistry::airId(), testBlock};
    ChunkCoord coordA{0, 0, 0};
    ChunkCoord coordB{64, 0, 0};
    ChunkData payloadA = buildPayload(coordA, registry, palette, false, std::nullopt, false);
    ChunkData payloadB = buildPayload(coordB, registry, palette, false, std::nullopt, false);

    MemoryContext ctx;
    saveRegionForPayload(ctx.service, ctx.context, "rigel:default", coordA, payloadA);
    saveRegionForPayload(ctx.service, ctx.context, "rigel:default", coordB, payloadB);

    AsyncChunkLoader loader(
        ctx.service,
        ctx.context,
        world,
        generator->config().world.version,
        0,
        0,
        1,
        generator);
    loader.setMaxInFlightRegions(1);
    loader.setPrefetchRadius(0);

    CHECK_EQ(loader.request(coordA), ChunkLoadRequestResult::Queued);
    CHECK_EQ(loader.request(coordB), ChunkLoadRequestResult::Queued);
    CHECK(loader.isPending(coordA));
    CHECK(loader.isPending(coordB));
    auto deferred = loader.workCount();
    CHECK_EQ(deferred.pending, static_cast<size_t>(2));
    CHECK_EQ(deferred.inFlight, static_cast<size_t>(1));
    CHECK_EQ(deferred.started, static_cast<uint64_t>(2));

    auto firstResolved = loader.drainCompletions(1);
    CHECK_EQ(firstResolved.size(), static_cast<size_t>(1));
    CHECK_EQ(firstResolved.front(), coordA);
    CHECK(!loader.isPending(coordA));
    CHECK(loader.isPending(coordB));
    auto secondActive = loader.workCount();
    CHECK_EQ(secondActive.pending, static_cast<size_t>(1));
    CHECK_EQ(secondActive.inFlight, static_cast<size_t>(1));
    CHECK_EQ(secondActive.started, static_cast<uint64_t>(2));

    auto secondResolved = loader.drainCompletions(1);
    CHECK_EQ(secondResolved.size(), static_cast<size_t>(1));
    CHECK_EQ(secondResolved.front(), coordB);
    CHECK(!loader.isPending(coordB));
    auto settled = loader.workCount();
    CHECK_EQ(settled.pending, static_cast<size_t>(0));
    CHECK_EQ(settled.inFlight, static_cast<size_t>(0));
    CHECK_EQ(settled.started, static_cast<uint64_t>(2));
    CHECK(world.chunkManager().getChunk(coordA) != nullptr);
    CHECK(world.chunkManager().getChunk(coordB) != nullptr);
}

TEST_CASE(AsyncChunkLoader_Cancel) {
    WorldResources resources;
    World world;
    world.initialize(resources);
    auto& registry = resources.registry();

    auto generator = makeGenerator(registry);
    world.setGenerator(generator);

    BlockID testA = registerTestBlock(registry, "rigel:test_cancel");
    std::vector<BlockID> palette = {BlockRegistry::airId(), testA};

    ChunkCoord coord{4, 0, 0};
    ChunkData payload = buildPayload(coord, registry, palette, false, std::nullopt, false);

    MemoryContext ctx;
    saveRegionForPayload(ctx.service, ctx.context, "rigel:default", coord, payload);

    AsyncChunkLoader loader(
        ctx.service,
        ctx.context,
        world,
        generator->config().world.version,
        0,
        0,
        1,
        generator);

    CHECK_EQ(loader.request(coord), ChunkLoadRequestResult::Queued);
    loader.cancel(coord);
    CHECK(!loader.isPending(coord));

    loader.drainCompletions(2);

    Chunk* loaded = world.chunkManager().getChunk(coord);
    CHECK(loaded == nullptr);
}

TEST_CASE(AsyncChunkLoader_CancelDeferredRequest) {
    WorldResources resources;
    World world;
    world.initialize(resources);
    auto& registry = resources.registry();

    auto generator = makeGenerator(registry);
    world.setGenerator(generator);

    BlockID testBlock = registerTestBlock(registry, "rigel:test_cancel_deferred");
    std::vector<BlockID> palette = {testBlock};
    ChunkCoord active{0, 0, 0};
    ChunkCoord deferred{1, 0, 0};
    ChunkData activePayload =
        buildPayload(active, registry, palette, false, std::nullopt, false);
    ChunkData deferredPayload =
        buildPayload(deferred, registry, palette, false, std::nullopt, false);

    MemoryContext ctx;
    saveRegionForPayloads(ctx.service,
                          ctx.context,
                          "rigel:default",
                          {{active, activePayload}, {deferred, deferredPayload}});

    AsyncChunkLoader loader(
        ctx.service,
        ctx.context,
        world,
        generator->config().world.version,
        0,
        0,
        1,
        generator);
    loader.setLoadQueueLimit(1);

    CHECK_EQ(loader.request(active), ChunkLoadRequestResult::Queued);
    CHECK_EQ(loader.request(deferred), ChunkLoadRequestResult::Deferred);
    CHECK(loader.isPending(deferred));
    CHECK_EQ(loader.workCount().pending, static_cast<size_t>(2));

    loader.cancel(deferred);
    CHECK(!loader.isPending(deferred));
    CHECK_EQ(loader.workCount().pending, static_cast<size_t>(1));

    auto resolved = loader.drainCompletions(2);
    CHECK_EQ(resolved.size(), static_cast<size_t>(1));
    CHECK_EQ(resolved.front(), active);
    CHECK(world.chunkManager().getChunk(active) != nullptr);
    CHECK(world.chunkManager().getChunk(deferred) == nullptr);
    CHECK_EQ(loader.workCount().pending, static_cast<size_t>(0));
}

TEST_CASE(AsyncChunkLoader_PartialSpan_BaseFill) {
    WorldResources resources;
    World world;
    world.initialize(resources);
    auto& registry = resources.registry();

    auto generator = makeGenerator(registry);
    world.setGenerator(generator);

    BlockID testA = registerTestBlock(registry, "rigel:test_partial");
    std::vector<BlockID> palette = {testA};

    ChunkCoord coord{5, 0, 0};
    ChunkSpan span;
    span.chunkX = coord.x;
    span.chunkY = coord.y;
    span.chunkZ = coord.z;
    span.offsetX = 0;
    span.offsetY = 0;
    span.offsetZ = 0;
    span.sizeX = Chunk::SIZE / 2;
    span.sizeY = Chunk::SIZE / 2;
    span.sizeZ = Chunk::SIZE / 2;

    ChunkData payload = buildPayload(coord, registry, palette, false, span, false);

    MemoryContext ctx;
    saveRegionForPayload(ctx.service, ctx.context, "rigel:default", coord, payload);

    AsyncChunkLoader loader(
        ctx.service,
        ctx.context,
        world,
        generator->config().world.version,
        0,
        0,
        1,
        generator);

    CHECK_EQ(loader.request(coord), ChunkLoadRequestResult::Queued);
    loader.drainCompletions(1);

    Chunk* loaded = world.chunkManager().getChunk(coord);
    CHECK(loaded != nullptr);
    if (!loaded) {
        return;
    }

    BlockState inside = loaded->getBlock(0, 0, 0);
    CHECK_EQ(inside.id, testA);

    BlockState outside = loaded->getBlock(Chunk::SIZE - 1, Chunk::SIZE - 1, Chunk::SIZE - 1);
    CHECK(!outside.isAir());
}

TEST_CASE(AsyncChunkLoader_MissingRegion_UsesNegativeCache) {
    WorldResources resources;
    World world;
    world.initialize(resources);
    auto& registry = resources.registry();

    auto generator = makeGenerator(registry);
    world.setGenerator(generator);

    MemoryContext ctx;
    AsyncChunkLoader loader(
        ctx.service,
        ctx.context,
        world,
        generator->config().world.version,
        0,
        0,
        2,
        generator);

    ChunkCoord missing{123, 4, -77};
    CHECK_EQ(loader.request(missing), ChunkLoadRequestResult::Queued);
    CHECK(loader.isPending(missing));

    auto resolved = loader.drainCompletions(8);

    CHECK(!loader.isPending(missing));
    CHECK(world.chunkManager().getChunk(missing) == nullptr);
    CHECK_EQ(resolved.size(), static_cast<size_t>(1));
    CHECK_EQ(resolved.front(), missing);

    CHECK_EQ(loader.request(missing), ChunkLoadRequestResult::Missing);
}

TEST_CASE(AsyncChunkLoader_DestroyWithInFlightJobs) {
    WorldResources resources;
    World world;
    world.initialize(resources);
    auto& registry = resources.registry();

    auto generator = makeGenerator(registry);
    world.setGenerator(generator);

    BlockID testBlock = registerTestBlock(registry, "rigel:test_destroy");
    std::vector<BlockID> palette = {BlockRegistry::airId(), testBlock};
    ChunkCoord payloadCoord{0, 0, 0};
    ChunkCoord regionCoord{64, 0, 0};
    ChunkData payload =
        buildPayload(payloadCoord, registry, palette, true, std::nullopt, true);
    ChunkData regionPayload =
        buildPayload(regionCoord, registry, palette, true, std::nullopt, true);

    MemoryContext ctx;
    saveRegionForPayload(ctx.service, ctx.context, "rigel:default", payloadCoord, payload);
    saveRegionForPayload(ctx.service, ctx.context, "rigel:default", regionCoord, regionPayload);

    auto loader = std::make_unique<AsyncChunkLoader>(
        ctx.service,
        ctx.context,
        world,
        generator->config().world.version,
        1,
        1,
        4,
        generator);
    loader->setPrefetchRadius(0);

    auto regionGate = std::make_shared<LoaderWorkGate>();
    auto payloadGate = std::make_shared<LoaderWorkGate>();
    LoaderWorkRelease releaseWork(regionGate, payloadGate);
    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::setPayloadBuildStartCallback(
        *loader,
        [payloadGate]() { payloadGate->enterAndWait(); });

    CHECK_EQ(loader->request(payloadCoord), ChunkLoadRequestResult::Queued);
    CHECK(waitForRegionCompletion(*loader));
    loader->drainCompletions(1);
    CHECK(payloadGate->waitUntilEntered());

    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::setRegionLoadStartCallback(
        *loader,
        [regionGate]() { regionGate->enterAndWait(); });
    CHECK_EQ(loader->request(regionCoord), ChunkLoadRequestResult::Queued);
    CHECK(regionGate->waitUntilEntered());

    auto completionQueueDestroyed = std::make_shared<std::promise<void>>();
    auto completionQueueLifetime = completionQueueDestroyed->get_future();
    auto lifetimeProbe = std::shared_ptr<ChunkRegionSnapshot>(
        new ChunkRegionSnapshot(),
        [completionQueueDestroyed](ChunkRegionSnapshot* snapshot) {
            delete snapshot;
            completionQueueDestroyed->set_value();
        });
    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::retainInRegionCompletionQueue(
        *loader,
        std::move(lifetimeProbe));

    auto ioPoolStopStarted = std::make_shared<std::promise<void>>();
    auto ioPoolStopping = ioPoolStopStarted->get_future();
    auto workerPoolStopStarted = std::make_shared<std::promise<void>>();
    auto workerPoolStopping = workerPoolStopStarted->get_future();
    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::setIoPoolStopStartCallback(
        *loader,
        [ioPoolStopStarted]() { ioPoolStopStarted->set_value(); });
    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::setWorkerPoolStopStartCallback(
        *loader,
        [workerPoolStopStarted]() { workerPoolStopStarted->set_value(); });

    auto destruction = std::async(
        std::launch::async,
        [loader = std::move(loader)]() mutable {
            loader.reset();
        });
    LoaderWorkRelease releaseWorkBeforeFutureWait(regionGate, payloadGate);

    CHECK_EQ(ioPoolStopping.wait_for(std::chrono::seconds(5)),
             std::future_status::ready);
    CHECK_EQ(destruction.wait_for(std::chrono::milliseconds(0)),
             std::future_status::timeout);
    CHECK_EQ(completionQueueLifetime.wait_for(std::chrono::milliseconds(0)),
             std::future_status::timeout);

    regionGate->release();
    CHECK_EQ(workerPoolStopping.wait_for(std::chrono::seconds(5)),
             std::future_status::ready);
    CHECK_EQ(destruction.wait_for(std::chrono::milliseconds(0)),
             std::future_status::timeout);
    CHECK_EQ(completionQueueLifetime.wait_for(std::chrono::milliseconds(0)),
             std::future_status::timeout);

    payloadGate->release();
    CHECK_EQ(destruction.wait_for(std::chrono::seconds(5)),
             std::future_status::ready);
    destruction.get();
    CHECK_EQ(completionQueueLifetime.wait_for(std::chrono::milliseconds(0)),
             std::future_status::ready);
}
