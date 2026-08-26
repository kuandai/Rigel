#include "TestFramework.h"
#include "ThreadPoolTestAccess.h"

#include "Rigel/Persistence/AsyncChunkLoader.h"
#include "Rigel/Persistence/Backends/Memory/MemoryFormat.h"
#include "Rigel/Persistence/ChunkSerializer.h"
#include "Rigel/Persistence/PersistenceService.h"
#include "Rigel/Persistence/Storage.h"
#include "Rigel/Voxel/WorldResources.h"
#include "Rigel/Voxel/World.h"
#include "Rigel/Voxel/ChunkStreamer.h"
#include "WorldGenerationTestFixture.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <random>
#include <stdexcept>
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

    static void setRegionLoadStartObserver(
        AsyncChunkLoader& loader,
        std::function<void(const RegionKey&, bool)> observer) {
        loader.m_regionLoadStartObserver =
            [observer = std::move(observer)](
                const RegionKey& key,
                AsyncChunkLoader::RegionJobOrigin origin) {
                observer(
                    key,
                    origin == AsyncChunkLoader::RegionJobOrigin::Direct);
            };
    }

    static void setRegionResultReadyToPublishCallback(
        AsyncChunkLoader& loader,
        std::function<void()> callback) {
        loader.m_regionResultReadyToPublishCallback = std::move(callback);
    }

    static void setPayloadBuildStartCallback(AsyncChunkLoader& loader,
                                             std::function<void()> callback) {
        loader.m_payloadBuildStartCallback = std::move(callback);
    }

    static void setPayloadResultPublishedObserver(
        AsyncChunkLoader& loader,
        std::function<void(Voxel::ChunkCoord)> observer) {
        loader.m_payloadResultPublishedObserver = std::move(observer);
    }

    static void setIoPoolStopStartCallback(AsyncChunkLoader& loader,
                                           std::function<void()> callback) {
        loader.m_ioPoolStopStartCallback = std::move(callback);
    }

    static void setWorkerPoolStopStartCallback(AsyncChunkLoader& loader,
                                               std::function<void()> callback) {
        loader.m_workerPoolStopStartCallback = std::move(callback);
    }

    static Voxel::detail::ThreadPool::JobId enqueueIoPoolJob(
        AsyncChunkLoader& loader,
        std::function<void()> job) {
        return loader.m_ioPool.enqueue(std::move(job));
    }

    static Voxel::detail::ThreadPool::JobId enqueueWorkerPoolJob(
        AsyncChunkLoader& loader,
        std::function<void()> job) {
        return loader.m_workerPool.enqueue(std::move(job));
    }

    static void gateNextIoPoolEnqueueReturn(
        AsyncChunkLoader& loader,
        std::atomic<bool>& entered,
        std::atomic<bool>& released) {
        Voxel::detail::ThreadPoolTestAccess::gateNextEnqueueReturn(
            loader.m_ioPool, entered, released);
    }

    static void gateNextIoPoolSubmissionCommit(
        AsyncChunkLoader& loader,
        std::atomic<bool>& entered,
        std::atomic<bool>& released) {
        Voxel::detail::ThreadPoolTestAccess::gateNextSubmissionCommit(
            loader.m_ioPool, entered, released);
    }

    static void stopIoPool(AsyncChunkLoader& loader) {
        loader.m_ioPool.stop();
    }

    static Voxel::RegionSchedulerOriginDiagnostics directRegionBoundaryMetrics(
        const AsyncChunkLoader& loader) {
        return loader.regionJobMetrics(loader.m_directRegionMetrics);
    }

    static Voxel::RegionSchedulerOriginDiagnostics
    speculativeRegionBoundaryMetrics(
        const AsyncChunkLoader& loader,
        std::atomic<bool>* subsetReadEntered = nullptr,
        std::atomic<bool>* subsetReadReleased = nullptr) {
        return loader.regionJobMetrics(
            loader.m_speculativeRegionMetrics,
            subsetReadEntered,
            subsetReadReleased);
    }

    static std::shared_ptr<const void> regionJobIdentity(
        const AsyncChunkLoader& loader,
        const RegionKey& key) {
        auto it = loader.m_regionJobs.find(key);
        return it == loader.m_regionJobs.end()
            ? std::shared_ptr<const void>{}
            : it->second;
    }

    static Voxel::detail::ThreadPool::JobId regionPoolJobId(
        const AsyncChunkLoader& loader,
        const RegionKey& key) {
        auto it = loader.m_regionJobs.find(key);
        return it == loader.m_regionJobs.end() ? 0 : it->second->poolJobId;
    }

    static std::optional<Voxel::ChunkLoadExecutionPhase> regionJobPhase(
        const AsyncChunkLoader& loader,
        const RegionKey& key) {
        auto it = loader.m_regionJobs.find(key);
        return it == loader.m_regionJobs.end()
            ? std::nullopt
            : std::optional<Voxel::ChunkLoadExecutionPhase>{
                  it->second->phase.load(std::memory_order_acquire)};
    }

    static bool regionJobHasDirectDemand(const AsyncChunkLoader& loader,
                                         const RegionKey& key) {
        auto it = loader.m_regionJobs.find(key);
        return it != loader.m_regionJobs.end() && it->second->demanded;
    }

    static bool regionJobHasDirectOrigin(const AsyncChunkLoader& loader,
                                         const RegionKey& key) {
        auto it = loader.m_regionJobs.find(key);
        return it != loader.m_regionJobs.end() &&
            it->second->origin == AsyncChunkLoader::RegionJobOrigin::Direct;
    }

    static size_t regionLoadAttemptCount(const AsyncChunkLoader& loader,
                                         const RegionKey& key) {
        auto it = loader.m_regionLoadAttempts.find(key);
        return it == loader.m_regionLoadAttempts.end() ? 0 : it->second;
    }

    static size_t regionOwnerCount(const AsyncChunkLoader& loader) {
        return loader.m_regionJobs.size();
    }

    static size_t regionAttemptOwnerCount(const AsyncChunkLoader& loader) {
        return loader.m_regionLoadAttempts.size();
    }

    static bool regionPhysicallyInFlight(const AsyncChunkLoader& loader,
                                         const RegionKey& key) {
        return loader.m_inFlight.find(key) != loader.m_inFlight.end();
    }

    static void setMetricClock(
        AsyncChunkLoader& loader,
        std::function<AsyncChunkLoader::RetryClock::time_point()> clock) {
        loader.m_metricClock = std::move(clock);
    }

    static bool queueSpeculativeRegionLoad(AsyncChunkLoader& loader,
                                           const RegionKey& key) {
        return loader.queueRegionLoad(
            key, AsyncChunkLoader::RegionJobOrigin::Speculative);
    }

    static size_t regionRetryOwnerCount(const AsyncChunkLoader& loader) {
        return loader.m_retryChunks.size();
    }

    static size_t regionCacheCount(const AsyncChunkLoader& loader) {
        return loader.m_cache.size();
    }

    static std::vector<RegionKey> submittedSpeculativeRegionKeys(
        const AsyncChunkLoader& loader) {
        std::vector<RegionKey> keys;
        keys.reserve(loader.m_submittedSpeculativeRegionJobs.size());
        for (const auto& job : loader.m_submittedSpeculativeRegionJobs) {
            keys.push_back(job->key);
        }
        return keys;
    }

    static std::vector<RegionKey> queuedSpeculativeRegionKeys(
        const AsyncChunkLoader& loader) {
        std::vector<RegionKey> keys;
        for (const RegionKey& key : loader.m_speculativeRegionLoads) {
            auto it = loader.m_regionJobs.find(key);
            if (it != loader.m_regionJobs.end() && !it->second->started &&
                !it->second->demanded) {
                keys.push_back(key);
            }
        }
        return keys;
    }

    static void markRegionKnownMissing(AsyncChunkLoader& loader,
                                       const RegionKey& key) {
        AsyncChunkLoader::RegionPresence& presence = loader.m_regionPresence[key];
        presence.exists = false;
        presence.nextCheck =
            std::chrono::steady_clock::now() + std::chrono::hours(1);
        loader.m_cache.erase(key);
    }

    static size_t regionCompletionCount(const AsyncChunkLoader& loader) {
        return loader.m_regionComplete.size();
    }

    static size_t payloadCompletionCount(const AsyncChunkLoader& loader) {
        return loader.m_chunkComplete.size();
    }

    static bool payloadRequestInFlight(const AsyncChunkLoader& loader,
                                       Voxel::ChunkCoord coord,
                                       Voxel::ChunkLoadRequestId requestId) {
        auto it = loader.m_payloadInFlight.find(coord);
        return it != loader.m_payloadInFlight.end() &&
            it->second->request.requestId == requestId;
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

std::string exceptionMessage(const std::function<void()>& operation) {
    try {
        operation();
    } catch (const std::exception& error) {
        return error.what();
    }
    throw Rigel::Test::TestFailure("Expected operation to fail");
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

class AtomicFlagRelease {
public:
    explicit AtomicFlagRelease(std::atomic<bool>& released)
        : m_released(released) {}

    ~AtomicFlagRelease() {
        release();
    }

    void release() {
        m_released.store(true, std::memory_order_release);
        m_released.notify_all();
    }

private:
    std::atomic<bool>& m_released;
};

bool waitUntilTrue(std::atomic<bool>& value) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!value.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    return value.load(std::memory_order_acquire);
}

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

class ExpectedLoaderFixtureError : public std::runtime_error {
public:
    ExpectedLoaderFixtureError()
        : std::runtime_error("expected loader fixture failure") {}
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

bool waitForPublishedRegionJobs(const AsyncChunkLoader& loader, size_t count) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        const size_t completionCount = Rigel::Persistence::detail::
            AsyncChunkLoaderTestAccess::regionCompletionCount(loader);
        if (completionCount >= count) {
            return true;
        }
        std::this_thread::yield();
    }
    const size_t completionCount = Rigel::Persistence::detail::
        AsyncChunkLoaderTestAccess::regionCompletionCount(loader);
    return completionCount >= count;
}

bool drainRegionJobsUntilSettled(
    AsyncChunkLoader& loader,
    std::vector<ChunkLoadCompletion>& resolved) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        auto completions = loader.drainCompletions(64);
        resolved.insert(
            resolved.end(), completions.begin(), completions.end());
        const auto metrics = loader.metrics();
        auto originSettled = [](const auto& origin) {
            return origin.logicalAdmissions ==
                    origin.resultsPublished +
                        origin.logicalPreStartCancellations &&
                origin.poolSubmissions ==
                    origin.poolWorkerStarts + origin.successfulPoolYields +
                        origin.terminalPoolCancellations &&
                origin.resultsPublished == origin.resultsDrained;
        };
        if (originSettled(metrics.directOrigin) &&
            originSettled(metrics.speculativeOrigin) &&
            metrics.demandOwnedQueued == 0 &&
            metrics.speculativeOwnedQueued == 0 &&
            metrics.demandOwnedDispatchedUndrained == 0 &&
            metrics.speculativeOwnedDispatchedUndrained == 0 &&
            metrics.speculativePoolJobsPending == 0) {
            return true;
        }
        std::this_thread::yield();
    }
    return false;
}

struct RegionLifecycleSnapshot {
    uint64_t directAdmissions = 0;
    uint64_t directPoolSubmissions = 0;
    uint64_t directPublications = 0;
    uint64_t directDrains = 0;
    uint64_t speculativeAdmissions = 0;
    uint64_t speculativePoolSubmissions = 0;
    uint64_t speculativePublications = 0;
    uint64_t speculativeDrains = 0;

    bool operator==(const RegionLifecycleSnapshot&) const = default;
};

RegionLifecycleSnapshot regionLifecycleSnapshot(
    const AsyncChunkLoader::Metrics& metrics) {
    return RegionLifecycleSnapshot{
        .directAdmissions = metrics.directOrigin.logicalAdmissions,
        .directPoolSubmissions = metrics.directOrigin.poolSubmissions,
        .directPublications = metrics.directOrigin.resultsPublished,
        .directDrains = metrics.directOrigin.resultsDrained,
        .speculativeAdmissions = metrics.speculativeOrigin.logicalAdmissions,
        .speculativePoolSubmissions =
            metrics.speculativeOrigin.poolSubmissions,
        .speculativePublications =
            metrics.speculativeOrigin.resultsPublished,
        .speculativeDrains = metrics.speculativeOrigin.resultsDrained
    };
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

WorldGenConfig loaderGeneratorDefinition() {
    WorldGenConfig config;
    config.seed = 1;
    config.solidBlock = "rigel:test_solid";
    config.surfaceBlock = "rigel:test_surface";
    config.waterBlock = config.solidBlock;
    config.shoreBlock = config.surfaceBlock;
    config.terrain.baseHeight = 64.0f;
    config.terrain.heightVariation = 0.0f;
    config.terrain.surfaceDepth = 1;
    config.biomes.entries.clear();

    WorldGenConfig::DensityNodeConfig density;
    density.id = "flat_height";
    density.type = "y";
    density.scale = -1.0f;
    density.offset = 64.0f;
    config.densityGraph.nodes.push_back(std::move(density));
    config.densityGraph.outputs["base_density"] = "flat_height";
    config.stageEnabled["caves"] = false;
    config.stageEnabled["structures"] = false;
    return config;
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

    return std::make_shared<WorldGenerator>(
        registry, loaderGeneratorDefinition());
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
        auto settings = Rigel::Test::savedWorldSettingsFixture(
            "Async Chunk Loader Test World");
        settings.seed = loaderGeneratorDefinition().seed;
        Rigel::Test::installSavedWorldGenerationFixture(
            service,
            context,
            settings,
            loaderGeneratorDefinition());
    }
};

class CapabilityOverrideFormat final : public PersistenceFormat {
public:
    CapabilityOverrideFormat(
        FormatDescriptor descriptor,
        std::unique_ptr<PersistenceFormat> delegate)
        : m_descriptor(std::move(descriptor))
        , m_delegate(std::move(delegate)) {}

    const FormatDescriptor& descriptor() const override {
        return m_descriptor;
    }

    WorldMetadataCodec& worldMetadataCodec() override {
        return m_delegate->worldMetadataCodec();
    }

    ZoneMetadataCodec& zoneMetadataCodec() override {
        return m_delegate->zoneMetadataCodec();
    }

    ChunkContainer& chunkContainer() override {
        return m_delegate->chunkContainer();
    }

    EntityContainer& entityContainer() override {
        return m_delegate->entityContainer();
    }

    RegionLayout& regionLayout() override {
        return m_delegate->regionLayout();
    }

private:
    FormatDescriptor m_descriptor;
    std::unique_ptr<PersistenceFormat> m_delegate;
};

bool isChunkRegionStoragePath(const std::string& path) {
    return path.find("/regions/region_") != std::string::npos;
}

class TransientReadFailureStorage final : public StorageBackend {
public:
    TransientReadFailureStorage(std::shared_ptr<StorageBackend> delegate,
                                size_t failures)
        : m_delegate(std::move(delegate)),
          m_failuresRemaining(failures) {}

    std::unique_ptr<ByteReader> openRead(const std::string& path) override {
        if (!isChunkRegionStoragePath(path)) {
            return m_delegate->openRead(path);
        }
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
        const std::string& path) override {
        return m_delegate->openWrite(path);
    }

    bool exists(const std::string& path) override {
        return m_delegate->exists(path);
    }

    StorageEntryKind entryKind(const std::string& path) override {
        return m_delegate->entryKind(path);
    }

    void forEachEntry(
        const std::string& path,
        const StorageEntryVisitor& visitor) override {
        m_delegate->forEachEntry(path, visitor);
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
        if (!isChunkRegionStoragePath(path)) {
            return m_delegate->openRead(path);
        }
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
        const std::string& path) override {
        return m_delegate->openWrite(path);
    }

    bool exists(const std::string& path) override {
        return m_delegate->exists(path);
    }

    StorageEntryKind entryKind(const std::string& path) override {
        return m_delegate->entryKind(path);
    }

    void forEachEntry(
        const std::string& path,
        const StorageEntryVisitor& visitor) override {
        m_delegate->forEachEntry(path, visitor);
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
        auto session = m_delegate->openWrite(m_path);
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
        if (path.ends_with("/world-settings.yaml") ||
            path.ends_with("/generator-definition.yaml") ||
            path.ends_with("/world.meta")) {
            ++m_identityReadAttempts;
        }
        return m_delegate->openRead(path);
    }

    std::unique_ptr<AtomicWriteSession> openWrite(
        const std::string& path) override {
        ++m_writeAttempts;
        size_t remaining = m_failuresRemaining.load();
        while (remaining > 0) {
            if (m_failuresRemaining.compare_exchange_weak(
                    remaining,
                    remaining - 1)) {
                throw std::runtime_error("injected transient write failure");
            }
        }
        return m_delegate->openWrite(path);
    }

    bool exists(const std::string& path) override {
        return m_delegate->exists(path);
    }

    StorageEntryKind entryKind(const std::string& path) override {
        return m_delegate->entryKind(path);
    }

