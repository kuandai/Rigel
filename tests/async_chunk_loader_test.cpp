#include "TestFramework.h"

#include "Rigel/Persistence/AsyncChunkLoader.h"
#include "Rigel/Persistence/Backends/Memory/MemoryFormat.h"
#include "Rigel/Persistence/ChunkSerializer.h"
#include "Rigel/Persistence/PersistenceService.h"
#include "Rigel/Persistence/Storage.h"
#include "Rigel/Voxel/WorldResources.h"
#include "Rigel/Voxel/World.h"
#include "Rigel/Voxel/ChunkStreamer.h"

#include <algorithm>
#include <atomic>
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

    static size_t payloadCompletionCount(const AsyncChunkLoader& loader) {
        return loader.m_chunkComplete.size();
    }

    static void setRetryClock(
        AsyncChunkLoader& loader,
        std::function<std::chrono::steady_clock::time_point()> clock) {
        loader.m_retryClock = std::move(clock);
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
ChunkLoadRequest makeLoadRequest(ChunkCoord coord) {
    static ChunkLoadRequestId nextRequestId = 1;
    return {coord, nextRequestId++};
}

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

bool waitForPayloadCompletions(const AsyncChunkLoader& loader, size_t count) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        size_t completionCount = Rigel::Persistence::detail::
            AsyncChunkLoaderTestAccess::payloadCompletionCount(loader);
        if (completionCount >= count) {
            return true;
        }
        std::this_thread::yield();
    }
    return Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
        payloadCompletionCount(loader) >= count;
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

    WorldGenConfig config;
    config.seed = 1;
    config.solidBlock = solid.identifier;
    config.surfaceBlock = surface.identifier;
    config.terrain.baseHeight = 64.0f;
    config.terrain.heightVariation = 0.0f;
    config.terrain.surfaceDepth = 1;
    return std::make_shared<WorldGenerator>(registry, std::move(config));
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
    Rigel::Test::TemporaryDirectory directory;
    FormatRegistry formats;
    PersistenceService service;
    PersistenceContext context;

    MemoryContext()
        : directory("rigel_async_loader"),
          service(formats) {
        formats.registerFormat(
            Backends::Memory::descriptor(),
            Backends::Memory::factory(),
            Backends::Memory::probe());

        context.rootPath = directory.path().string();
        context.preferredFormat = "memory";
        context.storage = std::make_shared<FilesystemBackend>();
    }
};

class TransientReadFailureStorage final : public StorageBackend {
public:
    TransientReadFailureStorage(std::shared_ptr<StorageBackend> delegate,
                                size_t failures)
        : m_delegate(std::move(delegate)),
          m_failuresRemaining(failures) {}

    std::unique_ptr<ByteReader> openRead(const std::string& path) override {
        ++m_readAttempts;
        size_t remaining = m_failuresRemaining.load();
        while (remaining > 0) {
            if (m_failuresRemaining.compare_exchange_weak(
                    remaining,
                    remaining - 1)) {
                throw StorageReadError("injected transient read failure");
            }
        }
        return m_delegate->openRead(path);
    }

    std::unique_ptr<AtomicWriteSession> openWrite(
        const std::string& path,
        AtomicWriteOptions options) override {
        return m_delegate->openWrite(path, options);
    }

    bool exists(const std::string& path) override {
        return m_delegate->exists(path);
    }

    std::vector<std::string> list(const std::string& path) override {
        return m_delegate->list(path);
    }

    void mkdirs(const std::string& path) override {
        m_delegate->mkdirs(path);
    }

    void remove(const std::string& path) override {
        m_delegate->remove(path);
    }

    size_t readAttempts() const {
        return m_readAttempts.load();
    }

    void restore() {
        m_failuresRemaining.store(0);
    }

private:
    std::shared_ptr<StorageBackend> m_delegate;
    std::atomic<size_t> m_failuresRemaining;
    std::atomic<size_t> m_readAttempts = 0;
};

class TransientMidReadFailureStorage final : public StorageBackend {
public:
    TransientMidReadFailureStorage(std::shared_ptr<StorageBackend> delegate,
                                   size_t failures)
        : m_delegate(std::move(delegate)),
          m_failuresRemaining(failures) {}

    std::unique_ptr<ByteReader> openRead(const std::string& path) override {
        restoreFile();
        auto reader = m_delegate->openRead(path);
        ++m_readAttempts;

        if (m_originalBytes.empty()) {
            m_path = path;
            m_originalBytes = reader->readAt(0, reader->size());
        }
        if (m_failuresRemaining > 0) {
            --m_failuresRemaining;
            std::filesystem::resize_file(path, 0);
            m_needsRestore = true;
        }
        return reader;
    }

    std::unique_ptr<AtomicWriteSession> openWrite(
        const std::string& path,
        AtomicWriteOptions options) override {
        return m_delegate->openWrite(path, options);
    }

    bool exists(const std::string& path) override {
        return m_delegate->exists(path);
    }

    std::vector<std::string> list(const std::string& path) override {
        return m_delegate->list(path);
    }

    void mkdirs(const std::string& path) override {
        m_delegate->mkdirs(path);
    }

    void remove(const std::string& path) override {
        m_delegate->remove(path);
    }

    size_t readAttempts() const {
        return m_readAttempts;
    }

    void restore() {
        m_failuresRemaining = 0;
        restoreFile();
    }

private:
    void restoreFile() {
        if (!m_needsRestore) {
            return;
        }
        auto session = m_delegate->openWrite(m_path, {});
        session->writer().writeBytes(m_originalBytes.data(), m_originalBytes.size());
        session->commit();
        m_needsRestore = false;
    }

    std::shared_ptr<StorageBackend> m_delegate;
    size_t m_failuresRemaining = 0;
    size_t m_readAttempts = 0;
    std::string m_path;
    std::vector<uint8_t> m_originalBytes;
    bool m_needsRestore = false;
};

class TransientWriteFailureStorage final : public StorageBackend {
public:
    TransientWriteFailureStorage(std::shared_ptr<StorageBackend> delegate,
                                 size_t failures)
        : m_delegate(std::move(delegate)),
          m_failuresRemaining(failures) {}

    std::unique_ptr<ByteReader> openRead(const std::string& path) override {
        return m_delegate->openRead(path);
    }

    std::unique_ptr<AtomicWriteSession> openWrite(
        const std::string& path,
        AtomicWriteOptions options) override {
        ++m_writeAttempts;
        size_t remaining = m_failuresRemaining.load();
        while (remaining > 0) {
            if (m_failuresRemaining.compare_exchange_weak(
                    remaining,
                    remaining - 1)) {
                throw std::runtime_error("injected transient write failure");
            }
        }
        return m_delegate->openWrite(path, options);
    }

    bool exists(const std::string& path) override {
        return m_delegate->exists(path);
    }

    std::vector<std::string> list(const std::string& path) override {
        return m_delegate->list(path);
    }

    void mkdirs(const std::string& path) override {
        m_delegate->mkdirs(path);
    }

    void remove(const std::string& path) override {
        m_delegate->remove(path);
    }