    void forEachEntry(
        const std::string& path,
        const StorageEntryVisitor& visitor) override {
        m_delegate->forEachEntry(path, visitor);
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

    size_t identityReadAttempts() const {
        return m_identityReadAttempts.load();
    }

private:
    std::shared_ptr<StorageBackend> m_delegate;
    std::atomic<size_t> m_failuresRemaining;
    std::atomic<size_t> m_writeAttempts = 0;
    std::atomic<size_t> m_identityReadAttempts = 0;
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
    auto session = context.storage->openWrite(path);
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
    streamer.setChunkLoadDiagnosticsCallback([loader]() {
        return loader->diagnostics();
    });
    streamer.setChunkLoadExecutionStateCallback([loader](ChunkCoord coord) {
        return loader->executionState(coord);
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

    std::optional<ChunkCoord> publishedPayloadCoord;
    std::optional<ChunkLoadExecutionState> publishedPayloadState;
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
    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
        setPayloadResultPublishedObserver(
            loader,
            [&](ChunkCoord publishedCoord) {
                publishedPayloadCoord = publishedCoord;
                publishedPayloadState = loader.executionState(coord);
            });

    const ChunkLoadRequest request = makeLoadRequest(coord);
    CHECK(!loader.executionState(coord).has_value());
    CHECK_EQ(loader.request(request), ChunkLoadRequestResult::Queued);
    CHECK(loader.isPending(coord));
    CHECK_EQ(
        loader.executionState(coord),
        ChunkLoadExecutionState({
            ChunkLoadExecutionOwner::Region,
            ChunkLoadExecutionPhase::ResultPublished}));

    auto resolved = loader.drainCompletions(1);

    const auto inlinePayloadPublished = ChunkLoadExecutionState{
        ChunkLoadExecutionOwner::Payload,
        ChunkLoadExecutionPhase::ResultPublished};
    CHECK_EQ(publishedPayloadCoord, coord);
    CHECK_EQ(publishedPayloadState, inlinePayloadPublished);

    Chunk* loaded = world.chunkManager().getChunk(coord);
    CHECK(loaded != nullptr);
    if (loaded) {
        verifyPayloadMatches(*loaded, payload);
    }
    CHECK(!loader.isPending(coord));
    CHECK(!loader.executionState(coord).has_value());
    CHECK_EQ(resolved.size(), static_cast<size_t>(1));
    CHECK_EQ(resolved.front().coord, coord);
    CHECK_EQ(resolved.front().requestId, request.requestId);
    CHECK_EQ(resolved.front().outcome, ChunkLoadOutcome::Loaded);
}

TEST_CASE(AsyncChunkLoader_rejects_runtime_generator_outside_saved_snapshot) {
    WorldResources resources;
    World world;
    world.initialize(resources);
    auto authoritative = makeGenerator(resources.registry());
    world.setGenerator(authoritative);
    MemoryContext context;

    auto divergentDefinition = loaderGeneratorDefinition();
    divergentDefinition.densityGraph.nodes.front().offset = 32.0f;
    auto divergent = std::make_shared<WorldGenerator>(
        resources.registry(), std::move(divergentDefinition));

    CHECK_EQ(
        exceptionMessage([&] {
            AsyncChunkLoader loader(
                context.service,
                context.context,
                world,
                authoritative->config().world.version,
                0,
                0,
                1,
                divergent);
        }),
        "Runtime generator does not match authoritative "
        "generator-definition.yaml for world save '" +
            context.context.rootPath + "'");
    CHECK_EQ(world.chunkManager().loadedChunkCount(), static_cast<size_t>(0));

    world.setGenerator(divergent);
    CHECK_EQ(
        exceptionMessage([&] {
            AsyncChunkLoader loader(
                context.service,
                context.context,
                world,
                authoritative->config().world.version,
                0,
                0,
                1,
                authoritative);
        }),
        "World generator does not match authoritative "
        "generator-definition.yaml for world save '" +
            context.context.rootPath + "'");
    CHECK_EQ(world.chunkManager().loadedChunkCount(), static_cast<size_t>(0));
    world.setGenerator(authoritative);

    auto wrongSeedDefinition = loaderGeneratorDefinition();
    wrongSeedDefinition.seed += 1;
    auto wrongSeed = std::make_shared<WorldGenerator>(
        resources.registry(), std::move(wrongSeedDefinition));
    CHECK_EQ(
        exceptionMessage([&] {
            AsyncChunkLoader loader(
                context.service,
                context.context,
                world,
                authoritative->config().world.version,
                0,
                0,
                1,
                wrongSeed);
        }),
        "Runtime generator does not match authoritative "
        "generator-definition.yaml for world save '" +
            context.context.rootPath + "'");
    CHECK_EQ(world.chunkManager().loadedChunkCount(), static_cast<size_t>(0));

    CHECK_EQ(
        exceptionMessage([&] {
            AsyncChunkLoader loader(
                context.service,
                context.context,
                world,
                authoritative->config().world.version + 1,
                0,
                0,
                1,
                authoritative);
        }),
        "Runtime generation semantics version does not match authoritative "
        "generator-definition.yaml for world save '" +
            context.context.rootPath + "'");
    CHECK_EQ(world.chunkManager().loadedChunkCount(), static_cast<size_t>(0));
}

TEST_CASE(AsyncChunkLoader_ExecutionStateTracksPhysicalRegionAndPayloadOwners) {
    WorldResources resources;
    World world;
    world.initialize(resources);
    auto& registry = resources.registry();
    auto generator = makeGenerator(registry);
    world.setGenerator(generator);

    const BlockID persisted =
        registerTestBlock(registry, "rigel:execution_state_payload");
    const ChunkCoord active{0, 0, 0};
    const ChunkCoord deferred{64, 0, 0};
    MemoryContext ctx;
    saveRegionForPayload(
        ctx.service,
        ctx.context,
        "rigel:default",
        active,
        buildPayload(
            active, registry, {persisted}, false, std::nullopt, false));
    saveRegionForPayload(
        ctx.service,
        ctx.context,
        "rigel:default",
        deferred,
        buildPayload(
            deferred, registry, {persisted}, false, std::nullopt, false));

    auto ioGate = std::make_shared<LoaderWorkGate>();
    auto workerGate = std::make_shared<LoaderWorkGate>();
    auto regionGate = std::make_shared<LoaderWorkGate>();
    auto payloadGate = std::make_shared<LoaderWorkGate>();
    AsyncChunkLoader loader(
        ctx.service,
        ctx.context,
        world,
        generator->config().world.version,
        1,
        1,
        1,
        generator);
    LoaderWorkRelease releaseIoAndRegion(ioGate, regionGate);
    LoaderWorkRelease releaseWorkerAndPayload(workerGate, payloadGate);
    loader.setPrefetchRadius(0);
    loader.setLoadQueueLimit(1);
    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
        setRegionLoadStartCallback(
            loader, [regionGate]() { regionGate->enterAndWait(); });
    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
        setPayloadBuildStartCallback(
            loader, [payloadGate]() { payloadGate->enterAndWait(); });
    CHECK_NE(
        Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
            enqueueIoPoolJob(
                loader, [ioGate]() { ioGate->enterAndWait(); }),
        static_cast<Rigel::Voxel::detail::ThreadPool::JobId>(0));
    CHECK(ioGate->waitUntilEntered());
    CHECK_NE(
        Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
            enqueueWorkerPoolJob(
                loader, [workerGate]() { workerGate->enterAndWait(); }),
        static_cast<Rigel::Voxel::detail::ThreadPool::JobId>(0));
    CHECK(workerGate->waitUntilEntered());

    CHECK_EQ(
        loader.request(makeLoadRequest(active)),
        ChunkLoadRequestResult::Queued);
    const auto regionQueued = ChunkLoadExecutionState{
        ChunkLoadExecutionOwner::Region,
        ChunkLoadExecutionPhase::PoolQueued};
    CHECK_EQ(loader.executionState(active), regionQueued);

    CHECK_EQ(
        loader.request(makeLoadRequest(deferred)),
        ChunkLoadRequestResult::Deferred);
    const auto schedulerPending = ChunkLoadExecutionState{
        ChunkLoadExecutionOwner::Region,
        ChunkLoadExecutionPhase::SchedulerPending};
    CHECK_EQ(loader.executionState(deferred), schedulerPending);
    loader.cancel(deferred);
    CHECK(!loader.executionState(deferred).has_value());

    ioGate->release();
    CHECK(regionGate->waitUntilEntered());
    const auto regionRunning = ChunkLoadExecutionState{
        ChunkLoadExecutionOwner::Region,
        ChunkLoadExecutionPhase::WorkerRunning};
    CHECK_EQ(loader.executionState(active), regionRunning);

    regionGate->release();
    CHECK(waitForRegionCompletion(loader));
    const auto regionPublished = ChunkLoadExecutionState{
        ChunkLoadExecutionOwner::Region,
        ChunkLoadExecutionPhase::ResultPublished};
    CHECK_EQ(loader.executionState(active), regionPublished);

    CHECK(loader.drainCompletions(1).empty());
    const auto payloadQueued = ChunkLoadExecutionState{
        ChunkLoadExecutionOwner::Payload,
        ChunkLoadExecutionPhase::PoolQueued};
    CHECK_EQ(loader.executionState(active), payloadQueued);
    workerGate->release();
    CHECK(payloadGate->waitUntilEntered());
    const auto payloadRunning = ChunkLoadExecutionState{
        ChunkLoadExecutionOwner::Payload,
        ChunkLoadExecutionPhase::WorkerRunning};
    CHECK_EQ(loader.executionState(active), payloadRunning);

    payloadGate->release();
    CHECK(waitForPayloadCompletions(loader, 1));
    const auto payloadPublished = ChunkLoadExecutionState{
        ChunkLoadExecutionOwner::Payload,
        ChunkLoadExecutionPhase::ResultPublished};
    CHECK_EQ(loader.executionState(active), payloadPublished);

    const auto completions = loader.drainCompletions(1);
    CHECK_EQ(completions.size(), static_cast<size_t>(1));
    CHECK_EQ(completions.front().coord, active);
    CHECK_EQ(completions.front().outcome, ChunkLoadOutcome::Loaded);
    CHECK(!loader.executionState(active).has_value());
    CHECK(world.chunkManager().getChunk(active) != nullptr);
    CHECK(loader.workCount().empty());
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
    auto regionGate = std::make_shared<LoaderWorkGate>();
    auto payloadGate = std::make_shared<LoaderWorkGate>();
    AsyncChunkLoader loader(
        ctx.service,
        ctx.context,
        world,
        generator->config().world.version,
        1,
        0,
        0,
        generator);
    LoaderWorkRelease releaseOnExit(regionGate, payloadGate);
    loader.setPrefetchRadius(0);
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
    auto stalePayloadGate = std::make_shared<LoaderWorkGate>();
    auto restartedPayloadGate = std::make_shared<LoaderWorkGate>();
    std::atomic<size_t> payloadStarts = 0;
    AsyncChunkLoader loader(
        ctx.service,
        ctx.context,
        world,
        generator->config().world.version,
        0,
        1,
        0,
        generator);
    LoaderWorkRelease releaseOnExit(stalePayloadGate, restartedPayloadGate);
    loader.setPrefetchRadius(0);
    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
        setPayloadBuildStartCallback(
            loader,
            [stalePayloadGate, restartedPayloadGate, &payloadStarts]() {
                size_t startIndex = payloadStarts.fetch_add(1);
                if (startIndex == 0) {
                    stalePayloadGate->enterAndWait();
                } else if (startIndex == 2) {
                    restartedPayloadGate->enterAndWait();
                }
            });

    const ChunkLoadRequest staleRequest = makeLoadRequest(staleCoord);
    CHECK_EQ(loader.request(staleRequest), ChunkLoadRequestResult::Queued);
    loader.drainCompletions(1);
    CHECK(stalePayloadGate->waitUntilEntered());

    Chunk& dirty = world.chunkManager().getOrCreateChunk(persistedCoord);
    dirty.fill(BlockState{edited}, registry);
    dirty.setWorldGenVersion(generator->config().world.version);
    CHECK(loader.persistChunk(persistedCoord));
    evictCleanChunk(world, registry, generator, persistedCoord);

    CHECK_EQ(loader.request(makeLoadRequest(refillCoord)), ChunkLoadRequestResult::Queued);
    loader.drainCompletions(1);

    stalePayloadGate->release();
    CHECK(waitForPayloadCompletions(loader, 2));
    std::vector<ChunkLoadCompletion> resolved = loader.drainCompletions(8);
    CHECK(restartedPayloadGate->waitUntilEntered());

    CHECK_EQ(resolved.size(), static_cast<size_t>(1));
    if (!resolved.empty()) {
        CHECK_EQ(resolved.front().coord, refillCoord);
        CHECK_EQ(resolved.front().outcome, ChunkLoadOutcome::Loaded);
    }
    CHECK_EQ(payloadStarts.load(), static_cast<size_t>(3));
    CHECK(loader.isPending(staleCoord));
    CHECK(!loader.isPending(refillCoord));
    CHECK(!world.chunkManager().hasChunk(staleCoord));
    CHECK(world.chunkManager().hasChunk(refillCoord));
    CHECK(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
              payloadRequestInFlight(
                  loader, staleCoord, staleRequest.requestId));

    restartedPayloadGate->release();
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

void checkCancelledPayloadCannotCompleteReplacement(
    ChunkLoadRequestId requestId) {
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
    auto stalePayloadGate = std::make_shared<LoaderWorkGate>();
    auto replacementPayloadGate = std::make_shared<LoaderWorkGate>();
    std::atomic<size_t> payloadStarts = 0;
    AsyncChunkLoader loader(
        ctx.service,
        ctx.context,
        world,
        generator->config().world.version,
        0,
        1,
        0,
        generator);
    LoaderWorkRelease releaseOnExit(stalePayloadGate, replacementPayloadGate);
    loader.setPrefetchRadius(0);
    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
        setPayloadBuildStartCallback(
            loader,
            [stalePayloadGate, replacementPayloadGate, &payloadStarts]() {
                size_t startIndex = payloadStarts.fetch_add(1);
                if (startIndex == 0) {
                    stalePayloadGate->enterAndWait();
                } else if (startIndex == 1) {
                    replacementPayloadGate->enterAndWait();
                }
            });

    const ChunkLoadRequest request{coord, requestId};
    CHECK_EQ(loader.request(request), ChunkLoadRequestResult::Queued);
    CHECK(loader.drainCompletions(1).empty());
    CHECK(stalePayloadGate->waitUntilEntered());

    loader.cancel(coord);
    CHECK(!loader.isPending(coord));
    CHECK_EQ(loader.workCount().pending, static_cast<size_t>(0));
    CHECK_EQ(loader.workCount().inFlight, static_cast<size_t>(1));
    CHECK_EQ(loader.workCount().started, static_cast<uint64_t>(1));

    CHECK_EQ(loader.request(request), ChunkLoadRequestResult::Queued);
    CHECK(loader.isPending(coord));
    CHECK_EQ(loader.workCount().pending, static_cast<size_t>(1));
    CHECK_EQ(loader.workCount().inFlight, static_cast<size_t>(1));
    CHECK_EQ(loader.workCount().started, static_cast<uint64_t>(2));
    const auto replacementWaiting = ChunkLoadExecutionState{
        ChunkLoadExecutionOwner::Payload,
        ChunkLoadExecutionPhase::SchedulerPending};
    CHECK_EQ(loader.executionState(coord), replacementWaiting);

    stalePayloadGate->release();
    CHECK(waitForPayloadCompletions(loader, 1));
    CHECK(loader.drainCompletions(8).empty());
    CHECK(replacementPayloadGate->waitUntilEntered());
    CHECK_EQ(payloadStarts.load(), static_cast<size_t>(2));
    CHECK(loader.isPending(coord));
    CHECK_EQ(loader.workCount().pending, static_cast<size_t>(1));
    CHECK_EQ(loader.workCount().inFlight, static_cast<size_t>(1));
    CHECK(!world.chunkManager().hasChunk(coord));
    const auto replacementRunning = ChunkLoadExecutionState{
        ChunkLoadExecutionOwner::Payload,
        ChunkLoadExecutionPhase::WorkerRunning};
    CHECK_EQ(loader.executionState(coord), replacementRunning);

    replacementPayloadGate->release();
    CHECK(waitForPayloadCompletions(loader, 1));
    std::vector<ChunkLoadCompletion> resolved = loader.drainCompletions(8);
    CHECK_EQ(resolved.size(), static_cast<size_t>(1));
    CHECK_EQ(resolved.front().coord, coord);
    CHECK_EQ(resolved.front().requestId, requestId);
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
    CHECK(loader.drainCompletions(8).empty());
}

TEST_CASE(AsyncChunkLoader_CancelledPayloadCannotCompleteReusedRequestId) {
    checkCancelledPayloadCannotCompleteReplacement(41);
}

TEST_CASE(AsyncChunkLoader_CancelledPayloadCannotCompleteZeroRequestId) {
    checkCancelledPayloadCannotCompleteReplacement(0);
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
    auto payloadGate = std::make_shared<LoaderWorkGate>();
    auto unusedGate = std::make_shared<LoaderWorkGate>();
    std::atomic<size_t> payloadStarts = 0;
    AsyncChunkLoader loader(
        ctx.service,
        ctx.context,
        world,
        generator->config().world.version,
        0,
        1,
        0,
        generator);
    LoaderWorkRelease releaseOnExit(unusedGate, payloadGate);
    loader.setPrefetchRadius(0);
    loader.setLoadQueueLimit(1);
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
    BlockID newerEdit = registerTestBlock(registry, "rigel:resident_newer_edit");
    const ChunkCoord coord{0, 0, 0};
    ChunkData payload = buildPayload(
        coord, registry, {persisted}, false, std::nullopt, false);

    MemoryContext ctx;
    saveRegionForPayload(
        ctx.service, ctx.context, "rigel:default", coord, payload);
    auto payloadGate = std::make_shared<LoaderWorkGate>();
    auto unusedGate = std::make_shared<LoaderWorkGate>();
    auto loader = std::make_shared<AsyncChunkLoader>(
        ctx.service,
        ctx.context,
        world,
        generator->config().world.version,
        0,
        1,
        0,
        generator);
    LoaderWorkRelease releaseOnExit(unusedGate, payloadGate);
    loader->setPrefetchRadius(0);
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
    streamer.setChunkLoadDiagnosticsCallback([loader]() {
        return loader->diagnostics();
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
    Chunk* const replacementIdentity = &replacement;

    streamer.update(coord.toWorldCenter());
    CHECK(!loader->isPending(coord));
    CHECK_EQ(loader->workCount().pending, static_cast<size_t>(0));
    CHECK_EQ(loader->workCount().inFlight, static_cast<size_t>(1));
    CHECK_EQ(loader->workCount().started, static_cast<uint64_t>(1));
    CHECK_EQ(streamer.workMetrics().meshJobsStarted,
             static_cast<uint64_t>(1));

    replacement.setBlock(0, 0, 0, BlockState{newerEdit}, registry);
    const uint32_t localWorldVersion = replacement.worldGenVersion();
    const uint32_t localMeshRevision = replacement.meshRevision();
    std::array<BlockState, Chunk::VOLUME> localBlocks{};
    replacement.copyBlocks(localBlocks);
    CHECK(replacement.isDirty());
    CHECK(replacement.isPersistDirty());
    CHECK(!replacement.loadedFromDisk());

    payloadGate->release();
    CHECK(waitForPayloadCompletions(*loader, 1));
    streamer.processCompletions();

    const Chunk* resident = world.chunkManager().getChunk(coord);
    CHECK_EQ(resident, replacementIdentity);
    if (resident) {
        std::array<BlockState, Chunk::VOLUME> residentBlocks{};
        resident->copyBlocks(residentBlocks);
        CHECK_EQ(residentBlocks, localBlocks);
        CHECK_EQ(resident->getBlock(0, 0, 0).id, newerEdit);
        CHECK_EQ(resident->worldGenVersion(), localWorldVersion);
        CHECK_EQ(resident->meshRevision(), localMeshRevision);
        CHECK(!resident->loadedFromDisk());
        CHECK(resident->isDirty());
        CHECK(resident->isPersistDirty());
    }
    CHECK_EQ(loader->workCount().pending, static_cast<size_t>(0));
    CHECK_EQ(loader->workCount().inFlight, static_cast<size_t>(0));
    CHECK_EQ(loader->workCount().started, static_cast<uint64_t>(1));
    CHECK(streamer.diagnostics().chunkLoad.empty());
    CHECK_EQ(streamer.workMetrics().meshJobsCompleted,
             static_cast<uint64_t>(1));
    CHECK_EQ(streamer.workMetrics().meshJobsAccepted,
             static_cast<uint64_t>(0));
    CHECK_EQ(streamer.workMetrics().meshJobsRejectedStale,
             static_cast<uint64_t>(1));
    CHECK_EQ(streamer.diagnostics().mesh.pending, static_cast<size_t>(1));

    streamer.update(coord.toWorldCenter());
    CHECK_EQ(streamer.workMetrics().meshJobsStarted,
             static_cast<uint64_t>(2));
    streamer.processCompletions();
    CHECK_EQ(world.chunkManager().getChunk(coord), replacementIdentity);
    CHECK_EQ(streamer.workMetrics().meshJobsCompleted,
             static_cast<uint64_t>(2));
    CHECK_EQ(streamer.workMetrics().meshJobsAccepted,
             static_cast<uint64_t>(1));
    CHECK_EQ(streamer.workMetrics().meshJobsRejectedStale,
             static_cast<uint64_t>(1));
    CHECK(meshStore.contains(coord));
    CHECK(!replacement.isDirty());
    CHECK(replacement.isPersistDirty());
    CHECK(!replacement.loadedFromDisk());

    streamer.markSpawnDiscoveryComplete();
    for (uint32_t stable = 0;
         stable < StreamingDiagnosticSnapshot::QuiescenceUpdateWindow;
         ++stable) {
        streamer.update(coord.toWorldCenter());
        streamer.processCompletions();
    }
    CHECK_EQ(loader->workCount().started, static_cast<uint64_t>(1));
    CHECK_EQ(streamer.workMetrics().meshJobsStarted,
             static_cast<uint64_t>(2));
    CHECK(streamer.diagnostics().chunkLoad.empty());
    CHECK(streamer.diagnostics().mesh.empty());
    CHECK_EQ(streamer.diagnostics().state,
             StreamingLifecycleState::Quiescent);
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
    const size_t identityReadsAfterConstruction =
        failingStorage->identityReadAttempts();
    CHECK(identityReadsAfterConstruction > 0);

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
    CHECK_EQ(
        failingStorage->identityReadAttempts(),
        identityReadsAfterConstruction);

    for (int update = 0; update < 59; ++update) {
        streamer.update(distant);
    }
    CHECK(world.chunkManager().hasChunk(coord));
    CHECK_EQ(failingStorage->writeAttempts(), static_cast<size_t>(1));

    streamer.update(distant);
    CHECK(!world.chunkManager().hasChunk(coord));
    CHECK_EQ(failingStorage->writeAttempts(), static_cast<size_t>(2));
    CHECK_EQ(
        failingStorage->identityReadAttempts(),
        identityReadsAfterConstruction);
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
    const auto savedGenerator = generator;
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
    streamer.getDebugStates(states, coord, 0);
    const auto failedEviction = std::find_if(
        states.begin(), states.end(), [coord](const auto& state) {
            return state.coord == coord;
        });
    CHECK(failedEviction != states.end());
    if (failedEviction != states.end()) {
        CHECK_EQ(failedEviction->state,
                 ChunkStreamer::DebugState::TerminalFailure);
        CHECK_EQ(failedEviction->failure,
                 ChunkStreamer::DebugFailure::Eviction);
    }

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
    reconstructed.setGenerator(savedGenerator);
    AsyncChunkLoader reconstructedLoader(
        ctx.service,
        ctx.context,
        reconstructed,
        savedGenerator->config().world.version,
        0,
        0,
        0,
        savedGenerator);
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
    const auto metrics = loader.metrics();
    CHECK_EQ(metrics.directOrigin.logicalAdmissions,
             static_cast<uint64_t>(4));
    CHECK_EQ(metrics.directOrigin.retryAdmissions,
             static_cast<uint64_t>(3));
    CHECK_EQ(metrics.directOrigin.inlineExecutions,
             static_cast<uint64_t>(4));
    CHECK_EQ(metrics.directOrigin.poolSubmissions,
             static_cast<uint64_t>(0));
    CHECK_EQ(metrics.directOrigin.resultsPublished,
             static_cast<uint64_t>(4));
    CHECK_EQ(metrics.directOrigin.resultsDrained,
             static_cast<uint64_t>(4));
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
    const auto retryWaiting = ChunkLoadExecutionState{
        ChunkLoadExecutionOwner::Region,
        ChunkLoadExecutionPhase::RetryWaiting};
    CHECK_EQ(loader.executionState(retryCoord), retryWaiting);
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

TEST_CASE(AsyncChunkLoader_PayloadFailureReportsPayloadTerminalOwner) {
    WorldResources resources;
    World world;
    world.initialize(resources);
    auto& registry = resources.registry();
    auto generator = makeGenerator(registry);
    world.setGenerator(generator);

    const BlockID persisted =
        registerTestBlock(registry, "rigel:payload_terminal_owner");
    const ChunkCoord coord{0, 0, 0};
    MemoryContext ctx;
    saveRegionForPayload(
        ctx.service,
        ctx.context,
        "rigel:default",
        coord,
        buildPayload(
            coord, registry, {persisted}, false, std::nullopt, false));

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
    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
        setPayloadBuildStartCallback(loader, []() {
            throw std::runtime_error("injected payload construction failure");
        });

    CHECK_EQ(
        loader.request(makeLoadRequest(coord)),
        ChunkLoadRequestResult::Queued);
    CHECK(loader.drainCompletions(1).empty());

    const auto payloadTerminal = ChunkLoadExecutionState{
        ChunkLoadExecutionOwner::Payload,
        ChunkLoadExecutionPhase::TerminalFailed};
    CHECK_EQ(loader.executionState(coord), payloadTerminal);
    CHECK(loader.isPending(coord));
    CHECK_EQ(loader.workCount().pending, static_cast<size_t>(1));
    CHECK_EQ(loader.workCount().inFlight, static_cast<size_t>(0));
    CHECK_EQ(loader.workCount().terminalErrors, static_cast<size_t>(1));
    CHECK(loader.workCount().lastError.find("injected payload construction failure") !=
          std::string::npos);

    loader.cancel(coord);
    CHECK(!loader.executionState(coord).has_value());
    CHECK(!loader.isPending(coord));
    CHECK(loader.workCount().empty());
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
    const auto regionTerminal = ChunkLoadExecutionState{
        ChunkLoadExecutionOwner::Region,
        ChunkLoadExecutionPhase::TerminalFailed};
    CHECK_EQ(loader.executionState(coord), regionTerminal);
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

TEST_CASE(AsyncChunkLoader_RegionMetricsAccountDirectAndSpeculativeJobs) {
    WorldResources resources;
    World world;
    world.initialize(resources);
    auto generator = makeGenerator(resources.registry());
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
    loader.setMaxInFlightRegions(16);
    loader.setPrefetchRadius(1);
    loader.setPrefetchPerRequest(2);

    const ChunkCoord requested{0, 0, 0};
    CHECK_EQ(loader.request(makeLoadRequest(requested)),
             ChunkLoadRequestResult::Queued);

    auto active = loader.metrics();
    CHECK_EQ(active.directOrigin.logicalAdmissions, static_cast<uint64_t>(1));
    CHECK_EQ(active.directOrigin.inlineExecutions, static_cast<uint64_t>(1));
    CHECK_EQ(active.directOrigin.resultsPublished, static_cast<uint64_t>(1));
    CHECK_EQ(active.directOrigin.missingProbes, static_cast<uint64_t>(1));
    CHECK_EQ(active.speculativeOrigin.logicalAdmissions, static_cast<uint64_t>(2));
    CHECK_EQ(active.speculativeOrigin.inlineExecutions, static_cast<uint64_t>(0));
    CHECK_EQ(active.speculativeOrigin.resultsPublished, static_cast<uint64_t>(0));
    CHECK_EQ(active.speculativeOrigin.missingProbes, static_cast<uint64_t>(0));
    CHECK_EQ(active.demandOwnedDispatchedUndrained, static_cast<size_t>(1));
    CHECK_EQ(active.speculativeOwnedDispatchedUndrained, static_cast<size_t>(0));
    CHECK_EQ(active.speculativeOwnedQueued, static_cast<size_t>(2));
    CHECK(active.directOrigin.maxAdmissionToWorkerStartNanoseconds <=
          active.directOrigin.admissionToWorkerStartNanoseconds);
    CHECK(active.speculativeOrigin.maxAdmissionToWorkerStartNanoseconds <=
          active.speculativeOrigin.admissionToWorkerStartNanoseconds);
    CHECK(active.directOrigin.maxWorkerExecutionNanoseconds <=
          active.directOrigin.workerExecutionNanoseconds);
    CHECK(active.speculativeOrigin.maxWorkerExecutionNanoseconds <=
          active.speculativeOrigin.workerExecutionNanoseconds);

    std::vector<ChunkLoadCompletion> resolved = loader.drainCompletions(8);
    CHECK_EQ(resolved.size(), static_cast<size_t>(1));
    CHECK_EQ(resolved.front().coord, requested);
    CHECK_EQ(resolved.front().outcome, ChunkLoadOutcome::Missing);
    const auto firstDrain = loader.metrics();
    CHECK_EQ(firstDrain.directOrigin.resultsDrained, static_cast<uint64_t>(1));
    CHECK_EQ(firstDrain.speculativeOrigin.inlineExecutions,
             static_cast<uint64_t>(1));
    CHECK_EQ(firstDrain.speculativeOrigin.resultsPublished,
             static_cast<uint64_t>(1));
    CHECK_EQ(firstDrain.speculativeOrigin.resultsDrained,
             static_cast<uint64_t>(0));
    CHECK_EQ(firstDrain.speculativeOwnedDispatchedUndrained,
             static_cast<size_t>(1));
    CHECK_EQ(firstDrain.speculativeOwnedQueued, static_cast<size_t>(1));

    CHECK(drainRegionJobsUntilSettled(loader, resolved));

    auto settled = loader.metrics();
    CHECK_EQ(settled.demandOwnedDispatchedUndrained, static_cast<size_t>(0));
    CHECK_EQ(settled.speculativeOwnedDispatchedUndrained, static_cast<size_t>(0));
    CHECK_EQ(settled.directOrigin.logicalAdmissions, settled.directOrigin.resultsPublished);
    CHECK_EQ(settled.speculativeOrigin.logicalAdmissions, settled.speculativeOrigin.resultsPublished);
    CHECK_EQ(loader.workCount().pending, static_cast<size_t>(0));
    CHECK_EQ(loader.workCount().inFlight, static_cast<size_t>(0));
}

TEST_CASE(AsyncChunkLoader_InlineZeroCapBoundsMaximalPrefetchDispatch) {
    WorldResources resources;
    World world;
    world.initialize(resources);
    auto generator = makeGenerator(resources.registry());
    world.setGenerator(generator);

    MemoryContext ctx;
    AsyncChunkLoader loader(
        ctx.service,
        ctx.context,
        world,
        generator->config().world.version,
        0,
        0,
        12,
        generator);
    loader.setMaxInFlightRegions(0);
    loader.setPrefetchRadius(4);
    loader.setPrefetchPerRequest(0);

    CHECK_EQ(loader.request(makeLoadRequest({0, 0, 0})),
             ChunkLoadRequestResult::Queued);

    const auto admitted = loader.metrics();
    CHECK_EQ(admitted.directOrigin.logicalAdmissions,
             static_cast<uint64_t>(1));
    CHECK_EQ(admitted.speculativeOrigin.logicalAdmissions,
             static_cast<uint64_t>(64));
    CHECK_EQ(admitted.directOrigin.inlineExecutions,
             static_cast<uint64_t>(1));
    CHECK_EQ(admitted.speculativeOrigin.inlineExecutions,
             static_cast<uint64_t>(0));
    CHECK_EQ(admitted.demandOwnedQueued, static_cast<size_t>(0));
    CHECK_EQ(admitted.speculativeOwnedQueued, static_cast<size_t>(64));
    CHECK_EQ(admitted.demandOwnedDispatchedUndrained,
             static_cast<size_t>(1));
    CHECK_EQ(admitted.speculativeOwnedDispatchedUndrained,
             static_cast<size_t>(0));
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionCompletionCount(loader),
             static_cast<size_t>(1));

    std::vector<ChunkLoadCompletion> resolved;
    CHECK(drainRegionJobsUntilSettled(loader, resolved));
    CHECK_EQ(resolved.size(), static_cast<size_t>(1));
    const auto settled = loader.metrics();
    CHECK_EQ(settled.directOrigin.inlineExecutions,
             static_cast<uint64_t>(1));
    CHECK_EQ(settled.speculativeOrigin.inlineExecutions,
             static_cast<uint64_t>(64));
    CHECK_EQ(settled.directOrigin.resultsPublished,
             settled.directOrigin.resultsDrained);
    CHECK_EQ(settled.speculativeOrigin.resultsPublished,
             settled.speculativeOrigin.resultsDrained);
    CHECK_EQ(settled.demandOwnedQueued, static_cast<size_t>(0));
    CHECK_EQ(settled.speculativeOwnedQueued, static_cast<size_t>(0));
    CHECK_EQ(settled.demandOwnedDispatchedUndrained,
             static_cast<size_t>(0));
    CHECK_EQ(settled.speculativeOwnedDispatchedUndrained,
             static_cast<size_t>(0));
}

TEST_CASE(AsyncChunkLoader_PublishesRegionResultAndAccountingTogether) {
    WorldResources resources;
    World world;
    world.initialize(resources);
    auto generator = makeGenerator(resources.registry());
    world.setGenerator(generator);

    MemoryContext ctx;
    auto publicationGate = std::make_shared<LoaderWorkGate>();
    auto unusedGate = std::make_shared<LoaderWorkGate>();
    AsyncChunkLoader loader(
        ctx.service,
        ctx.context,
        world,
        generator->config().world.version,
        1,
        0,
        0,
        generator);
    LoaderWorkRelease releaseOnExit(publicationGate, unusedGate);
    loader.setPrefetchRadius(0);
    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
        setRegionResultReadyToPublishCallback(
            loader,
            [publicationGate]() { publicationGate->enterAndWait(); });

    CHECK_EQ(loader.request(makeLoadRequest({0, 0, 0})),
             ChunkLoadRequestResult::Queued);
    CHECK(publicationGate->waitUntilEntered());

    const auto unpublished = loader.metrics();
    CHECK_EQ(unpublished.directOrigin.resultsPublished,
             static_cast<uint64_t>(0));
    CHECK_EQ(unpublished.directOrigin.resultsDrained,
             static_cast<uint64_t>(0));
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionCompletionCount(loader),
             static_cast<size_t>(0));
    CHECK(loader.drainCompletions(1).empty());
    CHECK(unpublished.directOrigin.resultsDrained <=
          unpublished.directOrigin.resultsPublished);

    publicationGate->release();
    CHECK(waitForPublishedRegionJobs(loader, 1));
    const auto published = loader.metrics();
    CHECK_EQ(published.directOrigin.resultsPublished,
             static_cast<uint64_t>(1));
    CHECK_EQ(published.directOrigin.resultsDrained,
             static_cast<uint64_t>(0));
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionCompletionCount(loader),
             static_cast<size_t>(1));
    CHECK(published.directOrigin.resultsDrained <=
          published.directOrigin.resultsPublished);

    const auto resolved = loader.drainCompletions(1);
    CHECK_EQ(resolved.size(), static_cast<size_t>(1));
    const auto settled = loader.metrics();
    CHECK_EQ(settled.directOrigin.resultsPublished,
             static_cast<uint64_t>(1));
    CHECK_EQ(settled.directOrigin.resultsDrained,
             static_cast<uint64_t>(1));
    CHECK(settled.directOrigin.resultsDrained <=
          settled.directOrigin.resultsPublished);
}

TEST_CASE(AsyncChunkLoader_InlineRegionTimingUsesScriptedMetricClock) {
    WorldResources resources;
    World world;
    world.initialize(resources);
    auto generator = makeGenerator(resources.registry());
    world.setGenerator(generator);

    MemoryContext ctx;
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

    const std::array<int64_t, 3> clockNanoseconds{10, 25, 65};
    size_t clockIndex = 0;
    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::setMetricClock(
        loader,
        [&clockNanoseconds, &clockIndex]() {
            const size_t index = clockIndex++;
            const int64_t value = index < clockNanoseconds.size()
                ? clockNanoseconds[index]
                : clockNanoseconds.back();
            return std::chrono::steady_clock::time_point{
                std::chrono::nanoseconds(value)};
        });

    CHECK_EQ(loader.request(makeLoadRequest({0, 0, 0})),
             ChunkLoadRequestResult::Queued);
    const auto published = loader.metrics();
    CHECK_EQ(clockIndex, clockNanoseconds.size());
    CHECK_EQ(published.directOrigin.logicalAdmissions,
             static_cast<uint64_t>(1));
    CHECK_EQ(published.directOrigin.inlineExecutions,
             static_cast<uint64_t>(1));
    CHECK_EQ(published.directOrigin.poolSubmissions,
             static_cast<uint64_t>(0));
    CHECK_EQ(published.directOrigin.resultsPublished,
             static_cast<uint64_t>(1));
    CHECK_EQ(published.directOrigin.resultsDrained,
             static_cast<uint64_t>(0));
    CHECK_EQ(published.directOrigin.admissionToWorkerStartNanoseconds,
             static_cast<uint64_t>(15));
    CHECK_EQ(published.directOrigin.maxAdmissionToWorkerStartNanoseconds,
             static_cast<uint64_t>(15));
    CHECK_EQ(published.directOrigin.workerExecutionNanoseconds,
             static_cast<uint64_t>(40));
    CHECK_EQ(published.directOrigin.maxWorkerExecutionNanoseconds,
             static_cast<uint64_t>(40));
    CHECK_EQ(published.demandOwnedDispatchedUndrained,
             static_cast<size_t>(1));

    const auto resolved = loader.drainCompletions(1);
    CHECK_EQ(resolved.size(), static_cast<size_t>(1));
    const auto settled = loader.metrics();
    CHECK_EQ(settled.directOrigin.resultsDrained,
             static_cast<uint64_t>(1));
    CHECK_EQ(settled.demandOwnedQueued, static_cast<size_t>(0));
    CHECK_EQ(settled.speculativeOwnedQueued, static_cast<size_t>(0));
    CHECK_EQ(settled.demandOwnedDispatchedUndrained,
             static_cast<size_t>(0));
    CHECK_EQ(settled.speculativeOwnedDispatchedUndrained,
             static_cast<size_t>(0));
}

TEST_CASE(AsyncChunkLoader_PoolYieldResubmissionRetainsAdmissionTiming) {
    WorldResources resources;
    World world;
    world.initialize(resources);
    auto generator = makeGenerator(resources.registry());
    world.setGenerator(generator);

    MemoryContext ctx;
    const ChunkCoord directCoord{160, 0, 0};
    const RegionKey directKey{"rigel:default", 10, 0, 0};
    const RegionKey speculativeKey{"rigel:default", 1, 0, 0};
    {
        auto format = ctx.service.openFormat(ctx.context);
        ChunkRegionSnapshot region;
        region.key = directKey;
        format->chunkContainer().saveRegion(region);
    }

    auto poolGate = std::make_shared<LoaderWorkGate>();
    auto barrierGate = std::make_shared<LoaderWorkGate>();
    auto speculativeStartGate = std::make_shared<LoaderWorkGate>();
    auto unusedPayloadGate = std::make_shared<LoaderWorkGate>();
    const std::array<int64_t, 6> clockNanoseconds{10, 20, 30, 40, 50, 60};
    std::atomic<size_t> clockIndex{0};
    AsyncChunkLoader loader(
        ctx.service,
        ctx.context,
        world,
        generator->config().world.version,
        1,
        0,
        1,
        generator);
    LoaderWorkRelease releaseOnExit(poolGate, unusedPayloadGate);
    LoaderWorkRelease releaseResubmissionOnExit(
        barrierGate, speculativeStartGate);
    loader.setMaxInFlightRegions(1);
    loader.setPrefetchRadius(0);
    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
        setRegionLoadStartObserver(
            loader,
            [speculativeKey, speculativeStartGate](
                const RegionKey& key, bool) {
                if (key == speculativeKey) {
                    speculativeStartGate->enterAndWait();
                }
            });
    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::enqueueIoPoolJob(
        loader, [poolGate]() { poolGate->enterAndWait(); });
    CHECK(poolGate->waitUntilEntered());

    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::setMetricClock(
        loader,
        [&clockNanoseconds, &clockIndex]() {
            const size_t index =
                clockIndex.fetch_add(1, std::memory_order_relaxed);
            const int64_t value = index < clockNanoseconds.size()
                ? clockNanoseconds[index]
                : clockNanoseconds.back();
            return std::chrono::steady_clock::time_point{
                std::chrono::nanoseconds(value)};
        });

    CHECK(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
              queueSpeculativeRegionLoad(loader, speculativeKey));
    const auto speculativeIdentity = Rigel::Persistence::detail::
        AsyncChunkLoaderTestAccess::regionJobIdentity(
            loader, speculativeKey);
    CHECK_EQ(loader.request(makeLoadRequest(directCoord)),
             ChunkLoadRequestResult::Queued);

    const auto yielded = loader.metrics();
    CHECK_EQ(yielded.speculativeOrigin.poolSubmissions,
             static_cast<uint64_t>(1));
    CHECK_EQ(yielded.speculativeOrigin.successfulPoolYields,
             static_cast<uint64_t>(1));
    CHECK_EQ(yielded.speculativeOrigin.poolResubmissions,
             static_cast<uint64_t>(0));
    CHECK_EQ(yielded.speculativeOwnedQueued, static_cast<size_t>(1));
    CHECK_EQ(yielded.demandOwnedDispatchedUndrained,
             static_cast<size_t>(1));
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionJobIdentity(loader, speculativeKey),
             speculativeIdentity);
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionPoolJobId(loader, speculativeKey),
             Rigel::Voxel::detail::ThreadPool::JobId{0});
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionLoadAttemptCount(loader, speculativeKey),
             static_cast<size_t>(0));
    CHECK_EQ(
        Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
            regionJobPhase(loader, speculativeKey),
        std::optional<ChunkLoadExecutionPhase>{
            ChunkLoadExecutionPhase::SchedulerPending});

    CHECK_NE(
        Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
            enqueueIoPoolJob(
                loader,
                [barrierGate]() { barrierGate->enterAndWait(); }),
        Rigel::Voxel::detail::ThreadPool::JobId{0});

    poolGate->release();
    CHECK(barrierGate->waitUntilEntered());
    CHECK(waitForPublishedRegionJobs(loader, 1));
    std::vector<ChunkLoadCompletion> resolved = loader.drainCompletions(1);
    CHECK_EQ(resolved.size(), static_cast<size_t>(1));
    CHECK(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
              regionPoolJobId(loader, speculativeKey) != 0);
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionLoadAttemptCount(loader, speculativeKey),
             static_cast<size_t>(1));
    CHECK_EQ(
        Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
            regionJobPhase(loader, speculativeKey),
        std::optional<ChunkLoadExecutionPhase>{
            ChunkLoadExecutionPhase::PoolQueued});

    barrierGate->release();
    CHECK(speculativeStartGate->waitUntilEntered());
    CHECK_EQ(
        Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
            regionJobPhase(loader, speculativeKey),
        std::optional<ChunkLoadExecutionPhase>{
            ChunkLoadExecutionPhase::WorkerRunning});
    speculativeStartGate->release();
    CHECK(waitForPublishedRegionJobs(loader, 1));
    CHECK_EQ(
        Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
            regionJobPhase(loader, speculativeKey),
        std::optional<ChunkLoadExecutionPhase>{
            ChunkLoadExecutionPhase::ResultPublished});
    auto speculativeDrain = loader.drainCompletions(1);
    resolved.insert(
        resolved.end(), speculativeDrain.begin(), speculativeDrain.end());
    CHECK_EQ(resolved.size(), static_cast<size_t>(1));
    CHECK_EQ(clockIndex.load(std::memory_order_relaxed),
             clockNanoseconds.size());

    const auto settled = loader.metrics();
    CHECK_EQ(settled.directOrigin.logicalAdmissions,
             static_cast<uint64_t>(1));
    CHECK_EQ(settled.directOrigin.poolSubmissions,
             static_cast<uint64_t>(1));
    CHECK_EQ(settled.directOrigin.poolWorkerStarts,
             static_cast<uint64_t>(1));
    CHECK_EQ(settled.directOrigin.admissionToWorkerStartNanoseconds,
             static_cast<uint64_t>(10));
    CHECK_EQ(settled.directOrigin.workerExecutionNanoseconds,
             static_cast<uint64_t>(10));
    CHECK_EQ(settled.speculativeOrigin.logicalAdmissions,
             static_cast<uint64_t>(1));
    CHECK_EQ(settled.speculativeOrigin.poolSubmissions,
             static_cast<uint64_t>(2));
    CHECK_EQ(settled.speculativeOrigin.poolResubmissions,
             static_cast<uint64_t>(1));
    CHECK_EQ(settled.speculativeOrigin.successfulPoolYields,
             static_cast<uint64_t>(1));
    CHECK_EQ(settled.speculativeOrigin.poolWorkerStarts,
             static_cast<uint64_t>(1));
    CHECK_EQ(settled.speculativeOrigin.admissionToWorkerStartNanoseconds,
             static_cast<uint64_t>(40));
    CHECK_EQ(settled.speculativeOrigin.maxAdmissionToWorkerStartNanoseconds,
             static_cast<uint64_t>(40));
    CHECK_EQ(settled.speculativeOrigin.workerExecutionNanoseconds,
             static_cast<uint64_t>(10));
    CHECK_EQ(settled.speculativeOrigin.maxWorkerExecutionNanoseconds,
             static_cast<uint64_t>(10));
    CHECK_EQ(settled.speculativeOrigin.resultsPublished,
             static_cast<uint64_t>(1));
    CHECK_EQ(settled.speculativeOrigin.resultsDrained,
             static_cast<uint64_t>(1));
    CHECK_EQ(settled.demandOwnedQueued, static_cast<size_t>(0));
    CHECK_EQ(settled.speculativeOwnedQueued, static_cast<size_t>(0));
    CHECK_EQ(settled.demandOwnedDispatchedUndrained,
             static_cast<size_t>(0));
    CHECK_EQ(settled.speculativeOwnedDispatchedUndrained,
             static_cast<size_t>(0));
    CHECK_EQ(settled.speculativePoolJobsPending, static_cast<size_t>(0));
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionOwnerCount(loader),
             static_cast<size_t>(0));
    CHECK(!Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
              regionPhysicallyInFlight(loader, speculativeKey));
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionCompletionCount(loader),
             static_cast<size_t>(0));
}

TEST_CASE(AsyncChunkLoader_InitialPoolSubmissionPrecedesObservableStart) {
    WorldResources resources;
    World world;
    world.initialize(resources);
    auto generator = makeGenerator(resources.registry());
    world.setGenerator(generator);

    MemoryContext ctx;
    auto workerStartGate = std::make_shared<LoaderWorkGate>();
    auto unusedPayloadGate = std::make_shared<LoaderWorkGate>();
    std::atomic<bool> enqueueReturnEntered{false};
    std::atomic<bool> enqueueReturnReleased{false};
    const ChunkLoadRequest request = makeLoadRequest({0, 0, 0});
    ChunkLoadRequestResult requestResult = ChunkLoadRequestResult::Missing;
    AsyncChunkLoader loader(
        ctx.service,
        ctx.context,
        world,
        generator->config().world.version,
        1,
        0,
        0,
        generator);
    std::jthread submitter;
    LoaderWorkRelease releaseWorkerOnExit(
        workerStartGate, unusedPayloadGate);
    AtomicFlagRelease releaseEnqueueOnExit(enqueueReturnReleased);
    loader.setMaxInFlightRegions(1);
    loader.setPrefetchRadius(0);
    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
        setRegionLoadStartObserver(
            loader,
            [workerStartGate](const RegionKey&, bool direct) {
                if (direct) {
                    workerStartGate->enterAndWait();
                }
            });
    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
        gateNextIoPoolEnqueueReturn(
            loader, enqueueReturnEntered, enqueueReturnReleased);

    submitter = std::jthread([&]() {
        requestResult = loader.request(request);
    });

    CHECK(waitUntilTrue(enqueueReturnEntered));
    CHECK(workerStartGate->waitUntilEntered());
    const auto boundary = Rigel::Persistence::detail::
        AsyncChunkLoaderTestAccess::directRegionBoundaryMetrics(loader);

    workerStartGate->release();
    releaseEnqueueOnExit.release();
    submitter.join();
    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::stopIoPool(loader);
    const auto resolved = loader.drainCompletions(1);
    const auto settled = loader.metrics();

    CHECK_EQ(requestResult, ChunkLoadRequestResult::Queued);
    CHECK_EQ(boundary.poolSubmissions, static_cast<uint64_t>(1));
    CHECK_EQ(boundary.poolResubmissions, static_cast<uint64_t>(0));
    CHECK_EQ(boundary.poolWorkerStarts, static_cast<uint64_t>(1));
    CHECK_EQ(boundary.resultsPublished, static_cast<uint64_t>(0));
    CHECK(boundary.resultsPublished <= boundary.poolWorkerStarts);
    CHECK(boundary.poolWorkerStarts <= boundary.poolSubmissions);
    CHECK_EQ(resolved.size(), static_cast<size_t>(1));
    CHECK_EQ(resolved.front().outcome, ChunkLoadOutcome::Missing);
    CHECK_EQ(settled.directOrigin.logicalAdmissions,
             static_cast<uint64_t>(1));
    CHECK_EQ(settled.directOrigin.poolSubmissions,
             static_cast<uint64_t>(1));
    CHECK_EQ(settled.directOrigin.poolResubmissions,
             static_cast<uint64_t>(0));
    CHECK_EQ(settled.directOrigin.poolWorkerStarts,
             static_cast<uint64_t>(1));
    CHECK_EQ(settled.directOrigin.resultsPublished,
             static_cast<uint64_t>(1));
    CHECK_EQ(settled.directOrigin.resultsDrained,
             static_cast<uint64_t>(1));
    CHECK_EQ(loader.workCount().pending, static_cast<size_t>(0));
    CHECK_EQ(loader.workCount().inFlight, static_cast<size_t>(0));
}