    size_t writeAttempts() const {
        return m_writeAttempts.load();
    }

private:
    std::shared_ptr<StorageBackend> m_delegate;
    std::atomic<size_t> m_failuresRemaining;
    std::atomic<size_t> m_writeAttempts = 0;
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

void writeRawMemoryRegion(
    PersistenceService& service,
    PersistenceContext& context,
    const std::string& zoneId,
    const std::vector<std::pair<ChunkCoord, ChunkData>>& payloads) {
    if (payloads.empty()) {
        return;
    }

    auto format = service.openFormat(context);
    const RegionKey regionKey = format->regionLayout().regionForChunk(
        zoneId, payloads.front().first);
    std::string zoneStoragePath = zoneId;
    std::replace(zoneStoragePath.begin(), zoneStoragePath.end(), ':', '/');
    const std::string directory = context.rootPath + "/zones/" + zoneStoragePath +
        "/regions";
    const std::string path = directory + "/region_" +
        std::to_string(regionKey.x) + "_" + std::to_string(regionKey.y) +
        "_" + std::to_string(regionKey.z) + ".mem";
    context.storage->mkdirs(directory);
    auto session = context.storage->openWrite(path, AtomicWriteOptions{});
    auto& writer = session->writer();
    writer.writeU32(static_cast<uint32_t>(payloads.size()));
    for (const auto& [coord, payload] : payloads) {
        writer.writeI32(coord.x);
        writer.writeI32(coord.y);
        writer.writeI32(coord.z);
        writer.writeI32(payload.span.chunkX);
        writer.writeI32(payload.span.chunkY);
        writer.writeI32(payload.span.chunkZ);
        writer.writeI32(payload.span.offsetX);
        writer.writeI32(payload.span.offsetY);
        writer.writeI32(payload.span.offsetZ);
        writer.writeI32(payload.span.sizeX);
        writer.writeI32(payload.span.sizeY);
        writer.writeI32(payload.span.sizeZ);
        writer.writeU32(static_cast<uint32_t>(payload.blocks.size()));
        for (const auto& block : payload.blocks) {
            writer.writeU16(block.id.type);
            writer.writeU8(block.metadata);
            writer.writeU8(block.lightLevel);
        }
    }
    writer.flush();
    session->commit();
}

void configureStreamerLoader(ChunkStreamer& streamer,
                             const std::shared_ptr<AsyncChunkLoader>& loader) {
    streamer.setChunkLoader([loader](ChunkLoadRequest request) {
        return loader->request(request);
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
    streamer.setChunkLoadWorkCallback([loader]() {
        return loader->workCount();
    });
    streamer.setChunkEvictionCallback([loader](ChunkCoord coord) {
        return loader->persistChunk(coord);
    });
}

void streamChunk(ChunkStreamer& streamer, ChunkCoord coord) {
    streamer.update(coord.toWorldCenter());
    streamer.processCompletions();
    streamer.update(coord.toWorldCenter());
    streamer.processCompletions();
}

void evictCleanChunk(World& world,
                     BlockRegistry& registry,
                     const std::shared_ptr<WorldGenerator>& generator,
                     ChunkCoord coord) {
    WorldMeshStore meshStore;
    ChunkStreamer streamer(
        world.chunkManager(), meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 0;
    stream.genQueueLimit = 0;
    stream.meshQueueLimit = 0;
    stream.updateBudgetPerFrame = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 0;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    streamer.setChunkLoader([](ChunkLoadRequest) {
        return ChunkLoadRequestResult::Deferred;
    });

    streamer.update(coord.offset(2, 0, 0).toWorldCenter());
    CHECK(!world.chunkManager().hasChunk(coord));
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

    const ChunkLoadRequest request = makeLoadRequest(coord);
    CHECK_EQ(loader.request(request), ChunkLoadRequestResult::Queued);
    CHECK(loader.isPending(coord));

    auto resolved = loader.drainCompletions(1);

    Chunk* loaded = world.chunkManager().getChunk(coord);
    CHECK(loaded != nullptr);
    if (loaded) {
        verifyPayloadMatches(*loaded, payload);
    }
    CHECK(!loader.isPending(coord));
    CHECK_EQ(resolved.size(), static_cast<size_t>(1));
    CHECK_EQ(resolved.front().coord, coord);
    CHECK_EQ(resolved.front().requestId, request.requestId);
    CHECK_EQ(resolved.front().outcome, ChunkLoadOutcome::Loaded);
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

    CHECK_EQ(loader.request(makeLoadRequest(coord)), ChunkLoadRequestResult::Queued);
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

    CHECK_EQ(loader.request(makeLoadRequest(coordA)), ChunkLoadRequestResult::Queued);
    CHECK_EQ(loader.request(makeLoadRequest(coordB)), ChunkLoadRequestResult::Queued);

    loader.drainCompletions(1);

    size_t loadedCount = world.chunkManager().loadedChunkCount();
    CHECK_EQ(loadedCount, static_cast<size_t>(1));

    loader.drainCompletions(4);
    loadedCount = world.chunkManager().loadedChunkCount();
    CHECK_EQ(loadedCount, static_cast<size_t>(2));
}

TEST_CASE(ChunkStreamer_EvictionPersistenceSurvivesLoaderAndWorldReload) {
    WorldResources resources;
    World world;
    world.initialize(resources);
    auto& registry = resources.registry();
    auto generator = makeGenerator(registry);
    world.setGenerator(generator);

    BlockID original = registerTestBlock(registry, "rigel:eviction_original");
    const ChunkCoord coord{0, 0, 0};
    ChunkData payload = buildPayload(
        coord,
        registry,
        {original},
        false,
        std::nullopt,
        false);

    MemoryContext ctx;
    saveRegionForPayload(
        ctx.service, ctx.context, "rigel:default", coord, payload);
    auto loader = std::make_shared<AsyncChunkLoader>(
        ctx.service,
        ctx.context,
        world,
        generator->config().world.version,
        0,
        0,
        0,
        generator);
    loader->setPrefetchRadius(0);

    WorldMeshStore meshStore;
    ChunkStreamer streamer(
        world.chunkManager(), meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 0;
    stream.genQueueLimit = 0;
    stream.meshQueueLimit = 0;
    stream.updateBudgetPerFrame = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 0;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    configureStreamerLoader(streamer, loader);

    streamChunk(streamer, coord);
    Chunk* loaded = world.chunkManager().getChunk(coord);
    CHECK(loaded != nullptr);
    if (!loaded) {
        return;
    }
    CHECK_EQ(loaded->getBlock(0, 0, 0).id, original);
    loaded->fill(BlockState{}, registry);
    CHECK(loaded->isPersistDirty());

    const ChunkCoord distant{4, 0, 0};
    streamer.update(distant.toWorldCenter());
    CHECK(!world.chunkManager().hasChunk(coord));

    streamChunk(streamer, coord);
    loaded = world.chunkManager().getChunk(coord);
    CHECK(loaded != nullptr);
    if (!loaded) {
        return;
    }
    CHECK(loaded->getBlock(0, 0, 0).isAir());
    CHECK(!loaded->isPersistDirty());

    World reconstructed;
    reconstructed.initialize(resources);
    reconstructed.setGenerator(generator);
    AsyncChunkLoader reconstructedLoader(
        ctx.service,
        ctx.context,
        reconstructed,
        generator->config().world.version,
        0,
        0,
        0,
        generator);
    reconstructedLoader.setPrefetchRadius(0);
    CHECK_EQ(reconstructedLoader.request(makeLoadRequest(coord)),
             ChunkLoadRequestResult::Queued);
    reconstructedLoader.drainCompletions(4);

    const Chunk* reconstructedChunk = reconstructed.chunkManager().getChunk(coord);
    CHECK(reconstructedChunk != nullptr);
    if (reconstructedChunk) {
        CHECK(reconstructedChunk->getBlock(0, 0, 0).isAir());
        CHECK(!reconstructedChunk->isPersistDirty());
    }
}

TEST_CASE(AsyncChunkLoader_PersistenceInvalidatesInFlightRegionSnapshot) {
    WorldResources resources;
    World world;
    world.initialize(resources);
    auto& registry = resources.registry();
    auto generator = makeGenerator(registry);
    world.setGenerator(generator);

    BlockID original = registerTestBlock(registry, "rigel:inflight_original");
    BlockID edited = registerTestBlock(registry, "rigel:inflight_edit");
    const ChunkCoord coord{0, 0, 0};
    ChunkData payload = buildPayload(
        coord,
        registry,
        {original},
        false,
        std::nullopt,
        false);

    MemoryContext ctx;
    saveRegionForPayload(
        ctx.service, ctx.context, "rigel:default", coord, payload);
    AsyncChunkLoader loader(
        ctx.service,
        ctx.context,
        world,
        generator->config().world.version,
        1,
        0,
        0,
        generator);
    loader.setPrefetchRadius(0);

    auto regionGate = std::make_shared<LoaderWorkGate>();
    auto payloadGate = std::make_shared<LoaderWorkGate>();
    LoaderWorkRelease releaseOnExit(regionGate, payloadGate);
    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
        setRegionLoadStartCallback(loader, [regionGate]() {
            regionGate->enterAndWait();
        });

    CHECK_EQ(loader.request(makeLoadRequest(coord)), ChunkLoadRequestResult::Queued);
    bool loadStarted = regionGate->waitUntilEntered();
    if (!loadStarted) {
        regionGate->release();
    }
    CHECK(loadStarted);

    Chunk& dirty = world.chunkManager().getOrCreateChunk(coord);
    dirty.fill(BlockState{edited}, registry);
    dirty.setWorldGenVersion(generator->config().world.version);
    CHECK(loader.persistChunk(coord));
    CHECK(!dirty.isPersistDirty());
    evictCleanChunk(world, registry, generator, coord);

    regionGate->release();
    CHECK(waitForRegionCompletion(loader));
    loader.drainCompletions(4);
    CHECK(waitForRegionCompletion(loader));
    loader.drainCompletions(4);

    const Chunk* loaded = world.chunkManager().getChunk(coord);
    CHECK(loaded != nullptr);
    if (loaded) {
        CHECK_EQ(loaded->getBlock(0, 0, 0).id, edited);
        CHECK(!loaded->isPersistDirty());
    }
}

TEST_CASE(AsyncChunkLoader_StalePayloadRestartsFromReplacementRegionCache) {
    WorldResources resources;
    World world;
    world.initialize(resources);
    auto& registry = resources.registry();
    auto generator = makeGenerator(registry);
    world.setGenerator(generator);

    BlockID original = registerTestBlock(registry, "rigel:stale_payload_original");
    BlockID edited = registerTestBlock(registry, "rigel:stale_payload_edit");
    const ChunkCoord persistedCoord{0, 0, 0};
    const ChunkCoord staleCoord{1, 0, 0};
    const ChunkCoord refillCoord{2, 0, 0};
    ChunkData persistedPayload = buildPayload(
        persistedCoord, registry, {original}, false, std::nullopt, false);
    ChunkData stalePayload = buildPayload(
        staleCoord, registry, {original}, false, std::nullopt, false);
    ChunkData refillPayload = buildPayload(
        refillCoord, registry, {original}, false, std::nullopt, false);

    MemoryContext ctx;
    saveRegionForPayloads(
        ctx.service,
        ctx.context,
        "rigel:default",
        {{persistedCoord, persistedPayload},
         {staleCoord, stalePayload},
         {refillCoord, refillPayload}});
    AsyncChunkLoader loader(
        ctx.service,
        ctx.context,
        world,
        generator->config().world.version,
        0,
        1,
        0,
        generator);
    loader.setPrefetchRadius(0);

    auto payloadGate = std::make_shared<LoaderWorkGate>();
    auto unusedGate = std::make_shared<LoaderWorkGate>();
    LoaderWorkRelease releaseOnExit(unusedGate, payloadGate);
    std::atomic<size_t> payloadStarts = 0;
    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
        setPayloadBuildStartCallback(loader, [payloadGate, &payloadStarts]() {
            if (payloadStarts.fetch_add(1) == 0) {
                payloadGate->enterAndWait();
            }
        });

    CHECK_EQ(loader.request(makeLoadRequest(staleCoord)), ChunkLoadRequestResult::Queued);
    loader.drainCompletions(1);
    CHECK(payloadGate->waitUntilEntered());

    Chunk& dirty = world.chunkManager().getOrCreateChunk(persistedCoord);
    dirty.fill(BlockState{edited}, registry);
    dirty.setWorldGenVersion(generator->config().world.version);
    CHECK(loader.persistChunk(persistedCoord));
    evictCleanChunk(world, registry, generator, persistedCoord);

    CHECK_EQ(loader.request(makeLoadRequest(refillCoord)), ChunkLoadRequestResult::Queued);
    loader.drainCompletions(1);

    payloadGate->release();
    CHECK(waitForPayloadCompletions(loader, 2));
    std::vector<ChunkLoadCompletion> resolved = loader.drainCompletions(8);
    CHECK_EQ(loader.workCount().inFlight, static_cast<size_t>(1));
    CHECK(waitForPayloadCompletions(loader, 1));
    std::vector<ChunkLoadCompletion> restarted = loader.drainCompletions(8);
    resolved.insert(resolved.end(), restarted.begin(), restarted.end());

    CHECK_EQ(resolved.size(), static_cast<size_t>(2));
    CHECK(!loader.isPending(staleCoord));
    CHECK(!loader.isPending(refillCoord));
    CHECK_EQ(loader.workCount().pending, static_cast<size_t>(0));
    CHECK_EQ(loader.workCount().inFlight, static_cast<size_t>(0));
    CHECK(world.chunkManager().hasChunk(staleCoord));
    CHECK(world.chunkManager().hasChunk(refillCoord));
}

TEST_CASE(AsyncChunkLoader_CancelledPayloadCannotCompleteNewRequest) {
    WorldResources resources;
    World world;
    world.initialize(resources);
    auto& registry = resources.registry();
    auto generator = makeGenerator(registry);
    world.setGenerator(generator);

    BlockID persisted = registerTestBlock(registry, "rigel:cancelled_payload");
    const ChunkCoord coord{0, 0, 0};
    ChunkData payload = buildPayload(
        coord, registry, {persisted}, false, std::nullopt, false);

    MemoryContext ctx;
    saveRegionForPayload(
        ctx.service, ctx.context, "rigel:default", coord, payload);
    AsyncChunkLoader loader(
        ctx.service,
        ctx.context,
        world,
        generator->config().world.version,
        0,
        1,
        0,
        generator);
    loader.setPrefetchRadius(0);

    auto payloadGate = std::make_shared<LoaderWorkGate>();
    auto unusedGate = std::make_shared<LoaderWorkGate>();
    LoaderWorkRelease releaseOnExit(unusedGate, payloadGate);
    std::atomic<size_t> payloadStarts = 0;
    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
        setPayloadBuildStartCallback(loader, [payloadGate, &payloadStarts]() {
            if (payloadStarts.fetch_add(1) == 0) {
                payloadGate->enterAndWait();
            }
        });

    CHECK_EQ(loader.request(makeLoadRequest(coord)), ChunkLoadRequestResult::Queued);
    CHECK(loader.drainCompletions(1).empty());
    CHECK(payloadGate->waitUntilEntered());

    loader.cancel(coord);
    CHECK(!loader.isPending(coord));
    CHECK_EQ(loader.workCount().pending, static_cast<size_t>(0));
    CHECK_EQ(loader.workCount().inFlight, static_cast<size_t>(1));

    CHECK_EQ(loader.request(makeLoadRequest(coord)), ChunkLoadRequestResult::Queued);
    CHECK(loader.isPending(coord));
    CHECK_EQ(loader.workCount().pending, static_cast<size_t>(1));
    CHECK_EQ(loader.workCount().inFlight, static_cast<size_t>(1));

    payloadGate->release();
    CHECK(waitForPayloadCompletions(loader, 1));
    CHECK(loader.drainCompletions(8).empty());
    CHECK(loader.isPending(coord));
    CHECK_EQ(loader.workCount().pending, static_cast<size_t>(1));
    CHECK_EQ(loader.workCount().inFlight, static_cast<size_t>(1));

    CHECK(waitForPayloadCompletions(loader, 1));
    std::vector<ChunkLoadCompletion> resolved = loader.drainCompletions(8);
    CHECK_EQ(resolved.size(), static_cast<size_t>(1));
    CHECK_EQ(resolved.front().coord, coord);
    CHECK_EQ(resolved.front().outcome, ChunkLoadOutcome::Loaded);
    CHECK_EQ(payloadStarts.load(), static_cast<size_t>(2));
    CHECK(!loader.isPending(coord));
    CHECK_EQ(loader.workCount().pending, static_cast<size_t>(0));
    CHECK_EQ(loader.workCount().inFlight, static_cast<size_t>(0));

    const Chunk* loaded = world.chunkManager().getChunk(coord);
    CHECK(loaded != nullptr);
    if (loaded) {
        CHECK_EQ(loaded->getBlock(0, 0, 0).id, persisted);
        CHECK(loaded->loadedFromDisk());
    }
}

TEST_CASE(AsyncChunkLoader_CancelledActiveRequestWakesDeferredCapacity) {
    WorldResources resources;
    World world;
    world.initialize(resources);
    auto& registry = resources.registry();
    auto generator = makeGenerator(registry);
    world.setGenerator(generator);

    BlockID persisted = registerTestBlock(registry, "rigel:cancel_capacity");
    const ChunkCoord active{0, 0, 0};
    const ChunkCoord deferred{1, 0, 0};
    ChunkData activePayload = buildPayload(
        active, registry, {persisted}, false, std::nullopt, false);
    ChunkData deferredPayload = buildPayload(
        deferred, registry, {persisted}, false, std::nullopt, false);

    MemoryContext ctx;
    saveRegionForPayloads(
        ctx.service,
        ctx.context,
        "rigel:default",
        {{active, activePayload}, {deferred, deferredPayload}});
    AsyncChunkLoader loader(
        ctx.service,
        ctx.context,
        world,
        generator->config().world.version,
        0,
        1,
        0,
        generator);
    loader.setPrefetchRadius(0);
    loader.setLoadQueueLimit(1);

    auto payloadGate = std::make_shared<LoaderWorkGate>();
    auto unusedGate = std::make_shared<LoaderWorkGate>();
    LoaderWorkRelease releaseOnExit(unusedGate, payloadGate);
    std::atomic<size_t> payloadStarts = 0;
    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
        setPayloadBuildStartCallback(loader, [payloadGate, &payloadStarts]() {
            if (payloadStarts.fetch_add(1) == 0) {
                payloadGate->enterAndWait();
            }
        });

    CHECK_EQ(loader.request(makeLoadRequest(active)), ChunkLoadRequestResult::Queued);
    CHECK(loader.drainCompletions(1).empty());
    CHECK(payloadGate->waitUntilEntered());
    const ChunkLoadRequest deferredRequest = makeLoadRequest(deferred);
    CHECK_EQ(loader.request(deferredRequest), ChunkLoadRequestResult::Deferred);
    CHECK_EQ(loader.workCount().pending, static_cast<size_t>(2));
    CHECK_EQ(loader.workCount().inFlight, static_cast<size_t>(1));

    loader.cancel(active);
    CHECK(!loader.isPending(active));
    CHECK_EQ(loader.request(deferredRequest), ChunkLoadRequestResult::Queued);
    CHECK_EQ(loader.workCount().pending, static_cast<size_t>(1));
    CHECK_EQ(loader.workCount().inFlight, static_cast<size_t>(2));

    payloadGate->release();
    CHECK(waitForPayloadCompletions(loader, 2));
    std::vector<ChunkLoadCompletion> resolved = loader.drainCompletions(8);
    CHECK_EQ(resolved.size(), static_cast<size_t>(1));
    CHECK_EQ(resolved.front().coord, deferred);
    CHECK_EQ(resolved.front().outcome, ChunkLoadOutcome::Loaded);
    CHECK_EQ(payloadStarts.load(), static_cast<size_t>(2));
    CHECK(!loader.isPending(deferred));
    CHECK_EQ(loader.workCount().pending, static_cast<size_t>(0));
    CHECK_EQ(loader.workCount().inFlight, static_cast<size_t>(0));
    CHECK(!world.chunkManager().hasChunk(active));
    CHECK(world.chunkManager().hasChunk(deferred));
}

TEST_CASE(ChunkStreamer_ResidentReplacementCancelsPendingPayload) {
    WorldResources resources;
    World world;
    world.initialize(resources);
    auto& registry = resources.registry();
    auto generator = makeGenerator(registry);
    world.setGenerator(generator);

    BlockID persisted = registerTestBlock(registry, "rigel:resident_persisted");
    BlockID generated = registerTestBlock(registry, "rigel:resident_generated");
    const ChunkCoord coord{0, 0, 0};
    ChunkData payload = buildPayload(
        coord, registry, {persisted}, false, std::nullopt, false);

    MemoryContext ctx;
    saveRegionForPayload(
        ctx.service, ctx.context, "rigel:default", coord, payload);
    auto loader = std::make_shared<AsyncChunkLoader>(
        ctx.service,
        ctx.context,
        world,
        generator->config().world.version,
        0,
        1,
        0,
        generator);
    loader->setPrefetchRadius(0);

    auto payloadGate = std::make_shared<LoaderWorkGate>();
    auto unusedGate = std::make_shared<LoaderWorkGate>();
    LoaderWorkRelease releaseOnExit(unusedGate, payloadGate);
    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
        setPayloadBuildStartCallback(*loader, [payloadGate]() {
            payloadGate->enterAndWait();
        });

    WorldMeshStore meshStore;
    ChunkStreamer streamer(
        world.chunkManager(), meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 0;
    stream.genQueueLimit = 0;
    stream.meshQueueLimit = 0;
    stream.updateBudgetPerFrame = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 0;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    configureStreamerLoader(streamer, loader);
    streamer.setChunkLoadWorkCallback([loader]() {
        return loader->workCount();
    });

    streamer.update(coord.toWorldCenter());
    streamer.processCompletions();
    CHECK(payloadGate->waitUntilEntered());
    CHECK(loader->isPending(coord));
    CHECK_EQ(loader->workCount().pending, static_cast<size_t>(1));
    CHECK_EQ(loader->workCount().inFlight, static_cast<size_t>(1));

    Chunk& replacement = world.chunkManager().getOrCreateChunk(coord);
    replacement.fill(BlockState{generated}, registry);
    replacement.setWorldGenVersion(generator->config().world.version);
    replacement.setLoadedFromDisk(false);
    replacement.clearPersistDirty();
    replacement.clearDirty();

    streamer.update(coord.toWorldCenter());
    CHECK(!loader->isPending(coord));
    CHECK_EQ(loader->workCount().pending, static_cast<size_t>(0));
    CHECK_EQ(loader->workCount().inFlight, static_cast<size_t>(1));

    payloadGate->release();
    CHECK(waitForPayloadCompletions(*loader, 1));
    streamer.processCompletions();

    const Chunk* resident = world.chunkManager().getChunk(coord);
    CHECK(resident != nullptr);
    if (resident) {
        CHECK_EQ(resident->getBlock(0, 0, 0).id, generated);
        CHECK_EQ(resident->worldGenVersion(), generator->config().world.version);
        CHECK(!resident->loadedFromDisk());
        CHECK(!resident->isPersistDirty());
    }
    CHECK_EQ(loader->workCount().pending, static_cast<size_t>(0));
    CHECK_EQ(loader->workCount().inFlight, static_cast<size_t>(0));
    CHECK(streamer.diagnostics().chunkLoad.empty());
}

TEST_CASE(ChunkStreamer_FailedEvictionPersistenceRetainsDirtyChunkUntilRetry) {
    WorldResources resources;
    World world;
    world.initialize(resources);
    auto& registry = resources.registry();
    auto generator = makeGenerator(registry);
    world.setGenerator(generator);

    BlockID original = registerTestBlock(registry, "rigel:failed_save_original");
    BlockID edited = registerTestBlock(registry, "rigel:failed_save_edit");
    const ChunkCoord coord{0, 0, 0};
    ChunkData payload = buildPayload(
        coord,
        registry,
        {original},
        false,
        std::nullopt,
        false);

    MemoryContext ctx;
    saveRegionForPayload(
        ctx.service, ctx.context, "rigel:default", coord, payload);
    auto failingStorage = std::make_shared<TransientWriteFailureStorage>(
        ctx.context.storage,
        1);
    ctx.context.storage = failingStorage;
    auto loader = std::make_shared<AsyncChunkLoader>(
        ctx.service,
        ctx.context,
        world,
        generator->config().world.version,
        0,
        0,
        0,
        generator);
    loader->setPrefetchRadius(0);

    WorldMeshStore meshStore;
    ChunkStreamer streamer(
        world.chunkManager(), meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 0;
    stream.genQueueLimit = 0;
    stream.meshQueueLimit = 0;
    stream.updateBudgetPerFrame = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 0;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    configureStreamerLoader(streamer, loader);

    streamChunk(streamer, coord);
    Chunk* loaded = world.chunkManager().getChunk(coord);
    CHECK(loaded != nullptr);
    if (!loaded) {
        return;
    }
    loaded->setBlock(0, 0, 0, BlockState{edited}, registry);

    const glm::vec3 distant = ChunkCoord{4, 0, 0}.toWorldCenter();
    streamer.update(distant);
    loaded = world.chunkManager().getChunk(coord);
    CHECK(loaded != nullptr);
    if (!loaded) {
        return;
    }
    CHECK(loaded->isPersistDirty());
    CHECK_EQ(loaded->getBlock(0, 0, 0).id, edited);
    CHECK_EQ(failingStorage->writeAttempts(), static_cast<size_t>(1));

    for (int update = 0; update < 59; ++update) {
        streamer.update(distant);
    }
    CHECK(world.chunkManager().hasChunk(coord));
    CHECK_EQ(failingStorage->writeAttempts(), static_cast<size_t>(1));

    streamer.update(distant);
    CHECK(!world.chunkManager().hasChunk(coord));
    CHECK_EQ(failingStorage->writeAttempts(), static_cast<size_t>(2));
}

TEST_CASE(ChunkStreamer_VersionReplacementPersistsEditedChunkBeforeRegeneration) {
    WorldResources resources;
    World world;
    world.initialize(resources);
    auto& registry = resources.registry();
    auto generator = makeGenerator(registry);
    world.setGenerator(generator);

    BlockID original = registerTestBlock(registry, "rigel:version_original");
    BlockID edited = registerTestBlock(registry, "rigel:version_edit");
    const ChunkCoord coord{0, 0, 0};
    ChunkData payload = buildPayload(
        coord,
        registry,
        {original},
        false,
        std::nullopt,
        false);

    MemoryContext ctx;
    saveRegionForPayload(
        ctx.service, ctx.context, "rigel:default", coord, payload);
    auto failingStorage = std::make_shared<TransientWriteFailureStorage>(
        ctx.context.storage,
        1);
    ctx.context.storage = failingStorage;
    auto loader = std::make_shared<AsyncChunkLoader>(
        ctx.service,
        ctx.context,
        world,
        generator->config().world.version,
        0,
        0,
        0,
        generator);
    loader->setPrefetchRadius(0);

    WorldMeshStore meshStore;
    ChunkStreamer streamer(
        world.chunkManager(), meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 1;
    stream.genQueueLimit = 0;
    stream.meshQueueLimit = 0;
    stream.updateBudgetPerFrame = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 0;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);

    size_t loadRequests = 0;
    streamer.setChunkLoader([loader, &loadRequests](ChunkLoadRequest request) {
        ++loadRequests;
        return loader->request(request);
    });
    streamer.setChunkPendingCallback([loader](ChunkCoord request) {
        return loader->isPending(request);
    });
    streamer.setChunkLoadDrain([loader](size_t budget) {
        return loader->drainCompletions(budget);
    });
    streamer.setChunkLoadCancel([loader](ChunkCoord request) {
        loader->cancel(request);
    });
    streamer.setChunkEvictionCallback([loader](ChunkCoord request) {
        return loader->persistChunk(request);
    });

    for (int index = 0; index < DirectionCount; ++index) {
        int dx = 0;
        int dy = 0;
        int dz = 0;
        directionOffset(static_cast<Direction>(index), dx, dy, dz);
        Chunk& neighbor = world.chunkManager().getOrCreateChunk(
            coord.offset(dx, dy, dz));
        neighbor.setWorldGenVersion(generator->config().world.version);
        neighbor.clearDirty();
    }

    streamChunk(streamer, coord);
    Chunk* loaded = world.chunkManager().getChunk(coord);
    CHECK(loaded != nullptr);
    if (!loaded) {
        return;
    }
    CHECK_EQ(loaded->getBlock(0, 0, 0).id, original);
    CHECK(meshStore.contains(coord));
    loaded->setBlock(0, 0, 0, BlockState{edited}, registry);

    const size_t settledLoadRequests = loadRequests;
    const uint64_t settledGenerationJobs =
        streamer.workMetrics().generationJobsStarted;
    WorldGenConfig changedConfig = generator->config();
    ++changedConfig.world.version;
    generator = std::make_shared<WorldGenerator>(
        registry, std::move(changedConfig));
    world.setGenerator(generator);
    streamer.setGenerator(generator);

    streamer.update(coord.toWorldCenter());

    loaded = world.chunkManager().getChunk(coord);
    CHECK(loaded != nullptr);
    if (!loaded) {
        return;
    }
    CHECK(loaded->isPersistDirty());
    CHECK_EQ(loaded->getBlock(0, 0, 0).id, edited);
    CHECK(meshStore.contains(coord));
    CHECK_EQ(failingStorage->writeAttempts(), static_cast<size_t>(1));
    CHECK_EQ(streamer.workMetrics().generationJobsStarted,
             settledGenerationJobs);

    std::vector<ChunkStreamer::DebugChunkState> states;
    streamer.getDebugStates(states);
    CHECK(std::any_of(states.begin(), states.end(), [coord](const auto& state) {
        return state.coord == coord;
    }));

    for (int update = 0; update < 59; ++update) {
        streamer.update(coord.toWorldCenter());
    }
    CHECK(world.chunkManager().hasChunk(coord));
    CHECK(meshStore.contains(coord));
    CHECK_EQ(failingStorage->writeAttempts(), static_cast<size_t>(1));
    CHECK_EQ(streamer.workMetrics().generationJobsStarted,
             settledGenerationJobs);

    streamer.update(coord.toWorldCenter());
    CHECK(!world.chunkManager().hasChunk(coord));
    CHECK(!meshStore.contains(coord));
    CHECK_EQ(failingStorage->writeAttempts(), static_cast<size_t>(2));
    CHECK_EQ(streamer.workMetrics().generationJobsStarted,
             settledGenerationJobs + 1);
    CHECK_EQ(loadRequests, settledLoadRequests);

    streamer.processCompletions();
    streamer.update(coord.toWorldCenter());
    streamer.processCompletions();
    Chunk* replacement = world.chunkManager().getChunk(coord);
    CHECK(replacement != nullptr);
    if (replacement) {
        CHECK_EQ(replacement->worldGenVersion(), generator->config().world.version);
        CHECK_NE(replacement->getBlock(0, 0, 0).id, edited);
        CHECK(!replacement->loadedFromDisk());
    }
    CHECK(meshStore.contains(coord));
    CHECK_EQ(loadRequests, settledLoadRequests);

    World reconstructed;
    reconstructed.initialize(resources);
    reconstructed.setGenerator(generator);
    AsyncChunkLoader reconstructedLoader(
        ctx.service,
        ctx.context,
        reconstructed,
        generator->config().world.version,
        0,
        0,
        0,
        generator);
    reconstructedLoader.setPrefetchRadius(0);
    CHECK_EQ(reconstructedLoader.request(makeLoadRequest(coord)),
             ChunkLoadRequestResult::Queued);
    reconstructedLoader.drainCompletions(4);

    const Chunk* recovered = reconstructed.chunkManager().getChunk(coord);
    CHECK(recovered != nullptr);
    if (recovered) {
        CHECK_EQ(recovered->getBlock(0, 0, 0).id, edited);
        CHECK(!recovered->isPersistDirty());
    }
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
    CHECK_EQ(loader->request(makeLoadRequest(blocker)), ChunkLoadRequestResult::Queued);

    WorldMeshStore meshStore;
    ChunkStreamer streamer(
        world.chunkManager(), meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 0;
    stream.genQueueLimit = 0;
    stream.meshQueueLimit = 0;
    stream.updateBudgetPerFrame = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 0;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    std::optional<ChunkLoadRequestResult> targetRequest;
    size_t targetRequestCount = 0;
    streamer.setChunkLoader([&, loader](ChunkLoadRequest request) {
        ChunkLoadRequestResult result = loader->request(request);
        if (request.coord == target) {
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

TEST_CASE(ChunkStreamer_TransientRegionFailurePreservesPersistedChunk) {
    WorldResources resources;
    World world;
    world.initialize(resources);
    auto& registry = resources.registry();

    auto generator = makeGenerator(registry);
    world.setGenerator(generator);

    BlockID persisted = registerTestBlock(registry, "rigel:test_retry_persisted");
    std::vector<BlockID> palette = {persisted};
    ChunkCoord coord{0, 0, 0};
    ChunkData payload =
        buildPayload(coord, registry, palette, false, std::nullopt, false);

    MemoryContext ctx;
    saveRegionForPayload(ctx.service,
                         ctx.context,
                         "rigel:default",
                         coord,
                         payload);
    auto failingStorage = std::make_shared<TransientReadFailureStorage>(
        ctx.context.storage,
        1);
    ctx.context.storage = failingStorage;

    auto loader = std::make_shared<AsyncChunkLoader>(
        ctx.service,
        ctx.context,
        world,
        generator->config().world.version,
        0,
        0,
        1,
        generator);
    loader->setPrefetchRadius(0);

    WorldMeshStore meshStore;
    ChunkStreamer streamer(
        world.chunkManager(), meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 0;
    stream.genQueueLimit = 0;
    stream.meshQueueLimit = 0;
    stream.updateBudgetPerFrame = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 0;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    streamer.setChunkLoader([loader](ChunkLoadRequest request) {
        return loader->request(request);
    });
    streamer.setChunkPendingCallback([loader](ChunkCoord request) {
        return loader->isPending(request);
    });
    streamer.setChunkLoadDrain([loader](size_t budget) {
        return loader->drainCompletions(budget);
    });
    streamer.setChunkLoadCancel([loader](ChunkCoord request) {
        loader->cancel(request);
    });

    streamer.update(coord.toWorldCenter());
    streamer.processCompletions();
    streamer.update(coord.toWorldCenter());
    streamer.processCompletions();

    Chunk* loaded = world.chunkManager().getChunk(coord);
    CHECK(loaded != nullptr);
    if (!loaded) {
        return;
    }
    CHECK(loaded->loadedFromDisk());
    verifyPayloadMatches(*loaded, payload);
    CHECK_EQ(failingStorage->readAttempts(), static_cast<size_t>(2));
    CHECK_EQ(streamer.workMetrics().generationJobsStarted,
             static_cast<uint64_t>(0));
}

TEST_CASE(ChunkStreamer_ExhaustedRegionReadsRecoverWithoutCameraMovement) {
    WorldResources resources;
    World world;
    world.initialize(resources);
    auto& registry = resources.registry();

    auto generator = makeGenerator(registry);
    world.setGenerator(generator);

    BlockID persisted = registerTestBlock(registry, "rigel:test_failed_region");
    std::vector<BlockID> palette = {persisted};
    ChunkCoord coord{0, 0, 0};
    ChunkData payload =
        buildPayload(coord, registry, palette, false, std::nullopt, false);

    MemoryContext ctx;
    saveRegionForPayload(ctx.service,
                         ctx.context,
                         "rigel:default",
                         coord,
                         payload);
    auto failingStorage = std::make_shared<TransientReadFailureStorage>(
        ctx.context.storage,
        100);
    ctx.context.storage = failingStorage;

    auto loader = std::make_shared<AsyncChunkLoader>(
        ctx.service,
        ctx.context,
        world,
        generator->config().world.version,
        0,
        0,
        1,
        generator);
    loader->setPrefetchRadius(0);
    auto retryNow = std::chrono::steady_clock::now();
    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::setRetryClock(
        *loader,
        [&retryNow]() { return retryNow; });

    WorldMeshStore meshStore;
    ChunkStreamer streamer(
        world.chunkManager(), meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 0;
    stream.genQueueLimit = 0;
    stream.meshQueueLimit = 0;
    stream.updateBudgetPerFrame = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 0;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    configureStreamerLoader(streamer, loader);
    streamer.markSpawnDiscoveryComplete();

    streamer.update(coord.toWorldCenter());
    streamer.processCompletions();
    CHECK_EQ(failingStorage->readAttempts(), static_cast<size_t>(3));
    CHECK(loader->isPending(coord));
    CHECK(world.chunkManager().getChunk(coord) == nullptr);
    CHECK_EQ(loader->workCount().pending, static_cast<size_t>(1));
    CHECK_EQ(loader->workCount().inFlight, static_cast<size_t>(0));
    CHECK_EQ(loader->workCount().terminalErrors, static_cast<size_t>(0));
    CHECK_EQ(streamer.diagnostics().state, StreamingLifecycleState::Streaming);
    CHECK_EQ(streamer.workMetrics().generationJobsStarted,
             static_cast<uint64_t>(0));

    for (int update = 0; update < 4; ++update) {
        streamer.update(coord.toWorldCenter());
        streamer.processCompletions();
    }
    CHECK_EQ(failingStorage->readAttempts(), static_cast<size_t>(3));
    CHECK_EQ(streamer.workMetrics().lastUpdateDesiredBuildCoordinatesInspected,
             static_cast<uint64_t>(0));
    CHECK_EQ(streamer.workMetrics().lastUpdateSchedulerCoordinatesInspected,
             static_cast<uint64_t>(0));

    failingStorage->restore();
    retryNow += std::chrono::seconds(1);
    streamer.processCompletions();

    Chunk* loaded = world.chunkManager().getChunk(coord);
    CHECK(loaded != nullptr);
    if (!loaded) {
        return;
    }
    CHECK(loaded->loadedFromDisk());
    verifyPayloadMatches(*loaded, payload);
    CHECK_EQ(failingStorage->readAttempts(), static_cast<size_t>(4));
    CHECK_EQ(streamer.workMetrics().generationJobsStarted,
             static_cast<uint64_t>(0));
    CHECK(!loader->isPending(coord));
    CHECK_EQ(loader->workCount().pending, static_cast<size_t>(0));
}

TEST_CASE(AsyncChunkLoader_ExhaustedMidReadFailuresRecoverWithoutNewRequest) {
    WorldResources resources;
    World world;
    world.initialize(resources);
    auto& registry = resources.registry();

    auto generator = makeGenerator(registry);
    world.setGenerator(generator);

    BlockID persisted = registerTestBlock(registry, "rigel:test_mid_read_retry");
    const ChunkCoord coord{0, 0, 0};
    ChunkData payload = buildPayload(
        coord, registry, {persisted}, false, std::nullopt, false);

    MemoryContext ctx;
    saveRegionForPayload(
        ctx.service, ctx.context, "rigel:default", coord, payload);
    auto failingStorage = std::make_shared<TransientMidReadFailureStorage>(
        ctx.context.storage,
        3);
    ctx.context.storage = failingStorage;

    AsyncChunkLoader loader(
        ctx.service,
        ctx.context,
        world,
        generator->config().world.version,
        0,
        0,
        1,
        generator);
    loader.setPrefetchRadius(0);
    loader.setLoadQueueLimit(1);
    auto retryNow = std::chrono::steady_clock::now();
    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::setRetryClock(
        loader,
        [&retryNow]() { return retryNow; });

    CHECK_EQ(loader.request(makeLoadRequest(coord)), ChunkLoadRequestResult::Queued);
    CHECK(loader.drainCompletions(8).empty());
    CHECK_EQ(failingStorage->readAttempts(), static_cast<size_t>(3));
    CHECK(loader.isPending(coord));
    CHECK_EQ(loader.workCount().pending, static_cast<size_t>(1));
    CHECK_EQ(loader.workCount().inFlight, static_cast<size_t>(0));
    CHECK_EQ(loader.workCount().terminalErrors, static_cast<size_t>(0));

    failingStorage->restore();
    retryNow += std::chrono::seconds(1);
    std::vector<ChunkLoadCompletion> resolved = loader.drainCompletions(8);

    CHECK_EQ(resolved.size(), static_cast<size_t>(1));
    CHECK_EQ(resolved.front().coord, coord);
    CHECK_EQ(resolved.front().outcome, ChunkLoadOutcome::Loaded);
    CHECK_EQ(failingStorage->readAttempts(), static_cast<size_t>(4));
    CHECK(!loader.isPending(coord));
    CHECK_EQ(loader.workCount().pending, static_cast<size_t>(0));
    CHECK_EQ(loader.workCount().inFlight, static_cast<size_t>(0));
    verifyPayloadMatches(*world.chunkManager().getChunk(coord), payload);
}

TEST_CASE(AsyncChunkLoader_CancelledRetryCannotAffectReplacementRequest) {
    WorldResources resources;
    World world;
    world.initialize(resources);
    auto& registry = resources.registry();

    auto generator = makeGenerator(registry);
    world.setGenerator(generator);

    BlockID persisted = registerTestBlock(registry, "rigel:test_retry_replacement");
    const ChunkCoord coord{0, 0, 0};
    ChunkData payload = buildPayload(
        coord, registry, {persisted}, false, std::nullopt, false);

    MemoryContext ctx;
    saveRegionForPayload(
        ctx.service, ctx.context, "rigel:default", coord, payload);
    auto failingStorage = std::make_shared<TransientReadFailureStorage>(
        ctx.context.storage,
        3);
    ctx.context.storage = failingStorage;

    AsyncChunkLoader loader(
        ctx.service,
        ctx.context,
        world,
        generator->config().world.version,
        0,
        0,
        1,
        generator);
    loader.setPrefetchRadius(0);
    loader.setLoadQueueLimit(1);
    auto retryNow = std::chrono::steady_clock::now();
    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::setRetryClock(
        loader,
        [&retryNow]() { return retryNow; });

    CHECK_EQ(loader.request(makeLoadRequest(coord)), ChunkLoadRequestResult::Queued);
    CHECK(loader.drainCompletions(8).empty());
    CHECK_EQ(failingStorage->readAttempts(), static_cast<size_t>(3));
    CHECK(loader.isPending(coord));

    loader.cancel(coord);
    CHECK(!loader.isPending(coord));
    CHECK_EQ(loader.workCount().pending, static_cast<size_t>(0));

    failingStorage->restore();
    CHECK_EQ(loader.request(makeLoadRequest(coord)), ChunkLoadRequestResult::Queued);
    CHECK(loader.isPending(coord));
    CHECK_EQ(loader.workCount().pending, static_cast<size_t>(1));
    CHECK_EQ(loader.workCount().inFlight, static_cast<size_t>(1));
    CHECK_EQ(loader.workCount().started, static_cast<uint64_t>(2));

    retryNow += std::chrono::seconds(1);
    std::vector<ChunkLoadCompletion> resolved = loader.drainCompletions(8);

    CHECK_EQ(resolved.size(), static_cast<size_t>(1));
    CHECK_EQ(resolved.front().coord, coord);
    CHECK_EQ(resolved.front().outcome, ChunkLoadOutcome::Loaded);
    CHECK_EQ(failingStorage->readAttempts(), static_cast<size_t>(4));
    CHECK(loader.drainCompletions(8).empty());
    CHECK(!loader.isPending(coord));
    CHECK_EQ(loader.workCount().pending, static_cast<size_t>(0));
    CHECK_EQ(loader.workCount().inFlight, static_cast<size_t>(0));
    verifyPayloadMatches(*world.chunkManager().getChunk(coord), payload);
}

TEST_CASE(AsyncChunkLoader_RegionRetryWaitUsesLoadCapacity) {
    WorldResources resources;
    World world;
    world.initialize(resources);
    auto& registry = resources.registry();

    auto generator = makeGenerator(registry);
    world.setGenerator(generator);

    BlockID persisted = registerTestBlock(registry, "rigel:test_retry_capacity");
    const ChunkCoord retryCoord{0, 0, 0};
    const ChunkCoord activeCoord{64, 0, 0};
    ChunkData retryPayload = buildPayload(
        retryCoord, registry, {persisted}, false, std::nullopt, false);
    ChunkData activePayload = buildPayload(
        activeCoord, registry, {persisted}, false, std::nullopt, false);

    MemoryContext ctx;
    saveRegionForPayload(ctx.service,
                         ctx.context,
                         "rigel:default",
                         retryCoord,
                         retryPayload);
    saveRegionForPayload(ctx.service,
                         ctx.context,
                         "rigel:default",
                         activeCoord,
                         activePayload);
    auto failingStorage = std::make_shared<TransientReadFailureStorage>(
        ctx.context.storage,
        3);
    ctx.context.storage = failingStorage;

    AsyncChunkLoader loader(
        ctx.service,
        ctx.context,
        world,
        generator->config().world.version,
        0,
        0,
        1,
        generator);
    loader.setPrefetchRadius(0);
    loader.setLoadQueueLimit(1);
    loader.setRegionDrainBudget(3);
    auto retryNow = std::chrono::steady_clock::now();
    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::setRetryClock(
        loader,
        [&retryNow]() { return retryNow; });

    CHECK_EQ(loader.request(makeLoadRequest(retryCoord)), ChunkLoadRequestResult::Queued);
    CHECK_EQ(loader.request(makeLoadRequest(activeCoord)), ChunkLoadRequestResult::Deferred);
    auto resolved = loader.drainCompletions(8);

    CHECK(resolved.empty());
    CHECK_EQ(failingStorage->readAttempts(), static_cast<size_t>(4));
    CHECK(loader.isPending(retryCoord));
    CHECK(loader.isPending(activeCoord));
    CHECK_EQ(loader.workCount().pending, static_cast<size_t>(2));
    CHECK_EQ(loader.workCount().inFlight, static_cast<size_t>(1));

    failingStorage->restore();
    retryNow += std::chrono::seconds(1);
    resolved = loader.drainCompletions(0);
    CHECK(resolved.empty());
    CHECK_EQ(failingStorage->readAttempts(), static_cast<size_t>(4));
    CHECK_EQ(loader.workCount().pending, static_cast<size_t>(2));
    CHECK_EQ(loader.workCount().inFlight, static_cast<size_t>(1));

    resolved = loader.drainCompletions(8);
    CHECK_EQ(resolved.size(), static_cast<size_t>(1));
    CHECK_EQ(resolved.front().coord, activeCoord);
    CHECK_EQ(resolved.front().outcome, ChunkLoadOutcome::Loaded);
    CHECK(loader.isPending(retryCoord));
    CHECK_EQ(loader.workCount().pending, static_cast<size_t>(1));
    CHECK_EQ(loader.workCount().inFlight, static_cast<size_t>(1));

    resolved = loader.drainCompletions(8);
    CHECK_EQ(resolved.size(), static_cast<size_t>(1));
    CHECK_EQ(resolved.front().coord, retryCoord);
    CHECK_EQ(resolved.front().outcome, ChunkLoadOutcome::Loaded);
    CHECK(!loader.isPending(retryCoord));
    CHECK_EQ(loader.workCount().pending, static_cast<size_t>(0));
    CHECK_EQ(loader.workCount().inFlight, static_cast<size_t>(0));
    verifyPayloadMatches(*world.chunkManager().getChunk(retryCoord), retryPayload);
}

TEST_CASE(AsyncChunkLoader_MalformedPayloadFailsOnBackgroundWorker) {
    WorldResources resources;
    World world;
    world.initialize(resources);
    auto& registry = resources.registry();

    auto generator = makeGenerator(registry);
    world.setGenerator(generator);

    BlockID persisted = registerTestBlock(registry, "rigel:test_malformed_payload");
    const ChunkCoord coord{0, 0, 0};
    const ChunkCoord deferredCoord{16, 0, 0};
    ChunkData payload = buildPayload(
        coord,
        registry,
        {persisted},
        false,
        std::nullopt,
        false);
    payload.blocks.pop_back();
    ChunkData deferredPayload = buildPayload(
        deferredCoord,
        registry,
        {persisted},
        false,
        std::nullopt,
        false);

    MemoryContext ctx;
    writeRawMemoryRegion(
        ctx.service,
        ctx.context,
        "rigel:default",
        {{coord, payload}});
    saveRegionForPayload(
        ctx.service, ctx.context, "rigel:default", deferredCoord, deferredPayload);

    AsyncChunkLoader loader(
        ctx.service,
        ctx.context,
        world,
        generator->config().world.version,
        1,
        1,
        1,
        generator);
    loader.setPrefetchRadius(0);
    loader.setLoadQueueLimit(1);

    CHECK_EQ(loader.request(makeLoadRequest(coord)), ChunkLoadRequestResult::Queued);
    CHECK_EQ(loader.request(makeLoadRequest(deferredCoord)), ChunkLoadRequestResult::Deferred);
    CHECK(waitForRegionCompletion(loader));
    std::vector<ChunkLoadCompletion> resolved = loader.drainCompletions(1);
    if (loader.workCount().terminalErrors == 0) {
        CHECK(waitForPayloadCompletions(loader, 1));
        resolved = loader.drainCompletions(1);
    }

    CHECK(resolved.empty());
    CHECK(loader.isPending(coord));
    CHECK_EQ(loader.workCount().terminalErrors, static_cast<size_t>(1));
    CHECK(loader.workCount().lastError.find("(0, 0, 0)") != std::string::npos);
    CHECK(loader.workCount().lastError.find("repair") != std::string::npos);

    CHECK(waitForRegionCompletion(loader));
    CHECK(loader.drainCompletions(1).empty());
    CHECK(waitForPayloadCompletions(loader, 1));
    resolved = loader.drainCompletions(8);
    CHECK_EQ(resolved.size(), static_cast<size_t>(1));
    CHECK_EQ(resolved.front().coord, deferredCoord);
    CHECK_EQ(resolved.front().outcome, ChunkLoadOutcome::Loaded);
    CHECK(!loader.isPending(deferredCoord));
    CHECK_EQ(loader.workCount().pending, static_cast<size_t>(1));
    CHECK_EQ(loader.workCount().inFlight, static_cast<size_t>(0));
    CHECK_EQ(loader.workCount().terminalErrors, static_cast<size_t>(1));
    CHECK(!loader.workCount().empty());
    CHECK(world.chunkManager().getChunk(coord) == nullptr);
    CHECK(world.chunkManager().getChunk(deferredCoord) != nullptr);

    WorldMeshStore meshStore;
    ChunkStreamer streamer(
        world.chunkManager(), meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 0;
    stream.genQueueLimit = 0;
    stream.meshQueueLimit = 0;
    stream.workerThreads = 0;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    auto sharedLoader = std::shared_ptr<AsyncChunkLoader>(&loader, [](AsyncChunkLoader*) {});
    configureStreamerLoader(streamer, sharedLoader);
    streamer.markSpawnDiscoveryComplete();
    for (int update = 0; update < 4; ++update) {
        streamer.update(coord.toWorldCenter());
        streamer.processCompletions();
        CHECK_EQ(streamer.diagnostics().state,
                 StreamingLifecycleState::Streaming);
        CHECK_EQ(streamer.diagnostics().chunkLoad.terminalErrors,
                 static_cast<size_t>(1));
        CHECK_EQ(streamer.workMetrics().lastUpdateDesiredBuildCoordinatesInspected,
                 static_cast<uint64_t>(update == 0 ? 1 : 0));
        if (update > 0) {
            CHECK_EQ(
                streamer.workMetrics().lastUpdateSchedulerCoordinatesInspected,
                static_cast<uint64_t>(0));
        }
    }
    CHECK_EQ(streamer.diagnostics().state, StreamingLifecycleState::Streaming);
    CHECK_EQ(streamer.diagnostics().chunkLoad.terminalErrors,
             static_cast<size_t>(1));
    CHECK(!streamer.diagnostics().workEmpty());
    CHECK_EQ(streamer.workMetrics().generationJobsStarted,
             static_cast<uint64_t>(0));

    streamer.update(deferredCoord.toWorldCenter());
    CHECK(!loader.isPending(coord));
    CHECK_EQ(loader.workCount().terminalErrors, static_cast<size_t>(0));
    CHECK(loader.workCount().lastError.empty());
}

TEST_CASE(AsyncChunkLoader_FailureSignatureTracksTerminalSetMutation) {
    WorldResources resources;
    World world;
    world.initialize(resources);
    auto& registry = resources.registry();
    auto generator = makeGenerator(registry);
    world.setGenerator(generator);

    BlockID persisted =
        registerTestBlock(registry, "rigel:terminal_signature_payload");
    const ChunkCoord representative{0, 0, 0};
    const ChunkCoord removed{1, 0, 0};
    const ChunkCoord replacement{2, 0, 0};
    ChunkData representativePayload = buildPayload(
        representative, registry, {persisted}, false, std::nullopt, false);
    ChunkData removedPayload = buildPayload(
        removed, registry, {persisted}, false, std::nullopt, false);
    ChunkData replacementPayload = buildPayload(
        replacement, registry, {persisted}, false, std::nullopt, false);
    representativePayload.blocks.pop_back();
    removedPayload.blocks.pop_back();
    replacementPayload.blocks.pop_back();

    MemoryContext ctx;
    writeRawMemoryRegion(
        ctx.service,
        ctx.context,
        "rigel:default",
        {{representative, representativePayload},
         {removed, removedPayload},
         {replacement, replacementPayload}});

    AsyncChunkLoader loader(
        ctx.service,
        ctx.context,
        world,
        generator->config().world.version,
        0,
        0,
        1,
        generator);
    loader.setPrefetchRadius(0);

    CHECK_EQ(loader.request(makeLoadRequest(representative)),
             ChunkLoadRequestResult::Queued);
    CHECK_EQ(loader.request(makeLoadRequest(removed)),
             ChunkLoadRequestResult::Queued);
    CHECK(loader.drainCompletions(8).empty());

    StreamingDiagnosticSnapshot previous;
    previous.chunkLoad = loader.workCount();
    CHECK_EQ(previous.chunkLoad.terminalErrors, static_cast<size_t>(2));
    CHECK(previous.chunkLoad.lastError.find("(0, 0, 0)") !=
          std::string::npos);

    loader.cancel(removed);
    CHECK_EQ(loader.request(makeLoadRequest(replacement)),
             ChunkLoadRequestResult::Queued);
    CHECK(loader.drainCompletions(8).empty());

    StreamingDiagnosticSnapshot current;
    current.chunkLoad = loader.workCount();
    CHECK_EQ(current.chunkLoad.terminalErrors,
             previous.chunkLoad.terminalErrors);
    CHECK_EQ(current.chunkLoad.lastError, previous.chunkLoad.lastError);
    CHECK(streamingFailureSignatureChanged(previous, current));
    CHECK_EQ(loader.workCount().failureVersion,
             current.chunkLoad.failureVersion);
    CHECK(!streamingFailureSignatureChanged(current, current));
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

    CHECK_EQ(loader.request(makeLoadRequest(coordA)), ChunkLoadRequestResult::Queued);
    CHECK_EQ(loader.request(makeLoadRequest(coordB)), ChunkLoadRequestResult::Queued);
    CHECK(loader.isPending(coordA));
    CHECK(loader.isPending(coordB));
    auto deferred = loader.workCount();
    CHECK_EQ(deferred.pending, static_cast<size_t>(2));
    CHECK_EQ(deferred.inFlight, static_cast<size_t>(1));
    CHECK_EQ(deferred.started, static_cast<uint64_t>(2));

    auto firstResolved = loader.drainCompletions(1);
    CHECK_EQ(firstResolved.size(), static_cast<size_t>(1));
    CHECK_EQ(firstResolved.front().coord, coordA);
    CHECK_EQ(firstResolved.front().outcome, ChunkLoadOutcome::Loaded);
    CHECK(!loader.isPending(coordA));
    CHECK(loader.isPending(coordB));
    auto secondActive = loader.workCount();
    CHECK_EQ(secondActive.pending, static_cast<size_t>(1));
    CHECK_EQ(secondActive.inFlight, static_cast<size_t>(1));
    CHECK_EQ(secondActive.started, static_cast<uint64_t>(2));

    auto secondResolved = loader.drainCompletions(1);
    CHECK_EQ(secondResolved.size(), static_cast<size_t>(1));
    CHECK_EQ(secondResolved.front().coord, coordB);
    CHECK_EQ(secondResolved.front().outcome, ChunkLoadOutcome::Loaded);
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

    CHECK_EQ(loader.request(makeLoadRequest(coord)), ChunkLoadRequestResult::Queued);
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

    CHECK_EQ(loader.request(makeLoadRequest(active)), ChunkLoadRequestResult::Queued);
    CHECK_EQ(loader.request(makeLoadRequest(deferred)), ChunkLoadRequestResult::Deferred);
    CHECK(loader.isPending(deferred));
    CHECK_EQ(loader.workCount().pending, static_cast<size_t>(2));

    loader.cancel(deferred);
    CHECK(!loader.isPending(deferred));
    CHECK_EQ(loader.workCount().pending, static_cast<size_t>(1));

    auto resolved = loader.drainCompletions(2);
    CHECK_EQ(resolved.size(), static_cast<size_t>(1));
    CHECK_EQ(resolved.front().coord, active);
    CHECK_EQ(resolved.front().outcome, ChunkLoadOutcome::Loaded);
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

    CHECK_EQ(loader.request(makeLoadRequest(coord)), ChunkLoadRequestResult::Queued);
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
    CHECK_EQ(loader.request(makeLoadRequest(missing)), ChunkLoadRequestResult::Queued);
    CHECK(loader.isPending(missing));

    auto resolved = loader.drainCompletions(8);

    CHECK(!loader.isPending(missing));
    CHECK(world.chunkManager().getChunk(missing) == nullptr);
    CHECK_EQ(resolved.size(), static_cast<size_t>(1));
    CHECK_EQ(resolved.front().coord, missing);
    CHECK_EQ(resolved.front().outcome, ChunkLoadOutcome::Missing);

    CHECK_EQ(loader.request(makeLoadRequest(missing)), ChunkLoadRequestResult::Missing);
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

    CHECK_EQ(loader->request(makeLoadRequest(payloadCoord)), ChunkLoadRequestResult::Queued);
    CHECK(waitForRegionCompletion(*loader));
    loader->drainCompletions(1);
    CHECK(payloadGate->waitUntilEntered());

    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::setRegionLoadStartCallback(
        *loader,
        [regionGate]() { regionGate->enterAndWait(); });
    CHECK_EQ(loader->request(makeLoadRequest(regionCoord)), ChunkLoadRequestResult::Queued);
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