TEST_CASE(AsyncChunkLoader_ResubmissionPrecedesObservableStart) {
    WorldResources resources;
    World world;
    world.initialize(resources);
    auto generator = makeGenerator(resources.registry());
    world.setGenerator(generator);

    MemoryContext ctx;
    auto poolBlocker = std::make_shared<LoaderWorkGate>();
    auto barrierGate = std::make_shared<LoaderWorkGate>();
    auto resubmissionStartGate = std::make_shared<LoaderWorkGate>();
    auto unusedPayloadGate = std::make_shared<LoaderWorkGate>();
    std::atomic<bool> submissionCommitEntered{false};
    std::atomic<bool> submissionCommitReleased{false};
    std::atomic<bool> enqueueReturnEntered{false};
    std::atomic<bool> enqueueReturnReleased{false};
    std::atomic<bool> diagnosticReadEntered{false};
    std::atomic<bool> diagnosticReadReleased{false};
    Rigel::Voxel::RegionSchedulerOriginDiagnostics readBoundary;
    std::vector<ChunkLoadCompletion> resolved;
    AsyncChunkLoader loader(
        ctx.service,
        ctx.context,
        world,
        generator->config().world.version,
        1,
        0,
        0,
        generator);
    std::jthread diagnosticReader;
    std::jthread drainer;
    LoaderWorkRelease releasePoolOnExit(poolBlocker, barrierGate);
    LoaderWorkRelease releaseResubmissionOnExit(
        resubmissionStartGate, unusedPayloadGate);
    AtomicFlagRelease releaseEnqueueOnExit(enqueueReturnReleased);
    AtomicFlagRelease releaseSubmissionCommitOnExit(
        submissionCommitReleased);
    AtomicFlagRelease releaseDiagnosticReadOnExit(diagnosticReadReleased);
    loader.setMaxInFlightRegions(1);
    loader.setPrefetchRadius(0);
    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
        setRegionLoadStartObserver(
            loader,
            [resubmissionStartGate](const RegionKey&, bool direct) {
                if (!direct) {
                    resubmissionStartGate->enterAndWait();
                }
            });
    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::enqueueIoPoolJob(
        loader, [poolBlocker]() { poolBlocker->enterAndWait(); });
    CHECK(poolBlocker->waitUntilEntered());

    const RegionKey speculativeKey{"rigel:default", 1, 0, 0};
    const bool speculativeQueued = Rigel::Persistence::detail::
        AsyncChunkLoaderTestAccess::queueSpeculativeRegionLoad(
            loader, speculativeKey);
    const ChunkLoadRequest directRequest = makeLoadRequest({160, 0, 0});
    const ChunkLoadRequestResult directRequestResult =
        loader.request(directRequest);
    const auto yielded = loader.metrics();
    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::enqueueIoPoolJob(
        loader, [barrierGate]() { barrierGate->enterAndWait(); });
    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
        gateNextIoPoolEnqueueReturn(
            loader, enqueueReturnEntered, enqueueReturnReleased);
    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
        gateNextIoPoolSubmissionCommit(
            loader, submissionCommitEntered, submissionCommitReleased);

    poolBlocker->release();
    CHECK(barrierGate->waitUntilEntered());
    diagnosticReader = std::jthread([&]() {
        readBoundary = Rigel::Persistence::detail::
            AsyncChunkLoaderTestAccess::speculativeRegionBoundaryMetrics(
                loader,
                &diagnosticReadEntered,
                &diagnosticReadReleased);
    });
    CHECK(waitUntilTrue(diagnosticReadEntered));
    barrierGate->release();
    drainer = std::jthread([&]() {
        resolved = loader.drainCompletions(1);
    });

    CHECK(waitUntilTrue(submissionCommitEntered));
    const auto firstPublication = Rigel::Persistence::detail::
        AsyncChunkLoaderTestAccess::speculativeRegionBoundaryMetrics(loader);
    CHECK_EQ(firstPublication.poolResubmissions, static_cast<uint64_t>(0));
    CHECK_EQ(firstPublication.poolSubmissions, static_cast<uint64_t>(2));
    CHECK_EQ(firstPublication.poolWorkerStarts, static_cast<uint64_t>(0));
    CHECK_EQ(firstPublication.resultsPublished, static_cast<uint64_t>(0));

    releaseSubmissionCommitOnExit.release();
    CHECK(waitUntilTrue(enqueueReturnEntered));
    CHECK(resubmissionStartGate->waitUntilEntered());
    releaseDiagnosticReadOnExit.release();
    diagnosticReader.join();
    const auto boundary = Rigel::Persistence::detail::
        AsyncChunkLoaderTestAccess::speculativeRegionBoundaryMetrics(loader);

    resubmissionStartGate->release();
    releaseEnqueueOnExit.release();
    drainer.join();
    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::stopIoPool(loader);
    auto speculativeResolved = loader.drainCompletions(1);
    resolved.insert(
        resolved.end(), speculativeResolved.begin(), speculativeResolved.end());
    const auto settled = loader.metrics();

    CHECK(speculativeQueued);
    CHECK_EQ(directRequestResult, ChunkLoadRequestResult::Queued);
    CHECK_EQ(yielded.speculativeOrigin.poolSubmissions,
             static_cast<uint64_t>(1));
    CHECK_EQ(yielded.speculativeOrigin.successfulPoolYields,
             static_cast<uint64_t>(1));
    CHECK_EQ(yielded.speculativeOrigin.poolResubmissions,
             static_cast<uint64_t>(0));
    CHECK_EQ(readBoundary.poolSubmissions, static_cast<uint64_t>(2));
    CHECK_EQ(readBoundary.poolResubmissions, static_cast<uint64_t>(0));
    CHECK_EQ(boundary.poolSubmissions, static_cast<uint64_t>(2));
    CHECK_EQ(boundary.poolResubmissions, static_cast<uint64_t>(1));
    CHECK_EQ(boundary.poolWorkerStarts, static_cast<uint64_t>(1));
    CHECK_EQ(boundary.resultsPublished, static_cast<uint64_t>(0));
    CHECK(boundary.poolResubmissions <= boundary.poolSubmissions);
    CHECK(boundary.resultsPublished <= boundary.poolWorkerStarts);
    CHECK(boundary.poolWorkerStarts <= boundary.poolSubmissions);
    CHECK_EQ(resolved.size(), static_cast<size_t>(1));
    CHECK_EQ(resolved.front().outcome, ChunkLoadOutcome::Missing);
    CHECK_EQ(settled.directOrigin.logicalAdmissions,
             settled.directOrigin.resultsPublished);
    CHECK_EQ(settled.directOrigin.poolSubmissions,
             settled.directOrigin.poolWorkerStarts);
    CHECK_EQ(settled.speculativeOrigin.logicalAdmissions,
             settled.speculativeOrigin.resultsPublished);
    CHECK_EQ(settled.speculativeOrigin.poolSubmissions,
             settled.speculativeOrigin.poolWorkerStarts +
                 settled.speculativeOrigin.successfulPoolYields);
    CHECK_EQ(settled.speculativeOrigin.poolResubmissions,
             static_cast<uint64_t>(1));
    CHECK_EQ(settled.speculativeOrigin.resultsPublished,
             settled.speculativeOrigin.resultsDrained);
    CHECK_EQ(settled.speculativePoolJobsPending, static_cast<size_t>(0));
    CHECK_EQ(loader.workCount().pending, static_cast<size_t>(0));
    CHECK_EQ(loader.workCount().inFlight, static_cast<size_t>(0));
}

TEST_CASE(AsyncChunkLoader_PromotionRetainsOriginTimingAndConsumesNoPrefetchHit) {
    WorldResources resources;
    World world;
    world.initialize(resources);
    auto generator = makeGenerator(resources.registry());
    world.setGenerator(generator);

    MemoryContext ctx;
    auto poolGate = std::make_shared<LoaderWorkGate>();
    auto unusedPayloadGate = std::make_shared<LoaderWorkGate>();
    const std::array<int64_t, 3> clockNanoseconds{10, 30, 45};
    std::atomic<size_t> clockIndex{0};
    AsyncChunkLoader loader(
        ctx.service,
        ctx.context,
        world,
        generator->config().world.version,
        1,
        0,
        1,
        generator);
    LoaderWorkRelease releaseOnExit(poolGate, unusedPayloadGate);
    loader.setMaxInFlightRegions(1);
    loader.setPrefetchRadius(0);
    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::enqueueIoPoolJob(
        loader, [poolGate]() { poolGate->enterAndWait(); });
    CHECK(poolGate->waitUntilEntered());

    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::setMetricClock(
        loader,
        [&clockNanoseconds, &clockIndex]() {
            const size_t index =
                clockIndex.fetch_add(1, std::memory_order_relaxed);
            const int64_t value = index < clockNanoseconds.size()
                ? clockNanoseconds[index]
                : clockNanoseconds.back();
            return std::chrono::steady_clock::time_point{
                std::chrono::nanoseconds(value)};
        });

    const RegionKey key{"rigel:default", 1, 0, 0};
    const ChunkCoord demand{16, 0, 0};
    CHECK(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
              queueSpeculativeRegionLoad(loader, key));
    const auto identity = Rigel::Persistence::detail::
        AsyncChunkLoaderTestAccess::regionJobIdentity(loader, key);
    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
        markRegionKnownMissing(loader, key);
    CHECK_EQ(loader.request(makeLoadRequest(demand)),
             ChunkLoadRequestResult::Queued);
    CHECK_EQ(loader.metrics().demandPromotions, static_cast<uint64_t>(1));
    CHECK_EQ(loader.metrics().directOrigin.logicalAdmissions,
             static_cast<uint64_t>(0));
    CHECK_EQ(loader.metrics().speculativeOrigin.logicalAdmissions,
             static_cast<uint64_t>(1));
    CHECK_EQ(loader.metrics().demandOwnedDispatchedUndrained,
             static_cast<size_t>(1));
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionJobIdentity(loader, key),
             identity);

    poolGate->release();
    CHECK(waitForPublishedRegionJobs(loader, 1));
    const auto resolved = loader.drainCompletions(1);
    CHECK_EQ(resolved.size(), static_cast<size_t>(1));
    CHECK_EQ(clockIndex.load(std::memory_order_relaxed),
             clockNanoseconds.size());
    const auto settled = loader.metrics();
    CHECK_EQ(settled.directOrigin.logicalAdmissions,
             static_cast<uint64_t>(0));
    CHECK_EQ(settled.speculativeOrigin.poolSubmissions,
             static_cast<uint64_t>(1));
    CHECK_EQ(settled.speculativeOrigin.poolWorkerStarts,
             static_cast<uint64_t>(1));
    CHECK_EQ(settled.speculativeOrigin.resultsPublished,
             static_cast<uint64_t>(1));
    CHECK_EQ(settled.speculativeOrigin.resultsDrained,
             static_cast<uint64_t>(1));
    CHECK_EQ(settled.speculativeOrigin.admissionToWorkerStartNanoseconds,
             static_cast<uint64_t>(20));
    CHECK_EQ(settled.speculativeOrigin.workerExecutionNanoseconds,
             static_cast<uint64_t>(15));
    CHECK_EQ(settled.usefulPrefetchCacheHits, static_cast<uint64_t>(0));
    CHECK_EQ(settled.demandOwnedDispatchedUndrained,
             static_cast<size_t>(0));
    CHECK_EQ(settled.speculativeOwnedDispatchedUndrained,
             static_cast<size_t>(0));
}

TEST_CASE(AsyncChunkLoader_PrefetchMetricsTrackCacheUseAndUnusedEviction) {
    WorldResources resources;
    World world;
    world.initialize(resources);
    auto generator = makeGenerator(resources.registry());
    world.setGenerator(generator);

    MemoryContext cacheHitContext;
    AsyncChunkLoader cacheHitLoader(
        cacheHitContext.service,
        cacheHitContext.context,
        world,
        generator->config().world.version,
        0,
        0,
        1,
        generator);
    cacheHitLoader.setMaxCachedRegions(2);
    cacheHitLoader.setMaxInFlightRegions(16);
    cacheHitLoader.setPrefetchRadius(1);
    cacheHitLoader.setPrefetchPerRequest(1);

    std::optional<RegionKey> prefetchedKey;
    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
        setRegionLoadStartObserver(
            cacheHitLoader,
            [&prefetchedKey](const RegionKey& key, bool direct) {
                if (!direct) {
                    prefetchedKey = key;
                }
            });

    CHECK_EQ(cacheHitLoader.request(makeLoadRequest({0, 0, 0})),
             ChunkLoadRequestResult::Queued);
    std::vector<ChunkLoadCompletion> cacheHitResolved;
    CHECK(drainRegionJobsUntilSettled(cacheHitLoader, cacheHitResolved));
    CHECK(prefetchedKey.has_value());
    if (!prefetchedKey) {
        return;
    }
    const ChunkCoord prefetchedCoord{
        prefetchedKey->x * 16,
        prefetchedKey->y * 16,
        prefetchedKey->z * 16};
    const RegionLifecycleSnapshot beforeCachedDemand =
        regionLifecycleSnapshot(cacheHitLoader.metrics());
    CHECK_EQ(beforeCachedDemand.directAdmissions, static_cast<uint64_t>(1));
    CHECK_EQ(beforeCachedDemand.directPoolSubmissions,
             static_cast<uint64_t>(0));
    CHECK_EQ(beforeCachedDemand.directPublications,
             static_cast<uint64_t>(1));
    CHECK_EQ(beforeCachedDemand.directDrains, static_cast<uint64_t>(1));
    CHECK_EQ(beforeCachedDemand.speculativeAdmissions,
             static_cast<uint64_t>(1));
    CHECK_EQ(beforeCachedDemand.speculativePoolSubmissions,
             static_cast<uint64_t>(0));
    CHECK_EQ(beforeCachedDemand.speculativePublications,
             static_cast<uint64_t>(1));
    CHECK_EQ(beforeCachedDemand.speculativeDrains,
             static_cast<uint64_t>(1));

    CHECK_EQ(cacheHitLoader.request(makeLoadRequest(prefetchedCoord)),
             ChunkLoadRequestResult::Missing);
    CHECK_EQ(cacheHitLoader.metrics().usefulPrefetchCacheHits,
             static_cast<uint64_t>(1));
    CHECK_EQ(cacheHitLoader.metrics().demandPromotions,
             static_cast<uint64_t>(0));
    CHECK_EQ(regionLifecycleSnapshot(cacheHitLoader.metrics()),
             beforeCachedDemand);

    CHECK_EQ(cacheHitLoader.request(makeLoadRequest(prefetchedCoord)),
             ChunkLoadRequestResult::Missing);
    const auto repeatedCacheUse = cacheHitLoader.metrics();
    CHECK_EQ(repeatedCacheUse.usefulPrefetchCacheHits,
             static_cast<uint64_t>(1));
    CHECK_EQ(regionLifecycleSnapshot(repeatedCacheUse), beforeCachedDemand);
    CHECK_EQ(repeatedCacheUse.demandOwnedQueued, static_cast<size_t>(0));
    CHECK_EQ(repeatedCacheUse.speculativeOwnedQueued, static_cast<size_t>(0));
    CHECK_EQ(repeatedCacheUse.demandOwnedDispatchedUndrained,
             static_cast<size_t>(0));
    CHECK_EQ(repeatedCacheUse.speculativeOwnedDispatchedUndrained,
             static_cast<size_t>(0));

    World evictionWorld;
    evictionWorld.initialize(resources);
    evictionWorld.setGenerator(generator);
    MemoryContext evictionContext;
    AsyncChunkLoader evictionLoader(
        evictionContext.service,
        evictionContext.context,
        evictionWorld,
        generator->config().world.version,
        0,
        0,
        1,
        generator);
    evictionLoader.setMaxCachedRegions(1);
    evictionLoader.setMaxInFlightRegions(16);
    evictionLoader.setPrefetchRadius(1);
    evictionLoader.setPrefetchPerRequest(1);

    CHECK_EQ(evictionLoader.request(makeLoadRequest({0, 0, 0})),
             ChunkLoadRequestResult::Queued);
    std::vector<ChunkLoadCompletion> evictionResolved;
    CHECK(drainRegionJobsUntilSettled(evictionLoader, evictionResolved));
    CHECK_EQ(evictionLoader.request(makeLoadRequest({160, 0, 0})),
             ChunkLoadRequestResult::Queued);
    CHECK(drainRegionJobsUntilSettled(evictionLoader, evictionResolved));

    const auto evictionMetrics = evictionLoader.metrics();
    CHECK_EQ(evictionMetrics.usefulPrefetchCacheHits,
             static_cast<uint64_t>(0));
    CHECK_EQ(evictionMetrics.speculativeEvictionsBeforeDemand,
             static_cast<uint64_t>(1));
    CHECK_EQ(evictionLoader.workCount().pending, static_cast<size_t>(0));
    CHECK_EQ(evictionLoader.workCount().inFlight, static_cast<size_t>(0));
}

TEST_CASE(AsyncChunkLoader_LaterDirectRegionOutranksUnstartedPrefetch) {
    WorldResources resources;
    World world;
    world.initialize(resources);
    auto generator = makeGenerator(resources.registry());
    world.setGenerator(generator);

    MemoryContext ctx;
    auto firstPoolBlocker = std::make_shared<LoaderWorkGate>();
    auto secondPoolBlocker = std::make_shared<LoaderWorkGate>();
    std::mutex startsMutex;
    std::vector<std::pair<RegionKey, bool>> starts;
    AsyncChunkLoader loader(
        ctx.service,
        ctx.context,
        world,
        generator->config().world.version,
        2,
        0,
        12,
        generator);
    LoaderWorkRelease releaseOnExit(firstPoolBlocker, secondPoolBlocker);
    loader.setMaxInFlightRegions(16);
    loader.setPrefetchRadius(1);
    loader.setPrefetchPerRequest(12);

    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::enqueueIoPoolJob(
        loader,
        [firstPoolBlocker]() { firstPoolBlocker->enterAndWait(); });
    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::enqueueIoPoolJob(
        loader,
        [secondPoolBlocker]() { secondPoolBlocker->enterAndWait(); });
    CHECK(firstPoolBlocker->waitUntilEntered());
    CHECK(secondPoolBlocker->waitUntilEntered());

    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
        setRegionLoadStartObserver(
            loader,
            [&startsMutex, &starts](
                const RegionKey& key,
                bool direct) {
                std::lock_guard<std::mutex> lock(startsMutex);
                starts.emplace_back(key, direct);
            });

    CHECK_EQ(loader.request(makeLoadRequest({0, 0, 0})),
             ChunkLoadRequestResult::Queued);
    CHECK_EQ(loader.metrics().directOrigin.logicalAdmissions, static_cast<uint64_t>(1));
    CHECK_EQ(loader.metrics().speculativeOrigin.logicalAdmissions,
             static_cast<uint64_t>(12));
    CHECK_EQ(loader.metrics().speculativePoolJobsPending,
             static_cast<size_t>(1));
    const auto submittedSpeculative = Rigel::Persistence::detail::
        AsyncChunkLoaderTestAccess::submittedSpeculativeRegionKeys(loader);
    CHECK_EQ(submittedSpeculative.size(), static_cast<size_t>(1));
    const RegionKey displaced = submittedSpeculative.front();
    const auto displacedIdentity = Rigel::Persistence::detail::
        AsyncChunkLoaderTestAccess::regionJobIdentity(loader, displaced);
    const auto displacedPoolJob = Rigel::Persistence::detail::
        AsyncChunkLoaderTestAccess::regionPoolJobId(loader, displaced);
    CHECK(displacedIdentity);
    CHECK(displacedPoolJob != 0);
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionLoadAttemptCount(loader, displaced),
             static_cast<size_t>(1));

    const ChunkCoord laterDirect{160, 0, 0};
    CHECK_EQ(loader.request(makeLoadRequest(laterDirect)),
             ChunkLoadRequestResult::Queued);
    CHECK_EQ(loader.metrics().directOrigin.logicalAdmissions, static_cast<uint64_t>(2));
    CHECK_EQ(loader.metrics().demandOwnedDispatchedUndrained, static_cast<size_t>(2));
    CHECK_EQ(loader.metrics().demandOwnedQueued, static_cast<size_t>(0));
    CHECK_EQ(loader.metrics().speculativeOwnedDispatchedUndrained,
             static_cast<size_t>(0));
    CHECK_EQ(loader.metrics().speculativeOwnedQueued,
             static_cast<size_t>(14));
    CHECK_EQ(loader.metrics().speculativePoolJobsPending,
             static_cast<size_t>(0));
    CHECK_EQ(loader.metrics().speculativePoolYieldCandidateVisits,
             static_cast<uint64_t>(1));
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionJobIdentity(loader, displaced),
             displacedIdentity);
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionPoolJobId(loader, displaced),
             Rigel::Voxel::detail::ThreadPool::JobId{0});
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionLoadAttemptCount(loader, displaced),
             static_cast<size_t>(0));
    CHECK(!Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
              regionJobHasDirectOrigin(loader, displaced));
    CHECK(!Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
              regionJobHasDirectDemand(loader, displaced));

    firstPoolBlocker->release();
    secondPoolBlocker->release();
    CHECK(waitForPublishedRegionJobs(loader, 2));
    auto resolved = loader.drainCompletions(2);
    CHECK_EQ(resolved.size(), static_cast<size_t>(2));
    const auto resubmittedPoolJob = Rigel::Persistence::detail::
        AsyncChunkLoaderTestAccess::regionPoolJobId(loader, displaced);
    CHECK(resubmittedPoolJob != 0);
    CHECK_NE(resubmittedPoolJob, displacedPoolJob);
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionJobIdentity(loader, displaced),
             displacedIdentity);
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionLoadAttemptCount(loader, displaced),
             static_cast<size_t>(1));
    CHECK(waitForPublishedRegionJobs(loader, 1));
    {
        std::lock_guard<std::mutex> lock(startsMutex);
        auto laterStart = std::find_if(
            starts.begin(), starts.end(),
            [](const auto& start) { return start.first.x == 10; });
        auto displacedStart = std::find_if(
            starts.begin(), starts.end(),
            [&displaced](const auto& start) {
                return start.first == displaced;
            });
        CHECK(laterStart != starts.end());
        CHECK(displacedStart != starts.end());
        CHECK(laterStart < displacedStart);
        CHECK(laterStart->second);
        CHECK(!displacedStart->second);
    }

    CHECK(drainRegionJobsUntilSettled(loader, resolved));
    CHECK_EQ(resolved.size(), static_cast<size_t>(2));
    const auto settled = loader.metrics();
    CHECK_EQ(settled.directOrigin.logicalAdmissions, settled.directOrigin.resultsPublished);
    CHECK_EQ(settled.speculativeOrigin.logicalAdmissions, settled.speculativeOrigin.resultsPublished);
    CHECK_EQ(loader.workCount().pending, static_cast<size_t>(0));
    CHECK_EQ(loader.workCount().inFlight, static_cast<size_t>(0));
}

TEST_CASE(AsyncChunkLoader_DirectDemandDisplacesBoundedQueuedPrefetch) {
    WorldResources resources;
    World world;
    world.initialize(resources);
    auto generator = makeGenerator(resources.registry());
    world.setGenerator(generator);

    MemoryContext ctx;
    auto firstStartGate = std::make_shared<LoaderWorkGate>();
    auto unusedPayloadGate = std::make_shared<LoaderWorkGate>();
    AsyncChunkLoader loader(
        ctx.service,
        ctx.context,
        world,
        generator->config().world.version,
        1,
        0,
        12,
        generator);
    LoaderWorkRelease releaseOnExit(firstStartGate, unusedPayloadGate);
    loader.setMaxInFlightRegions(16);
    loader.setPrefetchRadius(1);
    loader.setPrefetchPerRequest(12);

    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
        setRegionLoadStartObserver(
            loader,
            [firstStartGate](const RegionKey& key, bool direct) {
                if (direct && key.x == 0 && key.y == 0 && key.z == 0) {
                    firstStartGate->enterAndWait();
                }
            });

    CHECK_EQ(loader.request(makeLoadRequest({0, 0, 0})),
             ChunkLoadRequestResult::Queued);
    CHECK(firstStartGate->waitUntilEntered());
    CHECK_EQ(loader.request(makeLoadRequest({160, 0, 0})),
             ChunkLoadRequestResult::Queued);
    CHECK_EQ(loader.request(makeLoadRequest({320, 0, 0})),
             ChunkLoadRequestResult::Queued);

    const auto saturated = loader.metrics();
    CHECK_EQ(saturated.directOrigin.logicalAdmissions, static_cast<uint64_t>(3));
    CHECK_EQ(saturated.speculativeOrigin.logicalAdmissions, static_cast<uint64_t>(14));
    CHECK_EQ(saturated.speculativeOrigin.logicalPreStartCancellations,
             static_cast<uint64_t>(1));
    CHECK_EQ(saturated.demandOwnedDispatchedUndrained, static_cast<size_t>(1));
    CHECK_EQ(saturated.demandOwnedQueued, static_cast<size_t>(2));
    CHECK_EQ(saturated.speculativeOwnedDispatchedUndrained,
             static_cast<size_t>(0));
    CHECK_EQ(saturated.speculativeOwnedQueued,
             static_cast<size_t>(13));
    CHECK_EQ(
        saturated.demandOwnedDispatchedUndrained +
            saturated.demandOwnedQueued +
            saturated.speculativeOwnedDispatchedUndrained +
            saturated.speculativeOwnedQueued,
        static_cast<size_t>(16));
    CHECK_EQ(loader.workCount().inFlight, static_cast<size_t>(1));

    firstStartGate->release();
    std::vector<ChunkLoadCompletion> resolved;
    CHECK(drainRegionJobsUntilSettled(loader, resolved));
    CHECK_EQ(resolved.size(), static_cast<size_t>(3));
    const auto settled = loader.metrics();
    CHECK_EQ(
        settled.directOrigin.resultsPublished +
            settled.directOrigin.logicalPreStartCancellations,
        settled.directOrigin.logicalAdmissions);
    CHECK_EQ(
        settled.speculativeOrigin.resultsPublished +
            settled.speculativeOrigin.logicalPreStartCancellations,
        settled.speculativeOrigin.logicalAdmissions);
    CHECK_EQ(loader.workCount().pending, static_cast<size_t>(0));
    CHECK_EQ(loader.workCount().inFlight, static_cast<size_t>(0));
}

TEST_CASE(AsyncChunkLoader_UnlimitedCapacityBoundsQueuedPrefetch) {
    WorldResources resources;
    World world;
    world.initialize(resources);
    auto generator = makeGenerator(resources.registry());
    world.setGenerator(generator);

    MemoryContext ctx;
    auto firstStartGate = std::make_shared<LoaderWorkGate>();
    auto unusedPayloadGate = std::make_shared<LoaderWorkGate>();
    AsyncChunkLoader loader(
        ctx.service,
        ctx.context,
        world,
        generator->config().world.version,
        1,
        0,
        12,
        generator);
    LoaderWorkRelease releaseOnExit(firstStartGate, unusedPayloadGate);
    loader.setMaxInFlightRegions(0);
    loader.setPrefetchRadius(2);
    loader.setPrefetchPerRequest(0);

    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
        setRegionLoadStartObserver(
            loader,
            [firstStartGate](const RegionKey&, bool direct) {
                if (direct) {
                    firstStartGate->enterAndWait();
                }
            });

    const ChunkCoord requested{0, 0, 0};
    CHECK_EQ(loader.request(makeLoadRequest(requested)),
             ChunkLoadRequestResult::Queued);
    CHECK(firstStartGate->waitUntilEntered());

    const auto saturated = loader.metrics();
    CHECK_EQ(saturated.directOrigin.logicalAdmissions, static_cast<uint64_t>(1));
    CHECK_EQ(saturated.speculativeOrigin.logicalAdmissions, static_cast<uint64_t>(64));
    CHECK_EQ(saturated.demandOwnedDispatchedUndrained, static_cast<size_t>(1));
    CHECK_EQ(saturated.demandOwnedQueued, static_cast<size_t>(0));
    CHECK_EQ(saturated.speculativeOwnedDispatchedUndrained,
             static_cast<size_t>(0));
    CHECK_EQ(saturated.speculativeOwnedQueued,
             static_cast<size_t>(64));
    CHECK_EQ(loader.workCount().inFlight, static_cast<size_t>(1));

    loader.cancel(requested);
    CHECK(!loader.isPending(requested));
    const auto cancelled = loader.metrics();
    CHECK_EQ(cancelled.demandOwnedQueued, static_cast<size_t>(0));
    CHECK_EQ(cancelled.speculativeOwnedQueued, static_cast<size_t>(64));
    CHECK_EQ(cancelled.demandOwnedDispatchedUndrained, static_cast<size_t>(0));
    CHECK_EQ(cancelled.speculativeOwnedDispatchedUndrained,
             static_cast<size_t>(1));
    CHECK_EQ(loader.workCount().pending, static_cast<size_t>(0));
    CHECK_EQ(loader.workCount().inFlight, static_cast<size_t>(1));
    firstStartGate->release();

    std::vector<ChunkLoadCompletion> resolved;
    CHECK(drainRegionJobsUntilSettled(loader, resolved));
    CHECK(resolved.empty());
    const auto settled = loader.metrics();
    CHECK_EQ(settled.directOrigin.resultsPublished, settled.directOrigin.logicalAdmissions);
    CHECK_EQ(settled.speculativeOrigin.resultsPublished, settled.speculativeOrigin.logicalAdmissions);
    CHECK_EQ(settled.demandOwnedQueued, static_cast<size_t>(0));
    CHECK_EQ(settled.speculativeOwnedQueued, static_cast<size_t>(0));
    CHECK_EQ(settled.demandOwnedDispatchedUndrained, static_cast<size_t>(0));
    CHECK_EQ(settled.speculativeOwnedDispatchedUndrained, static_cast<size_t>(0));
    CHECK_EQ(loader.workCount().pending, static_cast<size_t>(0));
    CHECK_EQ(loader.workCount().inFlight, static_cast<size_t>(0));
}

TEST_CASE(AsyncChunkLoader_ZeroCapYieldKeepsQueuedPrefetchBounded) {
    WorldResources resources;
    World world;
    world.initialize(resources);
    auto generator = makeGenerator(resources.registry());
    world.setGenerator(generator);

    MemoryContext ctx;
    auto firstPoolBlocker = std::make_shared<LoaderWorkGate>();
    auto secondPoolBlocker = std::make_shared<LoaderWorkGate>();
    AsyncChunkLoader loader(
        ctx.service,
        ctx.context,
        world,
        generator->config().world.version,
        2,
        0,
        12,
        generator);
    LoaderWorkRelease releaseOnExit(firstPoolBlocker, secondPoolBlocker);
    loader.setMaxInFlightRegions(0);
    loader.setPrefetchRadius(2);
    loader.setPrefetchPerRequest(0);

    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::enqueueIoPoolJob(
        loader,
        [firstPoolBlocker]() { firstPoolBlocker->enterAndWait(); });
    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::enqueueIoPoolJob(
        loader,
        [secondPoolBlocker]() { secondPoolBlocker->enterAndWait(); });
    CHECK(firstPoolBlocker->waitUntilEntered());
    CHECK(secondPoolBlocker->waitUntilEntered());

    CHECK_EQ(loader.request(makeLoadRequest({0, 0, 0})),
             ChunkLoadRequestResult::Queued);
    const auto full = loader.metrics();
    CHECK_EQ(full.directOrigin.logicalAdmissions, static_cast<uint64_t>(1));
    CHECK_EQ(full.speculativeOrigin.logicalAdmissions,
             static_cast<uint64_t>(65));
    CHECK_EQ(full.demandOwnedDispatchedUndrained, static_cast<size_t>(1));
    CHECK_EQ(full.speculativeOwnedDispatchedUndrained,
             static_cast<size_t>(1));
    CHECK_EQ(full.speculativeOwnedQueued, static_cast<size_t>(64));
    CHECK_EQ(full.speculativePoolJobsPending, static_cast<size_t>(1));

    const auto submitted = Rigel::Persistence::detail::
        AsyncChunkLoaderTestAccess::submittedSpeculativeRegionKeys(loader);
    CHECK_EQ(submitted.size(), static_cast<size_t>(1));
    const RegionKey yieldedKey = submitted.front();
    const auto yieldedIdentity = Rigel::Persistence::detail::
        AsyncChunkLoaderTestAccess::regionJobIdentity(loader, yieldedKey);
    const auto yieldedPoolJob = Rigel::Persistence::detail::
        AsyncChunkLoaderTestAccess::regionPoolJobId(loader, yieldedKey);
    CHECK(yieldedIdentity);
    CHECK(yieldedPoolJob != 0);
    const auto queuedBefore = Rigel::Persistence::detail::
        AsyncChunkLoaderTestAccess::queuedSpeculativeRegionKeys(loader);
    CHECK_EQ(queuedBefore.size(), static_cast<size_t>(64));

    CHECK_EQ(loader.request(makeLoadRequest({160, 0, 0})),
             ChunkLoadRequestResult::Queued);
    const auto yielded = loader.metrics();
    CHECK_EQ(yielded.directOrigin.logicalAdmissions, static_cast<uint64_t>(2));
    CHECK_EQ(yielded.speculativeOrigin.logicalAdmissions,
             static_cast<uint64_t>(65));
    CHECK_EQ(yielded.speculativeOrigin.logicalPreStartCancellations,
             static_cast<uint64_t>(1));
    CHECK_EQ(yielded.speculativeOrigin.successfulPoolYields,
             static_cast<uint64_t>(1));
    CHECK_EQ(yielded.demandOwnedDispatchedUndrained, static_cast<size_t>(2));
    CHECK_EQ(yielded.speculativeOwnedDispatchedUndrained,
             static_cast<size_t>(0));
    CHECK_EQ(yielded.speculativeOwnedQueued, static_cast<size_t>(64));
    CHECK_EQ(yielded.speculativePoolJobsPending, static_cast<size_t>(0));
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionJobIdentity(loader, yieldedKey),
             yieldedIdentity);
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionPoolJobId(loader, yieldedKey),
             Rigel::Voxel::detail::ThreadPool::JobId{0});

    const auto queuedAfter = Rigel::Persistence::detail::
        AsyncChunkLoaderTestAccess::queuedSpeculativeRegionKeys(loader);
    CHECK_EQ(queuedAfter.size(), static_cast<size_t>(64));
    CHECK(std::find(queuedAfter.begin(), queuedAfter.end(), yieldedKey) !=
          queuedAfter.end());
    size_t displacedCount = 0;
    for (const RegionKey& key : queuedBefore) {
        if (std::find(queuedAfter.begin(), queuedAfter.end(), key) ==
            queuedAfter.end()) {
            ++displacedCount;
            CHECK(!Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                       regionJobIdentity(loader, key));
        }
    }
    CHECK_EQ(displacedCount, static_cast<size_t>(1));

    loader.setPrefetchRadius(0);
    const ChunkCoord promotedCoord{
        yieldedKey.x * 16, yieldedKey.y * 16, yieldedKey.z * 16};
    CHECK_EQ(loader.request(makeLoadRequest(promotedCoord)),
             ChunkLoadRequestResult::Queued);
    const auto promoted = loader.metrics();
    CHECK_EQ(promoted.demandOwnedQueued, static_cast<size_t>(1));
    CHECK_EQ(promoted.speculativeOwnedQueued, static_cast<size_t>(63));
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionJobIdentity(loader, yieldedKey),
             yieldedIdentity);

    const RegionKey replacementKey{"rigel:default", 100, 100, 100};
    CHECK(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
              queueSpeculativeRegionLoad(loader, replacementKey));
    CHECK_EQ(loader.metrics().speculativeOwnedQueued,
             static_cast<size_t>(64));

    loader.cancel(promotedCoord);
    const auto cancelled = loader.metrics();
    CHECK_EQ(cancelled.speculativeOrigin.logicalAdmissions,
             static_cast<uint64_t>(66));
    CHECK_EQ(cancelled.speculativeOrigin.logicalPreStartCancellations,
             static_cast<uint64_t>(2));
    CHECK_EQ(cancelled.demandOwnedQueued, static_cast<size_t>(0));
    CHECK_EQ(cancelled.speculativeOwnedQueued, static_cast<size_t>(64));
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionJobIdentity(loader, yieldedKey),
             yieldedIdentity);

    CHECK_EQ(loader.request(makeLoadRequest(promotedCoord)),
             ChunkLoadRequestResult::Queued);
    const auto reentered = loader.metrics();
    CHECK_EQ(reentered.demandOwnedQueued, static_cast<size_t>(1));
    CHECK_EQ(reentered.speculativeOwnedQueued, static_cast<size_t>(63));
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionJobIdentity(loader, yieldedKey),
             yieldedIdentity);

    firstPoolBlocker->release();
    secondPoolBlocker->release();
    std::vector<ChunkLoadCompletion> resolved;
    CHECK(drainRegionJobsUntilSettled(loader, resolved));
    CHECK_EQ(resolved.size(), static_cast<size_t>(3));

    const auto settled = loader.metrics();
    CHECK_EQ(settled.directOrigin.resultsPublished,
             settled.directOrigin.logicalAdmissions);
    CHECK_EQ(
        settled.speculativeOrigin.resultsPublished +
            settled.speculativeOrigin.logicalPreStartCancellations,
        settled.speculativeOrigin.logicalAdmissions);
    CHECK_EQ(settled.speculativeOrigin.poolSubmissions,
             settled.speculativeOrigin.poolWorkerStarts +
                 settled.speculativeOrigin.successfulPoolYields);
    CHECK_EQ(settled.speculativeOrigin.resultsPublished,
             settled.speculativeOrigin.resultsDrained);
    CHECK_EQ(settled.demandOwnedQueued, static_cast<size_t>(0));
    CHECK_EQ(settled.speculativeOwnedQueued, static_cast<size_t>(0));
    CHECK_EQ(settled.demandOwnedDispatchedUndrained, static_cast<size_t>(0));
    CHECK_EQ(settled.speculativeOwnedDispatchedUndrained,
             static_cast<size_t>(0));
    CHECK_EQ(settled.speculativePoolJobsPending, static_cast<size_t>(0));
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionOwnerCount(loader),
             static_cast<size_t>(0));
    CHECK_EQ(loader.workCount().pending, static_cast<size_t>(0));
    CHECK_EQ(loader.workCount().inFlight, static_cast<size_t>(0));
}

TEST_CASE(AsyncChunkLoader_ZeroCapCancelsPoolPendingPromotionIntoFullQueue) {
    WorldResources resources;
    World world;
    world.initialize(resources);
    auto generator = makeGenerator(resources.registry());
    world.setGenerator(generator);

    MemoryContext ctx;
    auto firstPoolBlocker = std::make_shared<LoaderWorkGate>();
    auto secondPoolBlocker = std::make_shared<LoaderWorkGate>();
    std::mutex startsMutex;
    std::vector<RegionKey> starts;
    AsyncChunkLoader loader(
        ctx.service,
        ctx.context,
        world,
        generator->config().world.version,
        2,
        0,
        12,
        generator);
    LoaderWorkRelease releaseOnExit(firstPoolBlocker, secondPoolBlocker);
    loader.setMaxInFlightRegions(0);
    loader.setPrefetchRadius(2);
    loader.setPrefetchPerRequest(0);

    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::enqueueIoPoolJob(
        loader,
        [firstPoolBlocker]() { firstPoolBlocker->enterAndWait(); });
    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::enqueueIoPoolJob(
        loader,
        [secondPoolBlocker]() { secondPoolBlocker->enterAndWait(); });
    CHECK(firstPoolBlocker->waitUntilEntered());
    CHECK(secondPoolBlocker->waitUntilEntered());
    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
        setRegionLoadStartObserver(
            loader,
            [&startsMutex, &starts](const RegionKey& key, bool) {
                std::lock_guard<std::mutex> lock(startsMutex);
                starts.push_back(key);
            });

    const ChunkCoord initialDemand{0, 0, 0};
    CHECK_EQ(loader.request(makeLoadRequest(initialDemand)),
             ChunkLoadRequestResult::Queued);
    const auto full = loader.metrics();
    CHECK_EQ(full.directOrigin.logicalAdmissions, static_cast<uint64_t>(1));
    CHECK_EQ(full.speculativeOrigin.logicalAdmissions,
             static_cast<uint64_t>(65));
    CHECK_EQ(full.demandOwnedDispatchedUndrained, static_cast<size_t>(1));
    CHECK_EQ(full.speculativeOwnedDispatchedUndrained,
             static_cast<size_t>(1));
    CHECK_EQ(full.speculativeOwnedQueued, static_cast<size_t>(64));
    CHECK_EQ(full.speculativePoolJobsPending, static_cast<size_t>(1));

    const auto submitted = Rigel::Persistence::detail::
        AsyncChunkLoaderTestAccess::submittedSpeculativeRegionKeys(loader);
    CHECK_EQ(submitted.size(), static_cast<size_t>(1));
    const RegionKey promotedKey = submitted.front();
    const ChunkCoord promotedDemand{
        promotedKey.x * 16, promotedKey.y * 16, promotedKey.z * 16};
    const auto promotedIdentity = Rigel::Persistence::detail::
        AsyncChunkLoaderTestAccess::regionJobIdentity(loader, promotedKey);
    const auto submittedPoolJob = Rigel::Persistence::detail::
        AsyncChunkLoaderTestAccess::regionPoolJobId(loader, promotedKey);
    CHECK(promotedIdentity);
    CHECK(submittedPoolJob != 0);
    const auto queuedBefore = Rigel::Persistence::detail::
        AsyncChunkLoaderTestAccess::queuedSpeculativeRegionKeys(loader);
    CHECK_EQ(queuedBefore.size(), static_cast<size_t>(64));

    loader.setPrefetchRadius(0);
    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
        markRegionKnownMissing(loader, promotedKey);
    CHECK_EQ(loader.request(makeLoadRequest(promotedDemand)),
             ChunkLoadRequestResult::Queued);
    const auto promoted = loader.metrics();
    CHECK_EQ(promoted.demandPromotions, static_cast<uint64_t>(1));
    CHECK_EQ(promoted.demandOwnedDispatchedUndrained,
             static_cast<size_t>(2));
    CHECK_EQ(promoted.speculativeOwnedDispatchedUndrained,
             static_cast<size_t>(0));
    CHECK_EQ(promoted.speculativeOwnedQueued, static_cast<size_t>(64));
    CHECK_EQ(promoted.speculativePoolJobsPending, static_cast<size_t>(0));
    CHECK_EQ(promoted.speculativeOrigin.successfulPoolYields,
             static_cast<uint64_t>(0));
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionJobIdentity(loader, promotedKey),
             promotedIdentity);
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionPoolJobId(loader, promotedKey),
             submittedPoolJob);
    CHECK(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
              regionJobHasDirectDemand(loader, promotedKey));

    const ChunkCoord replacementDemand{320, 0, 0};
    CHECK_EQ(loader.request(makeLoadRequest(replacementDemand)),
             ChunkLoadRequestResult::Queued);
    CHECK_EQ(loader.metrics().demandOwnedQueued, static_cast<size_t>(1));
    CHECK_EQ(loader.metrics().speculativeOwnedQueued, static_cast<size_t>(64));

    loader.cancel(promotedDemand);
    CHECK(!loader.isPending(promotedDemand));
    const auto cancelled = loader.metrics();
    CHECK_EQ(cancelled.speculativeOrigin.logicalAdmissions,
             static_cast<uint64_t>(65));
    CHECK_EQ(cancelled.speculativeOrigin.logicalPreStartCancellations,
             static_cast<uint64_t>(1));
    CHECK_EQ(cancelled.speculativeOrigin.successfulPoolYields,
             static_cast<uint64_t>(1));
    CHECK_EQ(cancelled.demandOwnedQueued, static_cast<size_t>(0));
    CHECK_EQ(cancelled.demandOwnedDispatchedUndrained,
             static_cast<size_t>(2));
    CHECK_EQ(cancelled.speculativeOwnedQueued, static_cast<size_t>(64));
    CHECK_EQ(cancelled.speculativeOwnedDispatchedUndrained,
             static_cast<size_t>(0));
    CHECK_EQ(cancelled.speculativePoolJobsPending, static_cast<size_t>(0));
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionJobIdentity(loader, promotedKey),
             promotedIdentity);
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionPoolJobId(loader, promotedKey),
             Rigel::Voxel::detail::ThreadPool::JobId{0});
    CHECK(!Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
               regionJobHasDirectDemand(loader, promotedKey));
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionLoadAttemptCount(loader, promotedKey),
             static_cast<size_t>(0));

    const auto queuedAfter = Rigel::Persistence::detail::
        AsyncChunkLoaderTestAccess::queuedSpeculativeRegionKeys(loader);
    CHECK_EQ(queuedAfter.size(), static_cast<size_t>(64));
    CHECK(std::find(queuedAfter.begin(), queuedAfter.end(), promotedKey) !=
          queuedAfter.end());
    size_t displacedCount = 0;
    for (const RegionKey& key : queuedBefore) {
        if (std::find(queuedAfter.begin(), queuedAfter.end(), key) ==
            queuedAfter.end()) {
            ++displacedCount;
            CHECK(!Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                       regionJobIdentity(loader, key));
        }
    }
    CHECK_EQ(displacedCount, static_cast<size_t>(1));

    firstPoolBlocker->release();
    secondPoolBlocker->release();
    std::vector<ChunkLoadCompletion> resolved;
    CHECK(drainRegionJobsUntilSettled(loader, resolved));
    CHECK_EQ(resolved.size(), static_cast<size_t>(2));
    {
        std::lock_guard<std::mutex> lock(startsMutex);
        CHECK_EQ(std::count(starts.begin(), starts.end(), promotedKey),
                 static_cast<std::ptrdiff_t>(1));
    }

    const auto settled = loader.metrics();
    CHECK_EQ(settled.directOrigin.resultsPublished,
             settled.directOrigin.logicalAdmissions);
    CHECK_EQ(settled.directOrigin.resultsPublished,
             settled.directOrigin.resultsDrained);
    CHECK_EQ(
        settled.speculativeOrigin.resultsPublished +
            settled.speculativeOrigin.logicalPreStartCancellations,
        settled.speculativeOrigin.logicalAdmissions);
    CHECK_EQ(settled.speculativeOrigin.poolSubmissions,
             settled.speculativeOrigin.poolWorkerStarts +
                 settled.speculativeOrigin.successfulPoolYields);
    CHECK_EQ(settled.speculativeOrigin.successfulPoolYields,
             static_cast<uint64_t>(1));
    CHECK_EQ(settled.speculativeOrigin.resultsPublished,
             settled.speculativeOrigin.resultsDrained);
    CHECK_EQ(settled.demandOwnedQueued, static_cast<size_t>(0));
    CHECK_EQ(settled.speculativeOwnedQueued, static_cast<size_t>(0));
    CHECK_EQ(settled.demandOwnedDispatchedUndrained, static_cast<size_t>(0));
    CHECK_EQ(settled.speculativeOwnedDispatchedUndrained,
             static_cast<size_t>(0));
    CHECK_EQ(settled.speculativePoolJobsPending, static_cast<size_t>(0));
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionOwnerCount(loader),
             static_cast<size_t>(0));
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionAttemptOwnerCount(loader),
             static_cast<size_t>(0));
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionCompletionCount(loader),
             static_cast<size_t>(0));
    CHECK_EQ(loader.workCount().pending, static_cast<size_t>(0));
    CHECK_EQ(loader.workCount().inFlight, static_cast<size_t>(0));
}

TEST_CASE(AsyncChunkLoader_CancelRemovesPoolPendingDirectRegionJob) {
    WorldResources resources;
    World world;
    world.initialize(resources);
    auto generator = makeGenerator(resources.registry());
    world.setGenerator(generator);

    MemoryContext ctx;
    auto firstPoolBlocker = std::make_shared<LoaderWorkGate>();
    auto secondPoolBlocker = std::make_shared<LoaderWorkGate>();
    AsyncChunkLoader loader(
        ctx.service,
        ctx.context,
        world,
        generator->config().world.version,
        2,
        0,
        12,
        generator);
    LoaderWorkRelease releaseOnExit(firstPoolBlocker, secondPoolBlocker);
    loader.setMaxInFlightRegions(16);
    loader.setPrefetchRadius(0);

    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::enqueueIoPoolJob(
        loader,
        [firstPoolBlocker]() { firstPoolBlocker->enterAndWait(); });
    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::enqueueIoPoolJob(
        loader,
        [secondPoolBlocker]() { secondPoolBlocker->enterAndWait(); });
    CHECK(firstPoolBlocker->waitUntilEntered());
    CHECK(secondPoolBlocker->waitUntilEntered());

    const ChunkCoord active{0, 0, 0};
    const ChunkCoord cancelled{160, 0, 0};
    CHECK_EQ(loader.request(makeLoadRequest(active)),
             ChunkLoadRequestResult::Queued);
    CHECK_EQ(loader.request(makeLoadRequest(cancelled)),
             ChunkLoadRequestResult::Queued);
    CHECK_EQ(loader.metrics().demandOwnedDispatchedUndrained, static_cast<size_t>(2));
    const RegionKey cancelledKey{"rigel:default", 10, 0, 0};
    const auto cancelledPoolJob = Rigel::Persistence::detail::
        AsyncChunkLoaderTestAccess::regionPoolJobId(loader, cancelledKey);
    CHECK(cancelledPoolJob != 0);
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionLoadAttemptCount(loader, cancelledKey),
             static_cast<size_t>(1));

    loader.cancel(cancelled);
    CHECK(!loader.isPending(cancelled));
    CHECK_EQ(loader.metrics().directOrigin.logicalPreStartCancellations,
             static_cast<uint64_t>(1));
    CHECK_EQ(loader.metrics().demandOwnedQueued, static_cast<size_t>(0));
    CHECK_EQ(loader.metrics().demandOwnedDispatchedUndrained, static_cast<size_t>(1));
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionJobIdentity(loader, cancelledKey),
             std::shared_ptr<const void>{});
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionLoadAttemptCount(loader, cancelledKey),
             static_cast<size_t>(0));
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionCacheCount(loader),
             static_cast<size_t>(0));
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionCompletionCount(loader),
             static_cast<size_t>(0));

    firstPoolBlocker->release();
    secondPoolBlocker->release();
    std::vector<ChunkLoadCompletion> resolved;
    CHECK(drainRegionJobsUntilSettled(loader, resolved));
    CHECK_EQ(resolved.size(), static_cast<size_t>(1));
    CHECK_EQ(resolved.front().coord, active);
    CHECK_EQ(loader.metrics().directOrigin.logicalAdmissions, static_cast<uint64_t>(2));
    CHECK_EQ(loader.metrics().directOrigin.resultsPublished, static_cast<uint64_t>(1));

    CHECK_EQ(loader.request(makeLoadRequest(cancelled)),
             ChunkLoadRequestResult::Queued);
    CHECK(drainRegionJobsUntilSettled(loader, resolved));
    CHECK_EQ(resolved.size(), static_cast<size_t>(2));
    CHECK_EQ(resolved.back().coord, cancelled);
    CHECK_EQ(loader.metrics().directOrigin.logicalAdmissions, static_cast<uint64_t>(3));
    CHECK_EQ(loader.metrics().directOrigin.resultsPublished, static_cast<uint64_t>(2));
    CHECK_EQ(loader.metrics().directOrigin.logicalPreStartCancellations,
             static_cast<uint64_t>(1));
    CHECK_EQ(loader.workCount().pending, static_cast<size_t>(0));
    CHECK_EQ(loader.workCount().inFlight, static_cast<size_t>(0));
}

TEST_CASE(AsyncChunkLoader_RunningDirectRegionOwnerSurvivesDemandChurn) {
    WorldResources resources;
    World world;
    world.initialize(resources);
    auto generator = makeGenerator(resources.registry());
    world.setGenerator(generator);

    MemoryContext ctx;
    auto runningGate = std::make_shared<LoaderWorkGate>();
    auto unusedPayloadGate = std::make_shared<LoaderWorkGate>();
    std::atomic<size_t> starts{0};
    AsyncChunkLoader loader(
        ctx.service,
        ctx.context,
        world,
        generator->config().world.version,
        1,
        0,
        0,
        generator);
    LoaderWorkRelease releaseOnExit(runningGate, unusedPayloadGate);
    loader.setPrefetchRadius(0);
    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
        setRegionLoadStartObserver(
            loader,
            [runningGate, &starts](const RegionKey&, bool directOrigin) {
                if (directOrigin) {
                    starts.fetch_add(1, std::memory_order_relaxed);
                    runningGate->enterAndWait();
                }
            });

    const ChunkCoord demand{0, 0, 0};
    const RegionKey key{"rigel:default", 0, 0, 0};
    const ChunkLoadRequest firstRequest = makeLoadRequest(demand);
    const ChunkLoadRequest replacementRequest = makeLoadRequest(demand);
    CHECK_EQ(loader.request(firstRequest), ChunkLoadRequestResult::Queued);
    CHECK(runningGate->waitUntilEntered());

    const auto identity = Rigel::Persistence::detail::
        AsyncChunkLoaderTestAccess::regionJobIdentity(loader, key);
    const auto poolJob = Rigel::Persistence::detail::
        AsyncChunkLoaderTestAccess::regionPoolJobId(loader, key);
    CHECK(identity);
    CHECK(poolJob != 0);
    CHECK(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
              regionJobHasDirectOrigin(loader, key));
    CHECK(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
              regionJobHasDirectDemand(loader, key));
    const auto running = loader.metrics();
    CHECK_EQ(running.demandOwnedQueued, static_cast<size_t>(0));
    CHECK_EQ(running.speculativeOwnedQueued, static_cast<size_t>(0));
    CHECK_EQ(running.demandOwnedDispatchedUndrained,
             static_cast<size_t>(1));
    CHECK_EQ(running.speculativeOwnedDispatchedUndrained,
             static_cast<size_t>(0));
    CHECK_EQ(
        Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
            regionJobPhase(loader, key),
        std::optional<ChunkLoadExecutionPhase>{
            ChunkLoadExecutionPhase::WorkerRunning});

    loader.cancel(demand);
    CHECK(!loader.isPending(demand));
    const auto cancelled = loader.metrics();
    CHECK_EQ(cancelled.demandOwnedQueued, static_cast<size_t>(0));
    CHECK_EQ(cancelled.speculativeOwnedQueued, static_cast<size_t>(0));
    CHECK_EQ(cancelled.demandOwnedDispatchedUndrained,
             static_cast<size_t>(0));
    CHECK_EQ(cancelled.speculativeOwnedDispatchedUndrained,
             static_cast<size_t>(1));
    CHECK_EQ(cancelled.directOrigin.logicalAdmissions,
             static_cast<uint64_t>(1));
    CHECK_EQ(cancelled.directOrigin.logicalPreStartCancellations,
             static_cast<uint64_t>(0));
    CHECK_EQ(cancelled.directOrigin.terminalPoolCancellations,
             static_cast<uint64_t>(0));
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionJobIdentity(loader, key),
             identity);
    CHECK(!Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
              regionJobHasDirectDemand(loader, key));
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionPoolJobId(loader, key),
             poolJob);
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionLoadAttemptCount(loader, key),
             static_cast<size_t>(1));
    CHECK_EQ(
        Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
            regionJobPhase(loader, key),
        std::optional<ChunkLoadExecutionPhase>{
            ChunkLoadExecutionPhase::WorkerRunning});

    CHECK_EQ(loader.request(replacementRequest),
             ChunkLoadRequestResult::Queued);
    const auto reentered = loader.metrics();
    CHECK_EQ(reentered.demandPromotions, static_cast<uint64_t>(1));
    CHECK_EQ(reentered.demandOwnedQueued, static_cast<size_t>(0));
    CHECK_EQ(reentered.speculativeOwnedQueued, static_cast<size_t>(0));
    CHECK_EQ(reentered.demandOwnedDispatchedUndrained,
             static_cast<size_t>(1));
    CHECK_EQ(reentered.speculativeOwnedDispatchedUndrained,
             static_cast<size_t>(0));
    CHECK_EQ(reentered.directOrigin.logicalAdmissions,
             static_cast<uint64_t>(1));
    CHECK_EQ(reentered.directOrigin.poolSubmissions,
             static_cast<uint64_t>(1));
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionJobIdentity(loader, key),
             identity);
    CHECK(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
              regionJobHasDirectDemand(loader, key));
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionPoolJobId(loader, key),
             poolJob);
    CHECK_EQ(
        Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
            regionJobPhase(loader, key),
        std::optional<ChunkLoadExecutionPhase>{
            ChunkLoadExecutionPhase::WorkerRunning});

    runningGate->release();
    CHECK(waitForPublishedRegionJobs(loader, 1));
    CHECK_EQ(
        Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
            regionJobPhase(loader, key),
        std::optional<ChunkLoadExecutionPhase>{
            ChunkLoadExecutionPhase::ResultPublished});
    std::vector<ChunkLoadCompletion> resolved;
    CHECK(drainRegionJobsUntilSettled(loader, resolved));
    CHECK_EQ(resolved.size(), static_cast<size_t>(1));
    CHECK_EQ(resolved.front().coord, demand);
    CHECK_EQ(resolved.front().requestId, replacementRequest.requestId);
    CHECK_EQ(resolved.front().outcome, ChunkLoadOutcome::Missing);
    CHECK_EQ(starts.load(std::memory_order_relaxed), static_cast<size_t>(1));

    const auto settled = loader.metrics();
    CHECK_EQ(settled.directOrigin.logicalAdmissions,
             static_cast<uint64_t>(1));
    CHECK_EQ(settled.directOrigin.poolSubmissions,
             static_cast<uint64_t>(1));
    CHECK_EQ(settled.directOrigin.poolWorkerStarts,
             static_cast<uint64_t>(1));
    CHECK_EQ(settled.directOrigin.resultsPublished,
             static_cast<uint64_t>(1));
    CHECK_EQ(settled.directOrigin.resultsDrained,
             static_cast<uint64_t>(1));
    CHECK_EQ(settled.speculativeOrigin.logicalAdmissions,
             static_cast<uint64_t>(0));
    CHECK_EQ(settled.demandOwnedQueued, static_cast<size_t>(0));
    CHECK_EQ(settled.speculativeOwnedQueued, static_cast<size_t>(0));
    CHECK_EQ(settled.demandOwnedDispatchedUndrained,
             static_cast<size_t>(0));
    CHECK_EQ(settled.speculativeOwnedDispatchedUndrained,
             static_cast<size_t>(0));
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionOwnerCount(loader),
             static_cast<size_t>(0));
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionAttemptOwnerCount(loader),
             static_cast<size_t>(0));
    CHECK_EQ(loader.workCount().pending, static_cast<size_t>(0));
    CHECK_EQ(loader.workCount().inFlight, static_cast<size_t>(0));
}

TEST_CASE(AsyncChunkLoader_RunningDirectCancellationCachesAsPrefetch) {
    WorldResources resources;
    World world;
    world.initialize(resources);
    auto generator = makeGenerator(resources.registry());
    world.setGenerator(generator);

    MemoryContext ctx;
    auto runningGate = std::make_shared<LoaderWorkGate>();
    auto unusedPayloadGate = std::make_shared<LoaderWorkGate>();
    std::atomic<size_t> starts{0};
    AsyncChunkLoader loader(
        ctx.service,
        ctx.context,
        world,
        generator->config().world.version,
        1,
        0,
        0,
        generator);
    LoaderWorkRelease releaseOnExit(runningGate, unusedPayloadGate);
    loader.setPrefetchRadius(0);
    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
        setRegionLoadStartObserver(
            loader,
            [runningGate, &starts](const RegionKey&, bool directOrigin) {
                if (directOrigin) {
                    starts.fetch_add(1, std::memory_order_relaxed);
                    runningGate->enterAndWait();
                }
            });

    const ChunkCoord demand{0, 0, 0};
    const RegionKey key{"rigel:default", 0, 0, 0};
    CHECK_EQ(loader.request(makeLoadRequest(demand)),
             ChunkLoadRequestResult::Queued);
    CHECK(runningGate->waitUntilEntered());

    const auto identity = Rigel::Persistence::detail::
        AsyncChunkLoaderTestAccess::regionJobIdentity(loader, key);
    const auto poolJob = Rigel::Persistence::detail::
        AsyncChunkLoaderTestAccess::regionPoolJobId(loader, key);
    CHECK(identity);
    CHECK(poolJob != 0);
    CHECK(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
              regionJobHasDirectOrigin(loader, key));
    CHECK(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
              regionJobHasDirectDemand(loader, key));

    loader.cancel(demand);
    CHECK(!loader.isPending(demand));
    const auto cancelled = loader.metrics();
    CHECK_EQ(cancelled.directOrigin.logicalAdmissions,
             static_cast<uint64_t>(1));
    CHECK_EQ(cancelled.directOrigin.logicalPreStartCancellations,
             static_cast<uint64_t>(0));
    CHECK_EQ(cancelled.demandOwnedQueued, static_cast<size_t>(0));
    CHECK_EQ(cancelled.speculativeOwnedQueued, static_cast<size_t>(0));
    CHECK_EQ(cancelled.demandOwnedDispatchedUndrained,
             static_cast<size_t>(0));
    CHECK_EQ(cancelled.speculativeOwnedDispatchedUndrained,
             static_cast<size_t>(1));
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionJobIdentity(loader, key),
             identity);
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionPoolJobId(loader, key),
             poolJob);
    CHECK(!Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
              regionJobHasDirectDemand(loader, key));
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionLoadAttemptCount(loader, key),
             static_cast<size_t>(1));

    runningGate->release();
    CHECK(waitForPublishedRegionJobs(loader, 1));
    const auto published = loader.metrics();
    CHECK_EQ(published.directOrigin.resultsPublished,
             static_cast<uint64_t>(1));
    CHECK_EQ(published.directOrigin.resultsDrained,
             static_cast<uint64_t>(0));
    CHECK_EQ(published.speculativeOwnedDispatchedUndrained,
             static_cast<size_t>(1));
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionCompletionCount(loader),
             static_cast<size_t>(1));
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionJobIdentity(loader, key),
             identity);

    const auto resolved = loader.drainCompletions(1);
    CHECK(resolved.empty());
    const auto drained = loader.metrics();
    CHECK_EQ(drained.directOrigin.resultsPublished,
             static_cast<uint64_t>(1));
    CHECK_EQ(drained.directOrigin.resultsDrained,
             static_cast<uint64_t>(1));
    CHECK_EQ(drained.directOrigin.poolSubmissions,
             static_cast<uint64_t>(1));
    CHECK_EQ(drained.directOrigin.poolWorkerStarts,
             static_cast<uint64_t>(1));
    CHECK_EQ(drained.usefulPrefetchCacheHits, static_cast<uint64_t>(0));
    CHECK_EQ(drained.demandOwnedQueued, static_cast<size_t>(0));
    CHECK_EQ(drained.speculativeOwnedQueued, static_cast<size_t>(0));
    CHECK_EQ(drained.demandOwnedDispatchedUndrained,
             static_cast<size_t>(0));
    CHECK_EQ(drained.speculativeOwnedDispatchedUndrained,
             static_cast<size_t>(0));
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionCacheCount(loader),
             static_cast<size_t>(1));

    const RegionLifecycleSnapshot beforeCachedDemand =
        regionLifecycleSnapshot(drained);
    CHECK_EQ(loader.request(makeLoadRequest(demand)),
             ChunkLoadRequestResult::Missing);
    CHECK_EQ(loader.metrics().usefulPrefetchCacheHits,
             static_cast<uint64_t>(1));
    CHECK_EQ(regionLifecycleSnapshot(loader.metrics()), beforeCachedDemand);

    CHECK_EQ(loader.request(makeLoadRequest(demand)),
             ChunkLoadRequestResult::Missing);
    const auto repeatedCacheUse = loader.metrics();
    CHECK_EQ(repeatedCacheUse.usefulPrefetchCacheHits,
             static_cast<uint64_t>(1));
    CHECK_EQ(regionLifecycleSnapshot(repeatedCacheUse), beforeCachedDemand);
    CHECK_EQ(starts.load(std::memory_order_relaxed), static_cast<size_t>(1));
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionOwnerCount(loader),
             static_cast<size_t>(0));
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionAttemptOwnerCount(loader),
             static_cast<size_t>(0));
    CHECK_EQ(loader.workCount().pending, static_cast<size_t>(0));
    CHECK_EQ(loader.workCount().inFlight, static_cast<size_t>(0));
}

TEST_CASE(AsyncChunkLoader_CancelRemovesLoaderQueuedDirectRegionJob) {
    WorldResources resources;
    World world;
    world.initialize(resources);
    auto generator = makeGenerator(resources.registry());
    world.setGenerator(generator);

    MemoryContext ctx;
    auto activeRegionGate = std::make_shared<LoaderWorkGate>();
    auto unusedPayloadGate = std::make_shared<LoaderWorkGate>();
    AsyncChunkLoader loader(
        ctx.service,
        ctx.context,
        world,
        generator->config().world.version,
        1,
        0,
        12,
        generator);
    LoaderWorkRelease releaseOnExit(activeRegionGate, unusedPayloadGate);
    loader.setMaxInFlightRegions(16);
    loader.setPrefetchRadius(0);

    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
        setRegionLoadStartObserver(
            loader,
            [activeRegionGate](const RegionKey& key, bool direct) {
                if (direct && key.x == 0 && key.y == 0 && key.z == 0) {
                    activeRegionGate->enterAndWait();
                }
            });

    const ChunkCoord active{0, 0, 0};
    const ChunkCoord cancelled{160, 0, 0};
    const RegionKey cancelledKey{"rigel:default", 10, 0, 0};
    CHECK_EQ(loader.request(makeLoadRequest(active)),
             ChunkLoadRequestResult::Queued);
    CHECK(activeRegionGate->waitUntilEntered());
    CHECK_EQ(loader.request(makeLoadRequest(cancelled)),
             ChunkLoadRequestResult::Queued);
    CHECK_EQ(loader.metrics().demandOwnedDispatchedUndrained, static_cast<size_t>(1));
    CHECK_EQ(loader.metrics().demandOwnedQueued, static_cast<size_t>(1));
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionOwnerCount(loader),
             static_cast<size_t>(2));
    CHECK(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
              regionJobHasDirectOrigin(loader, cancelledKey));
    CHECK(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
              regionJobHasDirectDemand(loader, cancelledKey));
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionPoolJobId(loader, cancelledKey),
             Rigel::Voxel::detail::ThreadPool::JobId{0});
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionLoadAttemptCount(loader, cancelledKey),
             static_cast<size_t>(0));

    loader.cancel(cancelled);
    CHECK(!loader.isPending(cancelled));
    CHECK_EQ(loader.metrics().directOrigin.logicalPreStartCancellations,
             static_cast<uint64_t>(1));
    CHECK_EQ(loader.metrics().demandOwnedQueued, static_cast<size_t>(0));
    CHECK_EQ(loader.metrics().demandOwnedDispatchedUndrained, static_cast<size_t>(1));
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionOwnerCount(loader),
             static_cast<size_t>(1));
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionJobIdentity(loader, cancelledKey),
             std::shared_ptr<const void>{});
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionLoadAttemptCount(loader, cancelledKey),
             static_cast<size_t>(0));
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionCacheCount(loader),
             static_cast<size_t>(0));
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionCompletionCount(loader),
             static_cast<size_t>(0));

    activeRegionGate->release();
    std::vector<ChunkLoadCompletion> resolved;
    CHECK(drainRegionJobsUntilSettled(loader, resolved));
    CHECK_EQ(resolved.size(), static_cast<size_t>(1));
    CHECK_EQ(resolved.front().coord, active);

    CHECK_EQ(loader.request(makeLoadRequest(cancelled)),
             ChunkLoadRequestResult::Queued);
    CHECK(drainRegionJobsUntilSettled(loader, resolved));
    CHECK_EQ(resolved.size(), static_cast<size_t>(2));
    CHECK_EQ(resolved.back().coord, cancelled);
    CHECK_EQ(loader.metrics().directOrigin.logicalAdmissions, static_cast<uint64_t>(3));
    CHECK_EQ(loader.metrics().directOrigin.resultsPublished, static_cast<uint64_t>(2));
    CHECK_EQ(loader.metrics().directOrigin.logicalPreStartCancellations,
             static_cast<uint64_t>(1));
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionOwnerCount(loader),
             static_cast<size_t>(0));
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionAttemptOwnerCount(loader),
             static_cast<size_t>(0));
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionCompletionCount(loader),
             static_cast<size_t>(0));
    CHECK_EQ(loader.workCount().pending, static_cast<size_t>(0));
    CHECK_EQ(loader.workCount().inFlight, static_cast<size_t>(0));
}

TEST_CASE(AsyncChunkLoader_LoaderQueuedPrefetchKeepsOwnerAcrossDemandChurn) {
    WorldResources resources;
    World world;
    world.initialize(resources);
    auto generator = makeGenerator(resources.registry());
    world.setGenerator(generator);

    MemoryContext ctx;
    auto activeGate = std::make_shared<LoaderWorkGate>();
    auto unusedPayloadGate = std::make_shared<LoaderWorkGate>();
    std::mutex startsMutex;
    std::vector<RegionKey> starts;
    AsyncChunkLoader loader(
        ctx.service,
        ctx.context,
        world,
        generator->config().world.version,
        1,
        0,
        12,
        generator);
    LoaderWorkRelease releaseOnExit(activeGate, unusedPayloadGate);
    loader.setMaxInFlightRegions(16);
    loader.setPrefetchRadius(1);
    loader.setPrefetchPerRequest(2);

    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
        setRegionLoadStartObserver(
            loader,
            [activeGate, &startsMutex, &starts](
                const RegionKey& key, bool) {
                bool first = false;
                {
                    std::lock_guard<std::mutex> lock(startsMutex);
                    starts.push_back(key);
                    first = starts.size() == 1;
                }
                if (first) {
                    activeGate->enterAndWait();
                }
            });

    CHECK_EQ(loader.request(makeLoadRequest({0, 0, 0})),
             ChunkLoadRequestResult::Queued);
    CHECK(activeGate->waitUntilEntered());
    const auto queuedSpeculation = Rigel::Persistence::detail::
        AsyncChunkLoaderTestAccess::queuedSpeculativeRegionKeys(loader);
    CHECK_EQ(queuedSpeculation.size(), static_cast<size_t>(2));
    const RegionKey key = queuedSpeculation.front();
    const ChunkCoord demand{key.x * 16, key.y * 16, key.z * 16};
    const auto identity = Rigel::Persistence::detail::
        AsyncChunkLoaderTestAccess::regionJobIdentity(loader, key);
    CHECK(identity);
    CHECK(!Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
              regionPhysicallyInFlight(loader, key));
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionPoolJobId(loader, key),
             Rigel::Voxel::detail::ThreadPool::JobId{0});
    const auto initiallyQueued = loader.metrics();
    CHECK_EQ(initiallyQueued.demandOwnedQueued, static_cast<size_t>(0));
    CHECK_EQ(initiallyQueued.speculativeOwnedQueued, static_cast<size_t>(2));
    CHECK_EQ(initiallyQueued.demandOwnedDispatchedUndrained,
             static_cast<size_t>(1));
    CHECK_EQ(initiallyQueued.speculativeOwnedDispatchedUndrained,
             static_cast<size_t>(0));
    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
        markRegionKnownMissing(loader, key);

    CHECK_EQ(loader.request(makeLoadRequest(demand)),
             ChunkLoadRequestResult::Queued);
    const auto promoted = loader.metrics();
    CHECK_EQ(promoted.demandPromotions, static_cast<uint64_t>(1));
    CHECK_EQ(promoted.demandOwnedQueued, static_cast<size_t>(1));
    CHECK_EQ(promoted.speculativeOwnedQueued, static_cast<size_t>(1));
    CHECK_EQ(promoted.demandOwnedDispatchedUndrained,
             static_cast<size_t>(1));
    CHECK_EQ(promoted.speculativeOwnedDispatchedUndrained,
             static_cast<size_t>(0));
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionJobIdentity(loader, key),
             identity);
    CHECK(!Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
              regionPhysicallyInFlight(loader, key));
    CHECK(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
              regionJobHasDirectDemand(loader, key));

    loader.cancel(demand);
    CHECK(!loader.isPending(demand));
    const auto cancelled = loader.metrics();
    CHECK_EQ(cancelled.demandOwnedQueued, static_cast<size_t>(0));
    CHECK_EQ(cancelled.speculativeOwnedQueued, static_cast<size_t>(2));
    CHECK_EQ(cancelled.demandOwnedDispatchedUndrained,
             static_cast<size_t>(1));
    CHECK_EQ(cancelled.speculativeOwnedDispatchedUndrained,
             static_cast<size_t>(0));
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionJobIdentity(loader, key),
             identity);
    CHECK(!Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
              regionJobHasDirectDemand(loader, key));
    CHECK(!Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
              regionPhysicallyInFlight(loader, key));

    CHECK_EQ(loader.request(makeLoadRequest(demand)),
             ChunkLoadRequestResult::Queued);
    const auto reentered = loader.metrics();
    CHECK_EQ(reentered.demandPromotions, static_cast<uint64_t>(2));
    CHECK_EQ(reentered.demandOwnedQueued, static_cast<size_t>(1));
    CHECK_EQ(reentered.speculativeOwnedQueued, static_cast<size_t>(1));
    CHECK_EQ(reentered.demandOwnedDispatchedUndrained,
             static_cast<size_t>(1));
    CHECK_EQ(reentered.speculativeOwnedDispatchedUndrained,
             static_cast<size_t>(0));
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionJobIdentity(loader, key),
             identity);
    CHECK(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
              regionJobHasDirectDemand(loader, key));

    activeGate->release();
    std::vector<ChunkLoadCompletion> resolved;
    CHECK(drainRegionJobsUntilSettled(loader, resolved));
    CHECK_EQ(resolved.size(), static_cast<size_t>(2));
    {
        std::lock_guard<std::mutex> lock(startsMutex);
        CHECK_EQ(std::count(starts.begin(), starts.end(), key),
                 static_cast<std::ptrdiff_t>(1));
    }
    CHECK_EQ(loader.metrics().speculativeOrigin.logicalAdmissions,
             static_cast<uint64_t>(2));
    CHECK_EQ(loader.metrics().speculativeOrigin.resultsPublished,
             static_cast<uint64_t>(2));
    CHECK_EQ(loader.metrics().demandOwnedQueued, static_cast<size_t>(0));
    CHECK_EQ(loader.metrics().speculativeOwnedQueued, static_cast<size_t>(0));
    CHECK_EQ(loader.metrics().demandOwnedDispatchedUndrained,
             static_cast<size_t>(0));
    CHECK_EQ(loader.metrics().speculativeOwnedDispatchedUndrained,
             static_cast<size_t>(0));
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionOwnerCount(loader),
             static_cast<size_t>(0));
    CHECK_EQ(loader.workCount().inFlight, static_cast<size_t>(0));
}

TEST_CASE(AsyncChunkLoader_PromotesPoolPendingSpeculationAheadOfNormalWork) {
    WorldResources resources;
    World world;
    world.initialize(resources);
    auto generator = makeGenerator(resources.registry());
    world.setGenerator(generator);

    MemoryContext ctx;
    auto firstPoolBlocker = std::make_shared<LoaderWorkGate>();
    auto secondPoolBlocker = std::make_shared<LoaderWorkGate>();
    std::mutex startsMutex;
    std::condition_variable startsCondition;
    std::vector<RegionKey> starts;
    const RegionKey competingNormalWork{"rigel:test_pool_work", 0, 0, 0};
    AsyncChunkLoader loader(
        ctx.service,
        ctx.context,
        world,
        generator->config().world.version,
        2,
        0,
        12,
        generator);
    LoaderWorkRelease releaseOnExit(firstPoolBlocker, secondPoolBlocker);
    loader.setMaxInFlightRegions(16);
    loader.setPrefetchRadius(1);
    loader.setPrefetchPerRequest(1);

    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::enqueueIoPoolJob(
        loader,
        [firstPoolBlocker]() { firstPoolBlocker->enterAndWait(); });
    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::enqueueIoPoolJob(
        loader,
        [secondPoolBlocker]() { secondPoolBlocker->enterAndWait(); });
    CHECK(firstPoolBlocker->waitUntilEntered());
    CHECK(secondPoolBlocker->waitUntilEntered());

    const auto competingPoolJob = Rigel::Persistence::detail::
        AsyncChunkLoaderTestAccess::enqueueIoPoolJob(
            loader,
            [&startsMutex,
             &startsCondition,
             &starts,
             competingNormalWork]() {
                {
                    std::lock_guard<std::mutex> lock(startsMutex);
                    starts.push_back(competingNormalWork);
                }
                startsCondition.notify_all();
            });
    CHECK(competingPoolJob != 0);
    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
        setRegionLoadStartObserver(
            loader,
            [&startsMutex, &startsCondition, &starts](
                const RegionKey& key,
                bool) {
                {
                    std::lock_guard<std::mutex> lock(startsMutex);
                    starts.push_back(key);
                }
                startsCondition.notify_all();
            });

    const ChunkCoord initialDemand{0, 0, 0};
    const RegionKey initialKey{"rigel:default", 0, 0, 0};
    CHECK_EQ(loader.request(makeLoadRequest(initialDemand)),
             ChunkLoadRequestResult::Queued);
    const auto submittedSpeculative = Rigel::Persistence::detail::
        AsyncChunkLoaderTestAccess::submittedSpeculativeRegionKeys(loader);
    CHECK_EQ(submittedSpeculative.size(), static_cast<size_t>(1));
    const RegionKey prefetchedKey = submittedSpeculative.front();
    const ChunkCoord prefetchedDemand{
        prefetchedKey.x * 16,
        prefetchedKey.y * 16,
        prefetchedKey.z * 16};
    const auto speculativeIdentity = Rigel::Persistence::detail::
        AsyncChunkLoaderTestAccess::regionJobIdentity(loader, prefetchedKey);
    const auto speculativePoolJob = Rigel::Persistence::detail::
        AsyncChunkLoaderTestAccess::regionPoolJobId(loader, prefetchedKey);
    CHECK(speculativeIdentity);
    CHECK(speculativePoolJob != 0);
    CHECK_NE(speculativePoolJob, competingPoolJob);
    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
        markRegionKnownMissing(loader, prefetchedKey);

    CHECK_EQ(loader.request(makeLoadRequest(prefetchedDemand)),
             ChunkLoadRequestResult::Queued);
    CHECK_EQ(loader.metrics().demandPromotions, static_cast<uint64_t>(1));
    CHECK_EQ(loader.metrics().directOrigin.logicalAdmissions, static_cast<uint64_t>(1));
    CHECK_EQ(loader.metrics().speculativeOrigin.logicalAdmissions, static_cast<uint64_t>(1));
    CHECK_EQ(loader.metrics().speculativePoolJobsPending,
             static_cast<size_t>(0));
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionJobIdentity(loader, prefetchedKey),
             speculativeIdentity);
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionPoolJobId(loader, prefetchedKey),
             speculativePoolJob);
    CHECK(!Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
              regionJobHasDirectOrigin(loader, prefetchedKey));
    CHECK(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
              regionJobHasDirectDemand(loader, prefetchedKey));
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionLoadAttemptCount(loader, prefetchedKey),
             static_cast<size_t>(1));

    firstPoolBlocker->release();
    {
        std::unique_lock<std::mutex> lock(startsMutex);
        CHECK(startsCondition.wait_for(
            lock,
            std::chrono::seconds(5),
            [&starts]() { return starts.size() == 3; }));
        CHECK_EQ(starts[0], initialKey);
        CHECK_EQ(starts[1], prefetchedKey);
        CHECK_EQ(starts[2], competingNormalWork);
    }

    secondPoolBlocker->release();
    std::vector<ChunkLoadCompletion> resolved;
    CHECK(drainRegionJobsUntilSettled(loader, resolved));
    CHECK_EQ(resolved.size(), static_cast<size_t>(2));
    CHECK_EQ(loader.metrics().speculativeOrigin.poolWorkerStarts,
             static_cast<uint64_t>(1));
    CHECK_EQ(loader.metrics().speculativeOrigin.resultsPublished,
             static_cast<uint64_t>(1));
    CHECK_EQ(loader.metrics().speculativePoolJobsPending,
             static_cast<size_t>(0));
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionOwnerCount(loader),
             static_cast<size_t>(0));
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionAttemptOwnerCount(loader),
             static_cast<size_t>(0));
    CHECK_EQ(loader.workCount().pending, static_cast<size_t>(0));
    CHECK_EQ(loader.workCount().inFlight, static_cast<size_t>(0));
}

TEST_CASE(AsyncChunkLoader_SameRegionDemandPromotesAndCoalescesPrefetch) {
    WorldResources resources;
    World world;
    world.initialize(resources);
    auto generator = makeGenerator(resources.registry());
    world.setGenerator(generator);

    MemoryContext ctx;
    auto firstPoolBlocker = std::make_shared<LoaderWorkGate>();
    auto secondPoolBlocker = std::make_shared<LoaderWorkGate>();
    auto prefetchedStartGate = std::make_shared<LoaderWorkGate>();
    auto unusedPayloadGate = std::make_shared<LoaderWorkGate>();
    auto gatedKey = std::make_shared<std::optional<RegionKey>>();
    std::mutex startsMutex;
    std::vector<std::pair<RegionKey, bool>> starts;
    AsyncChunkLoader loader(
        ctx.service,
        ctx.context,
        world,
        generator->config().world.version,
        2,
        0,
        12,
        generator);
    LoaderWorkRelease releaseOnExit(firstPoolBlocker, secondPoolBlocker);
    LoaderWorkRelease releasePrefetchedOnExit(
        prefetchedStartGate, unusedPayloadGate);
    loader.setMaxInFlightRegions(16);
    loader.setPrefetchRadius(1);
    loader.setPrefetchPerRequest(12);

    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::enqueueIoPoolJob(
        loader,
        [firstPoolBlocker]() { firstPoolBlocker->enterAndWait(); });
    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::enqueueIoPoolJob(
        loader,
        [secondPoolBlocker]() { secondPoolBlocker->enterAndWait(); });
    CHECK(firstPoolBlocker->waitUntilEntered());
    CHECK(secondPoolBlocker->waitUntilEntered());

    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
        setRegionLoadStartObserver(
            loader,
            [prefetchedStartGate, gatedKey, &startsMutex, &starts](
                const RegionKey& key,
                bool direct) {
                bool gateStart = false;
                {
                    std::lock_guard<std::mutex> lock(startsMutex);
                    starts.emplace_back(key, direct);
                    gateStart = *gatedKey && key == **gatedKey;
                }
                if (gateStart) {
                    prefetchedStartGate->enterAndWait();
                }
            });

    CHECK_EQ(loader.request(makeLoadRequest({0, 0, 0})),
             ChunkLoadRequestResult::Queued);
    const auto beforeDemand = loader.metrics();
    CHECK_EQ(beforeDemand.directOrigin.logicalAdmissions, static_cast<uint64_t>(1));
    CHECK_EQ(beforeDemand.speculativeOrigin.logicalAdmissions, static_cast<uint64_t>(12));
    const auto submittedSpeculative = Rigel::Persistence::detail::
        AsyncChunkLoaderTestAccess::submittedSpeculativeRegionKeys(loader);
    CHECK_EQ(submittedSpeculative.size(), static_cast<size_t>(1));
    const RegionKey prefetchedKey = submittedSpeculative.front();
    *gatedKey = prefetchedKey;
    const ChunkCoord prefetchedNeighbor{
        prefetchedKey.x * 16,
        prefetchedKey.y * 16,
        prefetchedKey.z * 16};
    const auto speculativeIdentity = Rigel::Persistence::detail::
        AsyncChunkLoaderTestAccess::regionJobIdentity(loader, prefetchedKey);
    const auto originalPoolJob = Rigel::Persistence::detail::
        AsyncChunkLoaderTestAccess::regionPoolJobId(loader, prefetchedKey);
    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
        markRegionKnownMissing(loader, prefetchedKey);

    CHECK_EQ(loader.request(makeLoadRequest(prefetchedNeighbor)),
             ChunkLoadRequestResult::Queued);
    const auto promoted = loader.metrics();
    CHECK_EQ(promoted.directOrigin.logicalAdmissions, beforeDemand.directOrigin.logicalAdmissions);
    CHECK_EQ(promoted.speculativeOrigin.logicalAdmissions,
             beforeDemand.speculativeOrigin.logicalAdmissions);
    CHECK_EQ(promoted.demandPromotions, static_cast<uint64_t>(1));
    CHECK_EQ(promoted.demandOwnedDispatchedUndrained, static_cast<size_t>(2));
    CHECK_EQ(promoted.demandOwnedQueued, static_cast<size_t>(0));
    CHECK_EQ(promoted.speculativeOwnedQueued, static_cast<size_t>(11));
    CHECK_EQ(promoted.speculativePoolJobsPending, static_cast<size_t>(0));
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionJobIdentity(loader, prefetchedKey),
             speculativeIdentity);
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionPoolJobId(loader, prefetchedKey),
             originalPoolJob);
    CHECK(!Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
              regionJobHasDirectOrigin(loader, prefetchedKey));
    CHECK(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
              regionJobHasDirectDemand(loader, prefetchedKey));
    CHECK(loader.isPending(prefetchedNeighbor));

    loader.cancel(prefetchedNeighbor);
    const auto cancelledPromotion = loader.metrics();
    CHECK(!loader.isPending(prefetchedNeighbor));
    CHECK_EQ(cancelledPromotion.demandOwnedQueued,
             static_cast<size_t>(0));
    CHECK_EQ(cancelledPromotion.speculativeOwnedQueued,
             static_cast<size_t>(11));
    CHECK_EQ(cancelledPromotion.speculativeOwnedDispatchedUndrained,
             static_cast<size_t>(1));
    CHECK_EQ(cancelledPromotion.speculativePoolJobsPending,
             static_cast<size_t>(1));
    CHECK_EQ(cancelledPromotion.speculativeOrigin.logicalPreStartCancellations,
             static_cast<uint64_t>(0));
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionJobIdentity(loader, prefetchedKey),
             speculativeIdentity);
    const auto demotedPoolJob = Rigel::Persistence::detail::
        AsyncChunkLoaderTestAccess::regionPoolJobId(loader, prefetchedKey);
    CHECK_EQ(demotedPoolJob, Rigel::Voxel::detail::ThreadPool::JobId{0});
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionLoadAttemptCount(loader, prefetchedKey),
             static_cast<size_t>(0));
    CHECK_EQ(
        Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
            regionJobPhase(loader, prefetchedKey),
        std::optional<ChunkLoadExecutionPhase>{
            ChunkLoadExecutionPhase::SchedulerPending});

    CHECK_EQ(loader.request(makeLoadRequest(prefetchedNeighbor)),
             ChunkLoadRequestResult::Queued);
    const auto replacementPromotion = loader.metrics();
    CHECK_EQ(replacementPromotion.demandPromotions, static_cast<uint64_t>(2));
    CHECK_EQ(replacementPromotion.demandOwnedQueued,
             static_cast<size_t>(0));
    CHECK_EQ(replacementPromotion.speculativeOwnedQueued,
             static_cast<size_t>(11));
    const auto replacementPoolJob = Rigel::Persistence::detail::
        AsyncChunkLoaderTestAccess::regionPoolJobId(loader, prefetchedKey);
    CHECK(replacementPoolJob != 0);
    CHECK_NE(replacementPoolJob, originalPoolJob);
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionJobIdentity(loader, prefetchedKey),
             speculativeIdentity);
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionLoadAttemptCount(loader, prefetchedKey),
             static_cast<size_t>(1));
    CHECK_EQ(
        Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
            regionJobPhase(loader, prefetchedKey),
        std::optional<ChunkLoadExecutionPhase>{
            ChunkLoadExecutionPhase::PoolQueued});

    firstPoolBlocker->release();
    secondPoolBlocker->release();
    CHECK(prefetchedStartGate->waitUntilEntered());
    CHECK_EQ(
        Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
            regionJobPhase(loader, prefetchedKey),
        std::optional<ChunkLoadExecutionPhase>{
            ChunkLoadExecutionPhase::WorkerRunning});
    prefetchedStartGate->release();
    CHECK(waitForPublishedRegionJobs(loader, 2));
    CHECK_EQ(
        Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
            regionJobPhase(loader, prefetchedKey),
        std::optional<ChunkLoadExecutionPhase>{
            ChunkLoadExecutionPhase::ResultPublished});
    auto resolved = loader.drainCompletions(2);
    {
        std::lock_guard<std::mutex> lock(startsMutex);
        const auto prefetchedStart = std::find_if(
            starts.begin(), starts.end(), [&prefetchedKey](const auto& start) {
                return start.first == prefetchedKey;
            });
        CHECK(prefetchedStart != starts.end());
        CHECK(!prefetchedStart->second);
        CHECK_EQ(std::count_if(
                     starts.begin(),
                     starts.end(),
                     [&prefetchedKey](const auto& start) {
                         return start.first == prefetchedKey;
                     }),
                 static_cast<std::ptrdiff_t>(1));
    }
    CHECK(drainRegionJobsUntilSettled(loader, resolved));
    CHECK_EQ(resolved.size(), static_cast<size_t>(2));
    CHECK(!loader.isPending(prefetchedNeighbor));
    CHECK_EQ(loader.metrics().usefulPrefetchCacheHits,
             static_cast<uint64_t>(0));
    CHECK_EQ(loader.metrics().speculativeOrigin.poolWorkerStarts,
             loader.metrics().speculativeOrigin.logicalAdmissions);
    CHECK_EQ(loader.metrics().speculativeOrigin.resultsPublished,
             loader.metrics().speculativeOrigin.logicalAdmissions);
    CHECK_EQ(loader.metrics().speculativePoolJobsPending,
             static_cast<size_t>(0));
    CHECK_EQ(loader.workCount().pending, static_cast<size_t>(0));
    CHECK_EQ(loader.workCount().inFlight, static_cast<size_t>(0));
}

TEST_CASE(AsyncChunkLoader_RunningSpeculativeDemandCompletesOnce) {
    WorldResources resources;
    World world;
    world.initialize(resources);
    auto generator = makeGenerator(resources.registry());
    world.setGenerator(generator);

    MemoryContext ctx;
    auto speculativeGate = std::make_shared<LoaderWorkGate>();
    auto unusedPayloadGate = std::make_shared<LoaderWorkGate>();
    std::mutex startMutex;
    std::optional<RegionKey> runningSpeculative;
    AsyncChunkLoader loader(
        ctx.service,
        ctx.context,
        world,
        generator->config().world.version,
        2,
        0,
        12,
        generator);
    LoaderWorkRelease releaseOnExit(speculativeGate, unusedPayloadGate);
    loader.setMaxInFlightRegions(16);
    loader.setPrefetchRadius(1);
    loader.setPrefetchPerRequest(1);

    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
        setRegionLoadStartObserver(
            loader,
            [speculativeGate, &startMutex, &runningSpeculative](
                const RegionKey& key,
                bool directOrigin) {
                if (directOrigin) {
                    return;
                }
                {
                    std::lock_guard<std::mutex> lock(startMutex);
                    runningSpeculative = key;
                }
                speculativeGate->enterAndWait();
            });

    CHECK_EQ(loader.request(makeLoadRequest({0, 0, 0})),
             ChunkLoadRequestResult::Queued);
    CHECK(speculativeGate->waitUntilEntered());

    RegionKey speculativeKey;
    {
        std::lock_guard<std::mutex> lock(startMutex);
        CHECK(runningSpeculative.has_value());
        speculativeKey = *runningSpeculative;
    }
    const ChunkCoord speculativeCoord{
        speculativeKey.x * 16,
        speculativeKey.y * 16,
        speculativeKey.z * 16};
    const auto speculativeIdentity = Rigel::Persistence::detail::
        AsyncChunkLoaderTestAccess::regionJobIdentity(loader, speculativeKey);
    const auto runningPoolJob = Rigel::Persistence::detail::
        AsyncChunkLoaderTestAccess::regionPoolJobId(loader, speculativeKey);
    CHECK(speculativeIdentity);
    CHECK(runningPoolJob != 0);
    CHECK_EQ(loader.metrics().speculativePoolJobsPending,
             static_cast<size_t>(0));
    CHECK_EQ(loader.metrics().speculativeOwnedDispatchedUndrained,
             static_cast<size_t>(1));

    CHECK_EQ(loader.request(makeLoadRequest(speculativeCoord)),
             ChunkLoadRequestResult::Queued);
    CHECK_EQ(loader.metrics().speculativePoolJobsPending,
             static_cast<size_t>(0));
    CHECK(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
              regionJobHasDirectDemand(loader, speculativeKey));
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionPoolJobId(loader, speculativeKey),
             runningPoolJob);

    loader.cancel(speculativeCoord);
    CHECK(!loader.isPending(speculativeCoord));
    CHECK_EQ(loader.metrics().speculativePoolJobsPending,
             static_cast<size_t>(0));
    CHECK_EQ(loader.metrics().speculativeOwnedDispatchedUndrained,
             static_cast<size_t>(1));
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionJobIdentity(loader, speculativeKey),
             speculativeIdentity);
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionPoolJobId(loader, speculativeKey),
             runningPoolJob);
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionLoadAttemptCount(loader, speculativeKey),
             static_cast<size_t>(1));

    CHECK_EQ(loader.request(makeLoadRequest(speculativeCoord)),
             ChunkLoadRequestResult::Queued);
    CHECK_EQ(loader.metrics().demandPromotions, static_cast<uint64_t>(2));
    CHECK_EQ(loader.metrics().speculativePoolJobsPending,
             static_cast<size_t>(0));
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionPoolJobId(loader, speculativeKey),
             runningPoolJob);

    speculativeGate->release();
    CHECK(waitForPublishedRegionJobs(loader, 2));
    std::vector<ChunkLoadCompletion> resolved;
    CHECK(drainRegionJobsUntilSettled(loader, resolved));
    CHECK_EQ(resolved.size(), static_cast<size_t>(2));
    const auto settled = loader.metrics();
    CHECK_EQ(settled.speculativeOrigin.poolWorkerStarts,
             static_cast<uint64_t>(1));
    CHECK_EQ(settled.speculativeOrigin.resultsPublished, static_cast<uint64_t>(1));
    CHECK_EQ(settled.speculativeOrigin.logicalPreStartCancellations,
             static_cast<uint64_t>(0));
    CHECK_EQ(settled.speculativePoolJobsPending, static_cast<size_t>(0));
    CHECK_EQ(loader.workCount().pending, static_cast<size_t>(0));
    CHECK_EQ(loader.workCount().inFlight, static_cast<size_t>(0));
}

TEST_CASE(AsyncChunkLoader_SubmittedJobSnapshotsStartObserver) {
    WorldResources resources;
    World world;
    world.initialize(resources);
    auto generator = makeGenerator(resources.registry());
    world.setGenerator(generator);

    MemoryContext ctx;
    auto firstPoolBlocker = std::make_shared<LoaderWorkGate>();
    auto secondPoolBlocker = std::make_shared<LoaderWorkGate>();
    std::atomic<size_t> originalObserverCalls = 0;
    std::atomic<size_t> replacementObserverCalls = 0;
    AsyncChunkLoader loader(
        ctx.service,
        ctx.context,
        world,
        generator->config().world.version,
        2,
        0,
        12,
        generator);
    LoaderWorkRelease releaseOnExit(firstPoolBlocker, secondPoolBlocker);
    loader.setMaxInFlightRegions(2);
    loader.setPrefetchRadius(0);

    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::enqueueIoPoolJob(
        loader,
        [firstPoolBlocker]() { firstPoolBlocker->enterAndWait(); });
    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::enqueueIoPoolJob(
        loader,
        [secondPoolBlocker]() { secondPoolBlocker->enterAndWait(); });
    CHECK(firstPoolBlocker->waitUntilEntered());
    CHECK(secondPoolBlocker->waitUntilEntered());

    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
        setRegionLoadStartObserver(
            loader,
            [&originalObserverCalls](const RegionKey&, bool) {
                originalObserverCalls.fetch_add(1, std::memory_order_relaxed);
            });
    CHECK_EQ(loader.request(makeLoadRequest({0, 0, 0})),
             ChunkLoadRequestResult::Queued);
    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
        setRegionLoadStartObserver(
            loader,
            [&replacementObserverCalls](const RegionKey&, bool) {
                replacementObserverCalls.fetch_add(1, std::memory_order_relaxed);
            });

    firstPoolBlocker->release();
    secondPoolBlocker->release();
    CHECK(waitForPublishedRegionJobs(loader, 1));
    const auto resolved = loader.drainCompletions(1);
    CHECK_EQ(resolved.size(), static_cast<size_t>(1));
    CHECK_EQ(originalObserverCalls.load(std::memory_order_relaxed),
             static_cast<size_t>(1));
    CHECK_EQ(replacementObserverCalls.load(std::memory_order_relaxed),
             static_cast<size_t>(0));
}

TEST_CASE(AsyncChunkLoader_GatedFixtureUnwindsAfterExpectedException) {
    WorldResources resources;
    World world;
    world.initialize(resources);
    auto generator = makeGenerator(resources.registry());
    world.setGenerator(generator);
    MemoryContext ctx;

    bool exceptionReachedCaller = false;
    try {
        auto regionGate = std::make_shared<LoaderWorkGate>();
        auto unusedPayloadGate = std::make_shared<LoaderWorkGate>();
        AsyncChunkLoader loader(
            ctx.service,
            ctx.context,
            world,
            generator->config().world.version,
            1,
            0,
            12,
            generator);
        LoaderWorkRelease releaseOnExit(regionGate, unusedPayloadGate);
        loader.setPrefetchRadius(0);
        Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
            setRegionLoadStartCallback(
                loader,
                [regionGate]() { regionGate->enterAndWait(); });
        CHECK_EQ(loader.request(makeLoadRequest({0, 0, 0})),
                 ChunkLoadRequestResult::Queued);
        CHECK(regionGate->waitUntilEntered());
        throw ExpectedLoaderFixtureError();
    } catch (const ExpectedLoaderFixtureError&) {
        exceptionReachedCaller = true;
    }
    CHECK(exceptionReachedCaller);
}

TEST_CASE(AsyncChunkLoader_DirectDemandChurnBoundsSubmittedSpeculation) {
    WorldResources resources;
    World world;
    world.initialize(resources);
    auto generator = makeGenerator(resources.registry());
    world.setGenerator(generator);

    MemoryContext ctx;
    {
        auto format = ctx.service.openFormat(ctx.context);
        for (int z = -1; z <= 1; ++z) {
            for (int y = -1; y <= 1; ++y) {
                for (int x = -1; x <= 1; ++x) {
                    if (x == 0 && y == 0 && z == 0) {
                        continue;
                    }
                    ChunkRegionSnapshot region;
                    region.key = RegionKey{"rigel:default", x, y, z};
                    format->chunkContainer().saveRegion(region);
                }
            }
        }
    }
    auto failingStorage = std::make_shared<TransientReadFailureStorage>(
        ctx.context.storage,
        1);
    ctx.context.storage = failingStorage;

    auto firstPoolBlocker = std::make_shared<LoaderWorkGate>();
    auto secondPoolBlocker = std::make_shared<LoaderWorkGate>();
    AsyncChunkLoader loader(
        ctx.service,
        ctx.context,
        world,
        generator->config().world.version,
        2,
        0,
        12,
        generator);
    LoaderWorkRelease releaseOnExit(firstPoolBlocker, secondPoolBlocker);
    loader.setMaxInFlightRegions(256);
    loader.setPrefetchRadius(1);
    loader.setPrefetchPerRequest(1);

    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::enqueueIoPoolJob(
        loader,
        [firstPoolBlocker]() { firstPoolBlocker->enterAndWait(); });
    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::enqueueIoPoolJob(
        loader,
        [secondPoolBlocker]() { secondPoolBlocker->enterAndWait(); });
    CHECK(firstPoolBlocker->waitUntilEntered());
    CHECK(secondPoolBlocker->waitUntilEntered());

    constexpr size_t kDemandCount = 128;
    std::vector<ChunkCoord> demands;
    demands.reserve(kDemandCount);
    for (size_t index = 0; index < kDemandCount; ++index) {
        const ChunkCoord coord{static_cast<int32_t>(index * 32), 0, 0};
        demands.push_back(coord);
        CHECK_EQ(loader.request(makeLoadRequest(coord)),
                 ChunkLoadRequestResult::Queued);
        if (index == 0) {
            loader.setPrefetchRadius(0);
        }
    }

    const auto saturated = loader.metrics();
    CHECK_EQ(saturated.directOrigin.logicalAdmissions,
             static_cast<uint64_t>(kDemandCount));
    CHECK_EQ(saturated.speculativeOrigin.logicalAdmissions, static_cast<uint64_t>(1));
    CHECK_EQ(saturated.demandOwnedDispatchedUndrained, static_cast<size_t>(2));
    CHECK_EQ(saturated.demandOwnedQueued, kDemandCount - 2);
    CHECK_EQ(saturated.speculativeOwnedQueued, static_cast<size_t>(1));
    CHECK_EQ(saturated.speculativePoolJobsPending, static_cast<size_t>(0));
    CHECK(saturated.maxSpeculativePoolJobsPending <=
          static_cast<size_t>(2));
    CHECK_EQ(saturated.speculativePoolYieldCalls,
             static_cast<uint64_t>(kDemandCount - 1));
    CHECK_EQ(saturated.speculativePoolYieldCandidateVisits,
             static_cast<uint64_t>(1));
    CHECK(saturated.maxSpeculativePoolYieldCandidateVisits <=
          static_cast<size_t>(2));
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionOwnerCount(loader),
             kDemandCount + 1);
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionAttemptOwnerCount(loader),
             static_cast<size_t>(2));

    for (ChunkCoord coord : demands) {
        loader.cancel(coord);
    }
    const auto cancelled = loader.metrics();
    CHECK_EQ(cancelled.directOrigin.logicalPreStartCancellations,
             static_cast<uint64_t>(kDemandCount));
    CHECK_EQ(cancelled.speculativeOwnedDispatchedUndrained,
             static_cast<size_t>(1));
    CHECK_EQ(cancelled.speculativePoolJobsPending,
             static_cast<size_t>(1));
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionOwnerCount(loader),
             static_cast<size_t>(1));
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionAttemptOwnerCount(loader),
             static_cast<size_t>(1));

    firstPoolBlocker->release();
    secondPoolBlocker->release();
    CHECK(waitForPublishedRegionJobs(loader, 1));
    std::vector<ChunkLoadCompletion> resolved;
    CHECK(drainRegionJobsUntilSettled(loader, resolved));
    CHECK(resolved.empty());
    CHECK_EQ(failingStorage->readAttempts(), static_cast<size_t>(1));

    const auto settled = loader.metrics();
    CHECK_EQ(settled.directOrigin.resultsPublished, static_cast<uint64_t>(0));
    CHECK_EQ(settled.directOrigin.logicalPreStartCancellations,
             settled.directOrigin.logicalAdmissions);
    CHECK_EQ(settled.speculativeOrigin.resultsPublished,
             settled.speculativeOrigin.logicalAdmissions);
    CHECK_EQ(settled.demandOwnedQueued, static_cast<size_t>(0));
    CHECK_EQ(settled.speculativeOwnedQueued, static_cast<size_t>(0));
    CHECK_EQ(settled.demandOwnedDispatchedUndrained, static_cast<size_t>(0));
    CHECK_EQ(settled.speculativeOwnedDispatchedUndrained, static_cast<size_t>(0));
    CHECK_EQ(settled.speculativePoolJobsPending, static_cast<size_t>(0));
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionOwnerCount(loader),
             static_cast<size_t>(0));
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionAttemptOwnerCount(loader),
             static_cast<size_t>(0));
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionRetryOwnerCount(loader),
             static_cast<size_t>(0));
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionCacheCount(loader),
             static_cast<size_t>(0));
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 regionCompletionCount(loader),
             static_cast<size_t>(0));
    CHECK_EQ(Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
                 payloadCompletionCount(loader),
             static_cast<size_t>(0));
    CHECK_EQ(loader.workCount().pending, static_cast<size_t>(0));
    CHECK_EQ(loader.workCount().inFlight, static_cast<size_t>(0));
}

TEST_CASE(AsyncChunkLoader_CancelDeferredDemandRestoresPrefetchPriority) {
    WorldResources resources;
    World world;
    world.initialize(resources);
    auto generator = makeGenerator(resources.registry());
    world.setGenerator(generator);

    MemoryContext ctx;
    auto firstStartGate = std::make_shared<LoaderWorkGate>();
    auto unusedPayloadGate = std::make_shared<LoaderWorkGate>();
    std::mutex startsMutex;
    std::vector<RegionKey> starts;
    AsyncChunkLoader loader(
        ctx.service,
        ctx.context,
        world,
        generator->config().world.version,
        1,
        0,
        12,
        generator);
    LoaderWorkRelease releaseOnExit(firstStartGate, unusedPayloadGate);
    loader.setMaxInFlightRegions(16);
    loader.setPrefetchRadius(1);
    loader.setPrefetchPerRequest(12);
    loader.setLoadQueueLimit(1);

    Rigel::Persistence::detail::AsyncChunkLoaderTestAccess::
        setRegionLoadStartObserver(
            loader,
            [firstStartGate, &startsMutex, &starts](
                const RegionKey& key,
                bool) {
                bool first = false;
                {
                    std::lock_guard<std::mutex> lock(startsMutex);
                    starts.push_back(key);
                    first = starts.size() == 1;
                }
                if (first) {
                    firstStartGate->enterAndWait();
                }
            });

    const ChunkCoord active{0, 0, 0};
    const ChunkCoord firstDeferred{16, 0, 0};
    const ChunkCoord secondDeferred{17, 0, 0};
    const ChunkCoord laterDirect{160, 0, 0};
    CHECK_EQ(loader.request(makeLoadRequest(active)),
             ChunkLoadRequestResult::Queued);
    CHECK(firstStartGate->waitUntilEntered());

    CHECK_EQ(loader.request(makeLoadRequest(firstDeferred)),
             ChunkLoadRequestResult::Deferred);
    CHECK_EQ(loader.request(makeLoadRequest(secondDeferred)),
             ChunkLoadRequestResult::Deferred);
    CHECK_EQ(loader.metrics().demandPromotions, static_cast<uint64_t>(1));
    CHECK_EQ(loader.metrics().demandOwnedQueued, static_cast<size_t>(1));
    CHECK_EQ(loader.metrics().speculativeOwnedQueued,
             static_cast<size_t>(11));

    loader.cancel(firstDeferred);
    CHECK_EQ(loader.metrics().demandOwnedQueued, static_cast<size_t>(1));
    CHECK_EQ(loader.metrics().speculativeOwnedQueued,
             static_cast<size_t>(11));

    loader.cancel(secondDeferred);
    CHECK_EQ(loader.metrics().demandOwnedQueued, static_cast<size_t>(0));
    CHECK_EQ(loader.metrics().speculativeOwnedQueued,
             static_cast<size_t>(12));
    CHECK_EQ(loader.workCount().pending, static_cast<size_t>(1));

    CHECK_EQ(loader.request(makeLoadRequest(laterDirect)),
             ChunkLoadRequestResult::Deferred);
    loader.setLoadQueueLimit(0);
    CHECK_EQ(loader.metrics().demandOwnedQueued, static_cast<size_t>(1));
    CHECK_EQ(loader.metrics().speculativeOwnedQueued,
             static_cast<size_t>(14));

    firstStartGate->release();
    CHECK(waitForPublishedRegionJobs(loader, 1));
    std::vector<ChunkLoadCompletion> resolved = loader.drainCompletions(1);
    CHECK_EQ(resolved.size(), static_cast<size_t>(1));
    CHECK(waitForPublishedRegionJobs(loader, 1));
    {
        std::lock_guard<std::mutex> lock(startsMutex);
        CHECK_EQ(starts.size(), static_cast<size_t>(2));
        CHECK_EQ(starts.front(), (RegionKey{"rigel:default", 0, 0, 0}));
        CHECK_EQ(starts.back(), (RegionKey{"rigel:default", 10, 0, 0}));
    }

    CHECK(drainRegionJobsUntilSettled(loader, resolved));
    CHECK_EQ(resolved.size(), static_cast<size_t>(2));
    const auto settled = loader.metrics();
    CHECK_EQ(settled.directOrigin.logicalAdmissions, settled.directOrigin.resultsPublished);
    CHECK_EQ(settled.speculativeOrigin.logicalAdmissions, settled.speculativeOrigin.resultsPublished);
    CHECK_EQ(settled.demandOwnedQueued, static_cast<size_t>(0));
    CHECK_EQ(settled.speculativeOwnedQueued, static_cast<size_t>(0));
    CHECK_EQ(settled.demandOwnedDispatchedUndrained, static_cast<size_t>(0));
    CHECK_EQ(settled.speculativeOwnedDispatchedUndrained, static_cast<size_t>(0));
    CHECK_EQ(loader.workCount().pending, static_cast<size_t>(0));
    CHECK_EQ(loader.workCount().inFlight, static_cast<size_t>(0));
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

TEST_CASE(AsyncChunkLoader_PartialSpan_RespectsDisabledBaseFillCapability) {
    WorldResources resources;
    World world;
    world.initialize(resources);
    auto& registry = resources.registry();

    auto generator = makeGenerator(registry);
    world.setGenerator(generator);

    const BlockID testA =
        registerTestBlock(registry, "rigel:test_partial_without_base_fill");
    const ChunkCoord coord{5, 0, 0};
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

    const ChunkData payload =
        buildPayload(coord, registry, {testA}, false, span, false);

    Rigel::Test::TemporaryDirectory directory("rigel_async_no_base_fill");
    FormatRegistry formats;
    FormatDescriptor descriptor = Backends::Memory::descriptor();
    descriptor.id = "memory-no-base-fill";
    descriptor.capabilities.fillMissingChunkSpans = false;
    formats.registerFormat(
        descriptor,
        [descriptor](const PersistenceContext& context) {
            return std::make_unique<CapabilityOverrideFormat>(
                descriptor, Backends::Memory::factory()(context));
        },
        [](StorageBackend& storage, const PersistenceContext& context) {
            if (!storage.exists(context.rootPath + "/world.meta")) {
                return std::optional<ProbeResult>();
            }
            return std::optional<ProbeResult>{ProbeResult{1.0f, true}};
        });
    PersistenceService service(formats);
    PersistenceContext context;
    context.rootPath = directory.path().string();
    context.preferredFormat = descriptor.id;
    context.storage = std::make_shared<FilesystemBackend>();
    auto settings = Rigel::Test::savedWorldSettingsFixture(
        "Disabled Base Fill Test World");
    settings.seed = loaderGeneratorDefinition().seed;
    Rigel::Test::installSavedWorldGenerationFixture(
        service,
        context,
        settings,
        loaderGeneratorDefinition());
    saveRegionForPayload(service, context, "rigel:default", coord, payload);

    AsyncChunkLoader loader(
        service,
        context,
        world,
        generator->config().world.version,
        0,
        0,
        1,
        generator);
    CHECK_EQ(loader.request(makeLoadRequest(coord)),
             ChunkLoadRequestResult::Queued);
    const auto completions = loader.drainCompletions(1);
    CHECK_EQ(completions.size(), static_cast<size_t>(1));
    CHECK_EQ(completions.front().outcome, ChunkLoadOutcome::Loaded);

    const Chunk* loaded = world.chunkManager().getChunk(coord);
    CHECK(loaded != nullptr);
    if (!loaded) {
        return;
    }
    CHECK_EQ(loaded->getBlock(0, 0, 0).id, testA);
    CHECK(loaded->getBlock(
                     Chunk::SIZE - 1,
                     Chunk::SIZE - 1,
                     Chunk::SIZE - 1)
              .isAir());
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

    auto regionGate = std::make_shared<LoaderWorkGate>();
    auto payloadGate = std::make_shared<LoaderWorkGate>();
    auto loader = std::make_unique<AsyncChunkLoader>(
        ctx.service,
        ctx.context,
        world,
        generator->config().world.version,
        1,
        1,
        4,
        generator);
    LoaderWorkRelease releaseWork(regionGate, payloadGate);
    loader->setPrefetchRadius(0);
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
