#include "TestFramework.h"

#include "Rigel/Persistence/AsyncChunkLoader.h"
#include "Rigel/Persistence/Backends/Memory/MemoryFormat.h"
#include "Rigel/Persistence/Backends/CR/CRChunkMapping.h"
#include "Rigel/Persistence/Backends/CR/CRFormat.h"
#include "Rigel/Persistence/ChunkSerializer.h"
#include "Rigel/Persistence/PersistenceService.h"
#include "Rigel/Persistence/Providers.h"
#include "Rigel/Persistence/Storage.h"
#include "Rigel/Voxel/ChunkStreamer.h"
#include "Rigel/Voxel/BlockType.h"
#include "Rigel/Voxel/MeshBuilder.h"
#include "Rigel/Voxel/World.h"
#include "Rigel/Voxel/WorldResources.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <thread>
#include <unordered_map>
#include <unordered_set>

using namespace Rigel::Voxel;

namespace Rigel::Voxel::detail {
struct ChunkStreamerTestAccess {
    static void setGenerationStartCallback(ChunkStreamer& streamer,
                                           std::function<void()> callback) {
        streamer.m_generationStartCallback = std::move(callback);
    }

    static void setMeshBuildStartCallback(ChunkStreamer& streamer,
                                          std::function<void()> callback) {
        streamer.m_meshBuildStartCallback = std::move(callback);
    }

    static size_t generationCompletionCount(const ChunkStreamer& streamer) {
        return streamer.m_genComplete.size();
    }

    static size_t meshCompletionCount(const ChunkStreamer& streamer) {
        return streamer.m_meshComplete.size();
    }

    static std::optional<size_t> pendingMeshIndexCount(ChunkStreamer& streamer) {
        ChunkStreamer::MeshResult result;
        if (!streamer.m_meshComplete.tryPop(result)) {
            return std::nullopt;
        }
        const size_t indexCount = result.mesh.indexCount();
        streamer.m_meshComplete.push(std::move(result));
        return indexCount;
    }

    static size_t inFlightMeshMissing(const ChunkStreamer& streamer) {
        return streamer.m_inFlightMeshMissing;
    }

    static size_t inFlightMeshDirty(const ChunkStreamer& streamer) {
        return streamer.m_inFlightMeshDirty;
    }

    static bool evictChunk(ChunkStreamer& streamer, ChunkCoord coord) {
        return streamer.evictChunk(coord);
    }

    static void reset(ChunkStreamer& streamer) {
        streamer.reset();
    }
};
}

namespace {
template<typename T>
concept HasPublicReset = requires(T& streamer) {
    streamer.reset();
};

static_assert(!HasPublicReset<ChunkStreamer>);

class WorkerGate {
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

class WorkerGateRelease {
public:
    explicit WorkerGateRelease(std::shared_ptr<WorkerGate> gate)
        : m_gate(std::move(gate)) {}

    ~WorkerGateRelease() {
        m_gate->release();
    }

private:
    std::shared_ptr<WorkerGate> m_gate;
};

bool waitForGenerationCompletion(const ChunkStreamer& streamer) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (Rigel::Voxel::detail::ChunkStreamerTestAccess::generationCompletionCount(
                streamer) > 0) {
            return true;
        }
        std::this_thread::yield();
    }
    return Rigel::Voxel::detail::ChunkStreamerTestAccess::generationCompletionCount(
        streamer) > 0;
}

bool waitForMeshCompletions(ChunkStreamer& streamer, uint64_t target) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        streamer.processCompletions();
        if (streamer.workMetrics().meshJobsCompleted >= target) {
            return true;
        }
        std::this_thread::yield();
    }
    streamer.processCompletions();
    return streamer.workMetrics().meshJobsCompleted >= target;
}

bool waitForPendingMeshCompletion(const ChunkStreamer& streamer) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (Rigel::Voxel::detail::ChunkStreamerTestAccess::meshCompletionCount(
                streamer) > 0) {
            return true;
        }
        std::this_thread::yield();
    }
    return Rigel::Voxel::detail::ChunkStreamerTestAccess::meshCompletionCount(
        streamer) > 0;
}

std::shared_ptr<WorldGenerator> makeGenerator(BlockRegistry& registry) {
    BlockType solid;
    solid.identifier = "rigel:stone";
    registry.registerBlock(solid.identifier, solid);

    BlockType surface;
    surface.identifier = "rigel:grass";
    registry.registerBlock(surface.identifier, surface);

    WorldGenConfig config;
    config.seed = 1;
    config.solidBlock = solid.identifier;
    config.surfaceBlock = surface.identifier;
    config.terrain.baseHeight = 0.0f;
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

BlockID registerTexturedTestBlock(BlockRegistry& registry,
                                  const std::string& identifier,
                                  const std::string& texture) {
    BlockType block;
    block.identifier = identifier;
    block.isOpaque = true;
    block.isSolid = true;
    block.textures = FaceTextures::uniform(texture);
    return registry.registerBlock(identifier, std::move(block));
}

void addTestTexture(TextureAtlas& atlas, const std::string& identifier) {
    std::array<unsigned char, 16 * 16 * 4> pixels{};
    pixels.fill(255);
    atlas.addTexture(identifier, pixels.data());
}

Rigel::Persistence::ChunkData buildPayload(ChunkCoord coord,
                                           BlockRegistry& registry,
                                           const std::vector<BlockID>& palette,
                                           bool random,
                                           std::optional<Rigel::Persistence::ChunkSpan> spanOpt,
                                           bool includeMetadata) {
    Chunk chunk(coord);
    Rigel::Persistence::ChunkSpan span;
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
        return Rigel::Persistence::serializeChunkSpan(chunk, span);
    }
    return Rigel::Persistence::serializeChunk(chunk);
}

void verifyPayloadMatches(const Chunk& chunk,
                          const Rigel::Persistence::ChunkData& payload) {
    Rigel::Persistence::ChunkData decoded;
    bool fullChunk =
        payload.span.offsetX == 0 &&
        payload.span.offsetY == 0 &&
        payload.span.offsetZ == 0 &&
        payload.span.sizeX == Chunk::SIZE &&
        payload.span.sizeY == Chunk::SIZE &&
        payload.span.sizeZ == Chunk::SIZE;
    if (fullChunk) {
        decoded = Rigel::Persistence::serializeChunk(chunk);
    } else {
        decoded = Rigel::Persistence::serializeChunkSpan(chunk, payload.span);
    }
    CHECK_EQ(decoded.span, payload.span);
    CHECK_EQ(decoded.blocks, payload.blocks);
}

struct PersistedChunkContext {
    Rigel::Test::TemporaryDirectory directory;
    Rigel::Persistence::FormatRegistry formats;
    Rigel::Persistence::PersistenceService service;
    Rigel::Persistence::PersistenceContext context;

    PersistedChunkContext()
        : directory("rigel_streamed_chunk"),
          service(formats) {
        formats.registerFormat(
            Rigel::Persistence::Backends::Memory::descriptor(),
            Rigel::Persistence::Backends::Memory::factory(),
            Rigel::Persistence::Backends::Memory::probe());
        context.rootPath = directory.path().string();
        context.preferredFormat = "memory";
        context.storage =
            std::make_shared<Rigel::Persistence::FilesystemBackend>();
    }

    void save(ChunkCoord coord,
              const Rigel::Persistence::ChunkData& payload) {
        auto format = service.openFormat(context);
        const auto regionKey = format->regionLayout().regionForChunk(
            "rigel:default", coord);
        Rigel::Persistence::ChunkRegionSnapshot region;
        region.key = regionKey;
        Rigel::Persistence::ChunkSnapshot snapshot;
        snapshot.key = {
            "rigel:default", coord.x, coord.y, coord.z};
        snapshot.data = payload;
        region.chunks.push_back(std::move(snapshot));
        format->chunkContainer().saveRegion(region);
    }
};

void configurePersistedChunkLoader(
    ChunkStreamer& streamer,
    const std::shared_ptr<Rigel::Persistence::AsyncChunkLoader>& loader) {
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

void addLoadedNeighborShell(ChunkManager& manager,
                            ChunkCoord center,
                            ChunkCoord omitted,
                            uint32_t worldGenVersion) {
    for (int i = 0; i < DirectionCount; ++i) {
        const Direction direction = static_cast<Direction>(i);
        int dx = 0;
        int dy = 0;
        int dz = 0;
        directionOffset(direction, dx, dy, dz);
        const ChunkCoord coord = center.offset(dx, dy, dz);
        if (coord == omitted) {
            continue;
        }
        Chunk& neighbor = manager.getOrCreateChunk(coord);
        neighbor.setWorldGenVersion(worldGenVersion);
        neighbor.setLoadedFromDisk(true);
        neighbor.clearPersistDirty();
        neighbor.clearDirty();
    }
}

bool meshesMatch(const ChunkMesh& lhs, const ChunkMesh& rhs) {
    if (lhs.vertices.size() != rhs.vertices.size() ||
        lhs.indices != rhs.indices) {
        return false;
    }

    for (size_t i = 0; i < lhs.vertices.size(); ++i) {
        const VoxelVertex& a = lhs.vertices[i];
        const VoxelVertex& b = rhs.vertices[i];
        if (a.x != b.x || a.y != b.y || a.z != b.z ||
            a.u != b.u || a.v != b.v ||
            a.normalIndex != b.normalIndex || a.aoLevel != b.aoLevel ||
            a.textureLayer != b.textureLayer || a.flags != b.flags) {
            return false;
        }
    }

    for (size_t i = 0; i < lhs.layers.size(); ++i) {
        if (lhs.layers[i].indexStart != rhs.layers[i].indexStart ||
            lhs.layers[i].indexCount != rhs.layers[i].indexCount) {
            return false;
        }
    }
    return true;
}

size_t installedMeshIndexCount(const WorldMeshStore& meshStore,
                               ChunkCoord coord) {
    size_t indexCount = 0;
    meshStore.forEach([&](const WorldMeshEntry& entry) {
        if (entry.coord == coord) {
            indexCount = entry.mesh.indexCount();
        }
    });
    return indexCount;
}

uint64_t installedMeshRevision(const WorldMeshStore& meshStore,
                               ChunkCoord coord) {
    uint64_t revision = 0;
    meshStore.forEach([&](const WorldMeshEntry& entry) {
        if (entry.coord == coord) {
            revision = entry.revision.value;
        }
    });
    return revision;
}
}

TEST_CASE(ChunkStreamer_GeneratesSphere) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);

    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 1;
    stream.unloadDistanceChunks = 1;
    stream.genQueueLimit = 0;
    stream.meshQueueLimit = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 0;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);

    streamer.update(glm::vec3(0.0f));
    streamer.processCompletions();
    CHECK_EQ(manager.loadedChunkCount(), static_cast<size_t>(7));
}

TEST_CASE(ChunkStreamer_RespectsQueueLimit) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);

    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 1;
    stream.unloadDistanceChunks = 1;
    stream.genQueueLimit = 2;
    stream.meshQueueLimit = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 0;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);

    streamer.update(glm::vec3(0.0f));
    streamer.processCompletions();
    CHECK_EQ(manager.loadedChunkCount(), static_cast<size_t>(2));
}

TEST_CASE(ChunkStreamer_UpdateBudget_DoesNotStarveOuterChunks) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);

    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 2;
    stream.unloadDistanceChunks = 2;
    stream.genQueueLimit = 0;
    stream.meshQueueLimit = 0;
    stream.updateBudgetPerFrame = 1;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 0;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);

    bool foundOuter = false;
    for (int frame = 0; frame < 128; ++frame) {
        streamer.update(glm::vec3(0.0f));
        streamer.processCompletions();
        if (manager.hasChunk({2, 0, 0})) {
            foundOuter = true;
            break;
        }
    }

    CHECK(foundOuter);
}

TEST_CASE(ChunkStreamer_EvictsOutsideRadius) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);

    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 0;
    stream.genQueueLimit = 0;
    stream.meshQueueLimit = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 0;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);

    streamer.update(glm::vec3(0.0f));
    streamer.processCompletions();
    CHECK_EQ(manager.loadedChunkCount(), static_cast<size_t>(1));

    streamer.update(glm::vec3(static_cast<float>(ChunkSize * 4), 0.0f, 0.0f));
    streamer.processCompletions();
    CHECK_EQ(manager.loadedChunkCount(), static_cast<size_t>(1));
}

TEST_CASE(ChunkStreamer_RetainsDirtyChunkWithoutEvictionPersistence) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    BlockID edited = registerTestBlock(registry, "rigel:eviction_edit");

    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 0;
    stream.genQueueLimit = 0;
    stream.meshQueueLimit = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 0;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);

    const ChunkCoord origin{0, 0, 0};
    streamer.update(origin.toWorldCenter());
    streamer.processCompletions();
    streamer.update(origin.toWorldCenter());
    Chunk* chunk = manager.getChunk(origin);
    CHECK(chunk != nullptr);
    if (!chunk) {
        return;
    }
    chunk->setBlock(0, 0, 0, BlockState{edited}, registry);
    CHECK(chunk->isPersistDirty());

    streamer.update(ChunkCoord{4, 0, 0}.toWorldCenter());

    CHECK(manager.hasChunk(origin));
    CHECK(manager.getChunk(origin)->isPersistDirty());
}

TEST_CASE(ChunkStreamer_RetriesFailedEvictionPersistenceAtBoundedIntervals) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    BlockID edited = registerTestBlock(registry, "rigel:retry_eviction_edit");

    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 0;
    stream.genQueueLimit = 0;
    stream.meshQueueLimit = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 0;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    streamer.markSpawnDiscoveryComplete();

    const ChunkCoord origin{0, 0, 0};
    streamer.update(origin.toWorldCenter());
    streamer.processCompletions();
    Chunk* chunk = manager.getChunk(origin);
    CHECK(chunk != nullptr);
    if (!chunk) {
        return;
    }
    chunk->setBlock(0, 0, 0, BlockState{edited}, registry);

    size_t persistenceAttempts = 0;
    streamer.setChunkEvictionCallback([&](ChunkCoord coord) {
        ++persistenceAttempts;
        if (persistenceAttempts == 1) {
            return false;
        }
        Chunk* saved = manager.getChunk(coord);
        if (saved) {
            saved->clearPersistDirty();
        }
        return true;
    });

    const glm::vec3 distant = ChunkCoord{4, 0, 0}.toWorldCenter();
    streamer.update(distant);
    streamer.processCompletions();
    CHECK(manager.hasChunk(origin));
    CHECK_EQ(persistenceAttempts, static_cast<size_t>(1));
    CHECK_EQ(streamer.diagnostics().eviction.pending, static_cast<size_t>(1));
    CHECK(streamer.diagnostics().eviction.lastError.find("eviction persistence") !=
          std::string::npos);
    CHECK(streamer.diagnostics().eviction.lastError.find("(0, 0, 0)") !=
          std::string::npos);

    for (int update = 0; update < 59; ++update) {
        streamer.update(distant);
        streamer.processCompletions();
        CHECK_EQ(streamer.diagnostics().state,
                 StreamingLifecycleState::Streaming);
        CHECK_EQ(streamer.diagnostics().eviction.pending,
                 static_cast<size_t>(1));
        CHECK_EQ(streamer.workMetrics().lastUpdateDesiredBuildCoordinatesInspected,
                 static_cast<uint64_t>(0));
    }
    CHECK(manager.hasChunk(origin));
    CHECK_EQ(persistenceAttempts, static_cast<size_t>(1));

    streamer.update(distant);
    CHECK(!manager.hasChunk(origin));
    CHECK_EQ(persistenceAttempts, static_cast<size_t>(2));
    CHECK_EQ(streamer.diagnostics().eviction.pending, static_cast<size_t>(0));
    streamer.processCompletions();

    for (uint32_t stable = 1;
         stable <= StreamingDiagnosticSnapshot::QuiescenceUpdateWindow;
         ++stable) {
        streamer.update(distant);
        streamer.processCompletions();
    }
    CHECK_EQ(streamer.diagnostics().state,
             StreamingLifecycleState::Quiescent);
}

TEST_CASE(ChunkStreamer_StreamingEventsRetireIneligibleEvictionRetry) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    BlockID edited = registerTestBlock(registry, "rigel:retired_eviction_edit");

    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 2;
    stream.genQueueLimit = 0;
    stream.meshQueueLimit = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 0;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    streamer.markSpawnDiscoveryComplete();

    const ChunkCoord origin{0, 0, 0};
    streamer.update(origin.toWorldCenter());
    streamer.processCompletions();
    Chunk* originChunk = manager.getChunk(origin);
    CHECK(originChunk != nullptr);
    if (!originChunk) {
        return;
    }
    originChunk->setBlock(0, 0, 0, BlockState{edited}, registry);

    size_t persistenceAttempts = 0;
    streamer.setChunkEvictionCallback([&](ChunkCoord request) {
        CHECK_EQ(request, origin);
        ++persistenceAttempts;
        return false;
    });

    const ChunkCoord outsideUnload{4, 0, 0};
    streamer.update(outsideUnload.toWorldCenter());
    streamer.processCompletions();
    CHECK(manager.hasChunk(origin));
    CHECK_EQ(persistenceAttempts, static_cast<size_t>(1));
    CHECK_EQ(streamer.diagnostics().eviction.pending, static_cast<size_t>(1));
    CHECK(!streamer.diagnostics().eviction.lastError.empty());

    const ChunkCoord insideUnloadOutsideView{1, 0, 0};
    streamer.update(insideUnloadOutsideView.toWorldCenter());
    streamer.processCompletions();
    CHECK(manager.hasChunk(origin));
    CHECK_EQ(persistenceAttempts, static_cast<size_t>(1));
    CHECK_EQ(streamer.diagnostics().eviction.pending, static_cast<size_t>(0));
    CHECK(streamer.diagnostics().eviction.lastError.empty());
    CHECK_EQ(
        streamer.workMetrics().lastUpdateDeferredEvictionCoordinatesInspected,
        static_cast<uint64_t>(1));

    for (uint32_t update = 0;
         update <= StreamingDiagnosticSnapshot::QuiescenceUpdateWindow;
         ++update) {
        streamer.update(insideUnloadOutsideView.toWorldCenter());
        streamer.processCompletions();
    }
    CHECK_EQ(streamer.diagnostics().state,
             StreamingLifecycleState::Quiescent);

    const uint64_t generationJobs = streamer.workMetrics().generationJobsStarted;
    const uint64_t meshJobs = streamer.workMetrics().meshJobsStarted;
    for (int update = 0; update < 60; ++update) {
        streamer.update(insideUnloadOutsideView.toWorldCenter());
        streamer.processCompletions();
        CHECK_EQ(
            streamer.workMetrics().lastUpdateDesiredBuildCoordinatesInspected,
            static_cast<uint64_t>(0));
        CHECK_EQ(streamer.workMetrics().lastUpdateSchedulerCoordinatesInspected,
                 static_cast<uint64_t>(0));
        CHECK_EQ(
            streamer.workMetrics().lastUpdateCacheEvictionCoordinatesInspected,
            static_cast<uint64_t>(0));
        CHECK_EQ(
            streamer.workMetrics().lastUpdateResidentEvictionCoordinatesInspected,
            static_cast<uint64_t>(0));
        CHECK_EQ(
            streamer.workMetrics().lastUpdateDeferredEvictionCoordinatesInspected,
            static_cast<uint64_t>(0));
    }
    CHECK_EQ(persistenceAttempts, static_cast<size_t>(1));
    CHECK_EQ(streamer.workMetrics().generationJobsStarted, generationJobs);
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, meshJobs);

    streamer.update(outsideUnload.toWorldCenter());
    streamer.processCompletions();
    CHECK_EQ(persistenceAttempts, static_cast<size_t>(2));
    CHECK_EQ(streamer.diagnostics().eviction.pending, static_cast<size_t>(1));

    const uint64_t eligibilityChecks =
        streamer.workMetrics().deferredEvictionCoordinatesInspected;
    stream.unloadDistanceChunks = 5;
    streamer.setConfig(stream);
    CHECK_EQ(persistenceAttempts, static_cast<size_t>(2));
    CHECK_EQ(streamer.diagnostics().eviction.pending, static_cast<size_t>(0));
    CHECK(streamer.diagnostics().eviction.lastError.empty());
    CHECK_EQ(streamer.workMetrics().deferredEvictionCoordinatesInspected,
             eligibilityChecks + 1);
}

TEST_CASE(ChunkStreamer_CachePressureRetainsChunkWhenPersistenceDefers) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    BlockID edited = registerTestBlock(registry, "rigel:cache_eviction_edit");

    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 8;
    stream.genQueueLimit = 0;
    stream.meshQueueLimit = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 0;
    stream.maxResidentChunks = 1;
    streamer.setConfig(stream);

    const ChunkCoord origin{0, 0, 0};
    streamer.update(origin.toWorldCenter());
    streamer.processCompletions();
    streamer.update(origin.toWorldCenter());
    Chunk* chunk = manager.getChunk(origin);
    CHECK(chunk != nullptr);
    if (!chunk) {
        return;
    }
    chunk->setBlock(0, 0, 0, BlockState{edited}, registry);

    size_t persistenceAttempts = 0;
    streamer.setChunkEvictionCallback([&](ChunkCoord) {
        ++persistenceAttempts;
        return false;
    });

    const ChunkCoord neighbor{1, 0, 0};
    streamer.update(neighbor.toWorldCenter());
    streamer.processCompletions();
    streamer.update(neighbor.toWorldCenter());

    CHECK(manager.hasChunk(origin));
    CHECK(manager.getChunk(origin)->isPersistDirty());
    CHECK_EQ(persistenceAttempts, static_cast<size_t>(1));
    for (int update = 0; update < 5; ++update) {
        streamer.update(neighbor.toWorldCenter());
    }
    CHECK_EQ(persistenceAttempts, static_cast<size_t>(1));
}

TEST_CASE(ChunkStreamer_CachePressureDeferralRemainsNonQuiescentUntilRetry) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    BlockID edited = registerTestBlock(registry, "rigel:cache_retry_edit");

    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 8;
    stream.genQueueLimit = 0;
    stream.meshQueueLimit = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 0;
    stream.maxResidentChunks = 1;
    streamer.setConfig(stream);
    streamer.markSpawnDiscoveryComplete();

    size_t persistenceAttempts = 0;
    streamer.setChunkEvictionCallback([&](ChunkCoord) {
        ++persistenceAttempts;
        return false;
    });

    const ChunkCoord first{0, 0, 0};
    streamer.update(first.toWorldCenter());
    streamer.processCompletions();
    streamer.update(first.toWorldCenter());
    Chunk* firstChunk = manager.getChunk(first);
    CHECK(firstChunk != nullptr);
    if (!firstChunk) {
        return;
    }
    firstChunk->setBlock(0, 0, 0, BlockState{edited}, registry);

    const ChunkCoord second{1, 0, 0};
    streamer.update(second.toWorldCenter());
    streamer.processCompletions();
    streamer.update(second.toWorldCenter());
    Chunk* secondChunk = manager.getChunk(second);
    CHECK(secondChunk != nullptr);
    if (!secondChunk) {
        return;
    }
    secondChunk->setBlock(0, 0, 0, BlockState{edited}, registry);

    const ChunkCoord current{2, 0, 0};
    streamer.update(current.toWorldCenter());
    streamer.processCompletions();
    streamer.update(current.toWorldCenter());

    CHECK_EQ(persistenceAttempts, static_cast<size_t>(2));
    CHECK(manager.hasChunk(first));
    CHECK(manager.hasChunk(second));
    CHECK(manager.getChunk(first)->isPersistDirty());
    CHECK(manager.getChunk(second)->isPersistDirty());
    CHECK(streamer.workMetrics().lastUpdateCacheEvictionCoordinatesInspected > 0);

    const uint64_t settledInspections =
        streamer.workMetrics().cacheEvictionCoordinatesInspected;
    for (int update = 0; update < 10; ++update) {
        streamer.update(current.toWorldCenter());
        streamer.processCompletions();
        CHECK_EQ(streamer.diagnostics().state,
                 StreamingLifecycleState::Streaming);
        CHECK_EQ(streamer.diagnostics().eviction.pending,
                 static_cast<size_t>(2));
        CHECK_EQ(
            streamer.workMetrics().lastUpdateCacheEvictionCoordinatesInspected,
            static_cast<uint64_t>(0));
        CHECK_EQ(streamer.workMetrics().lastUpdateDesiredBuildCoordinatesInspected,
                 static_cast<uint64_t>(0));
        CHECK_EQ(streamer.workMetrics().lastUpdateSchedulerCoordinatesInspected,
                 static_cast<uint64_t>(0));
    }
    CHECK_EQ(persistenceAttempts, static_cast<size_t>(2));
    CHECK_EQ(streamer.workMetrics().cacheEvictionCoordinatesInspected,
             settledInspections);
}

TEST_CASE(ChunkStreamer_VersionReplacementStaysPendingThroughGeneration) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto originalGenerator = makeGenerator(registry);
    WorldGenConfig replacementConfig = originalGenerator->config();
    ++replacementConfig.world.version;
    auto replacementGenerator =
        std::make_shared<WorldGenerator>(registry, replacementConfig);
    BlockID edited =
        registerTestBlock(registry, "rigel:replacement_persistence_edit");

    const ChunkCoord coord{0, 0, 0};
    Chunk& original = manager.getOrCreateChunk(coord);
    original.setBlock(0, 0, 0, BlockState{edited}, registry);
    original.setWorldGenVersion(originalGenerator->config().world.version);
    original.setLoadedFromDisk(true);

    ChunkStreamer streamer(
        manager, meshStore, registry, nullptr, originalGenerator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 2;
    stream.genQueueLimit = 1;
    stream.meshQueueLimit = 0;
    stream.updateBudgetPerFrame = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 0;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    streamer.markSpawnDiscoveryComplete();

    size_t persistenceAttempts = 0;
    streamer.setChunkEvictionCallback([&](ChunkCoord request) {
        CHECK_EQ(request, coord);
        ++persistenceAttempts;
        if (persistenceAttempts == 1) {
            return false;
        }
        Chunk* chunk = manager.getChunk(request);
        CHECK(chunk != nullptr);
        if (chunk) {
            chunk->clearPersistDirty();
        }
        return true;
    });

    streamer.setGenerator(replacementGenerator);
    streamer.update(coord.toWorldCenter());
    streamer.processCompletions();
    CHECK_EQ(persistenceAttempts, static_cast<size_t>(1));
    CHECK_EQ(streamer.diagnostics().eviction.pending, static_cast<size_t>(1));
    CHECK_EQ(streamer.diagnostics().state, StreamingLifecycleState::Streaming);

    const ChunkCoord outsideView{1, 0, 0};
    streamer.update(outsideView.toWorldCenter());
    streamer.processCompletions();
    CHECK_EQ(streamer.diagnostics().eviction.pending, static_cast<size_t>(1));
    CHECK_EQ(persistenceAttempts, static_cast<size_t>(1));

    streamer.update(outsideView.toWorldCenter());
    streamer.processCompletions();
    CHECK_EQ(streamer.diagnostics().eviction.pending, static_cast<size_t>(1));
    CHECK_EQ(persistenceAttempts, static_cast<size_t>(1));

    for (int update = 0; update < 57; ++update) {
        streamer.update(outsideView.toWorldCenter());
        streamer.processCompletions();
        CHECK_EQ(streamer.diagnostics().eviction.pending,
                 static_cast<size_t>(1));
        CHECK_EQ(streamer.diagnostics().state,
                 StreamingLifecycleState::Streaming);
        CHECK_EQ(streamer.workMetrics().lastUpdateDesiredBuildCoordinatesInspected,
                 static_cast<uint64_t>(0));
        CHECK_EQ(streamer.workMetrics().lastUpdateSchedulerCoordinatesInspected,
                 static_cast<uint64_t>(0));
    }

    streamer.update(outsideView.toWorldCenter());
    CHECK_EQ(persistenceAttempts, static_cast<size_t>(2));
    CHECK(!manager.hasChunk(coord));
    CHECK_EQ(streamer.diagnostics().generation.inFlight, static_cast<size_t>(0));
    CHECK_EQ(streamer.diagnostics().eviction.pending, static_cast<size_t>(1));
    CHECK(streamer.diagnostics().eviction.lastError.empty());

    streamer.update(coord.toWorldCenter());
    CHECK_EQ(streamer.diagnostics().generation.inFlight, static_cast<size_t>(1));
    CHECK_EQ(streamer.diagnostics().eviction.pending, static_cast<size_t>(1));

    streamer.processCompletions();
    CHECK(manager.hasChunk(coord));
    CHECK_EQ(manager.getChunk(coord)->worldGenVersion(),
             replacementGenerator->config().world.version);
    CHECK_EQ(streamer.diagnostics().generation.inFlight, static_cast<size_t>(0));
    CHECK_EQ(streamer.diagnostics().eviction.pending, static_cast<size_t>(0));
}

TEST_CASE(ChunkStreamer_OrdinaryEvictionPreservesVersionReplacement) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto originalGenerator = makeGenerator(registry);
    WorldGenConfig replacementConfig = originalGenerator->config();
    ++replacementConfig.world.version;
    auto replacementGenerator =
        std::make_shared<WorldGenerator>(registry, replacementConfig);
    BlockID edited =
        registerTestBlock(registry, "rigel:replacement_distance_eviction_edit");

    const ChunkCoord coord{0, 0, 0};
    Chunk& original = manager.getOrCreateChunk(coord);
    original.setBlock(0, 0, 0, BlockState{edited}, registry);
    original.setWorldGenVersion(originalGenerator->config().world.version);
    original.setLoadedFromDisk(true);

    ChunkStreamer streamer(
        manager, meshStore, registry, nullptr, originalGenerator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 2;
    stream.genQueueLimit = 1;
    stream.meshQueueLimit = 0;
    stream.updateBudgetPerFrame = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 0;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    streamer.markSpawnDiscoveryComplete();

    size_t persistenceAttempts = 0;
    streamer.setChunkEvictionCallback([&](ChunkCoord request) {
        CHECK_EQ(request, coord);
        ++persistenceAttempts;
        return false;
    });

    streamer.setGenerator(replacementGenerator);
    streamer.update(coord.toWorldCenter());
    streamer.processCompletions();
    CHECK(manager.hasChunk(coord));
    CHECK_EQ(persistenceAttempts, static_cast<size_t>(1));
    CHECK_EQ(streamer.diagnostics().eviction.pending, static_cast<size_t>(1));

    manager.getChunk(coord)->clearPersistDirty();
    const ChunkCoord outsideUnload{4, 0, 0};
    streamer.update(outsideUnload.toWorldCenter());
    streamer.processCompletions();
    CHECK(!manager.hasChunk(coord));
    CHECK_EQ(persistenceAttempts, static_cast<size_t>(1));
    CHECK_EQ(streamer.diagnostics().eviction.pending, static_cast<size_t>(1));
    CHECK(streamer.diagnostics().eviction.lastError.empty());

    for (int update = 0; update < 60; ++update) {
        streamer.update(outsideUnload.toWorldCenter());
        streamer.processCompletions();
        CHECK_EQ(streamer.diagnostics().eviction.pending,
                 static_cast<size_t>(1));
        CHECK_EQ(persistenceAttempts, static_cast<size_t>(1));
        CHECK_EQ(
            streamer.workMetrics().lastUpdateDeferredEvictionCoordinatesInspected,
            static_cast<uint64_t>(0));
    }

    streamer.update(coord.toWorldCenter());
    CHECK_EQ(streamer.diagnostics().generation.inFlight, static_cast<size_t>(1));
    CHECK_EQ(streamer.diagnostics().eviction.pending, static_cast<size_t>(1));

    streamer.processCompletions();
    Chunk* replacement = manager.getChunk(coord);
    CHECK(replacement != nullptr);
    if (!replacement) {
        return;
    }
    CHECK_EQ(replacement->worldGenVersion(), replacementConfig.world.version);
    CHECK_EQ(streamer.diagnostics().generation.inFlight, static_cast<size_t>(0));
    CHECK_EQ(streamer.diagnostics().eviction.pending, static_cast<size_t>(0));
}

TEST_CASE(ChunkStreamer_GeneratorReplacementRetainsDeferredEviction) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    BlockID edited =
        registerTestBlock(registry, "rigel:generator_replacement_eviction_edit");

    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 8;
    stream.genQueueLimit = 0;
    stream.meshQueueLimit = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 0;
    stream.maxResidentChunks = 1;
    streamer.setConfig(stream);

    const ChunkCoord first{0, 0, 0};
    streamer.update(first.toWorldCenter());
    streamer.processCompletions();
    streamer.update(first.toWorldCenter());
    Chunk* firstChunk = manager.getChunk(first);
    CHECK(firstChunk != nullptr);
    if (!firstChunk) {
        return;
    }
    firstChunk->setBlock(0, 0, 0, BlockState{edited}, registry);

    size_t persistenceAttempts = 0;
    streamer.setChunkEvictionCallback([&](ChunkCoord coord) {
        ++persistenceAttempts;
        if (persistenceAttempts == 1) {
            return false;
        }
        Chunk* saved = manager.getChunk(coord);
        if (saved) {
            saved->clearPersistDirty();
        }
        return true;
    });

    const ChunkCoord current{1, 0, 0};
    streamer.update(current.toWorldCenter());
    streamer.processCompletions();
    streamer.update(current.toWorldCenter());

    CHECK(manager.hasChunk(first));
    CHECK(manager.getChunk(first)->isPersistDirty());
    CHECK_EQ(persistenceAttempts, static_cast<size_t>(1));

    auto replacementGenerator =
        std::make_shared<WorldGenerator>(registry, generator->config());
    streamer.setGenerator(replacementGenerator);

    for (int update = 0; update < 59; ++update) {
        streamer.update(current.toWorldCenter());
    }
    CHECK(manager.hasChunk(first));
    CHECK_EQ(persistenceAttempts, static_cast<size_t>(1));

    streamer.update(current.toWorldCenter());
    CHECK(!manager.hasChunk(first));
    CHECK_EQ(persistenceAttempts, static_cast<size_t>(2));
}

TEST_CASE(ChunkStreamer_LoadsChunkPayload_Deterministic) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    BlockID testA = registerTestBlock(registry, "rigel:test_a");
    BlockID testB = registerTestBlock(registry, "rigel:test_b");
    std::vector<BlockID> palette = {BlockRegistry::airId(), testA, testB};

    ChunkCoord coord{2, 0, 0};
    Rigel::Persistence::ChunkData payload = buildPayload(coord, registry, palette, false, std::nullopt, true);

    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 0;
    stream.genQueueLimit = 0;
    stream.meshQueueLimit = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 0;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);

    streamer.setChunkLoader([&](ChunkLoadRequest loadRequest) {
        ChunkCoord request = loadRequest.coord;
        if (request != coord) {
            return ChunkLoadRequestResult::Missing;
        }
        Chunk& target = manager.getOrCreateChunk(request);
        Rigel::Persistence::applyChunkData(payload, target, registry);
        target.setWorldGenVersion(generator->config().world.version);
        target.clearPersistDirty();
        return ChunkLoadRequestResult::Queued;
    });

    streamer.update(coord.toWorldCenter());
    streamer.processCompletions();

    Chunk* loaded = manager.getChunk(coord);
    CHECK(loaded != nullptr);
    if (!loaded) {
        return;
    }
    verifyPayloadMatches(*loaded, payload);
}

TEST_CASE(ChunkStreamer_LoadsChunkPayload_Random) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    BlockID testA = registerTestBlock(registry, "rigel:test_random_a");
    BlockID testB = registerTestBlock(registry, "rigel:test_random_b");
    std::vector<BlockID> palette = {BlockRegistry::airId(), testA, testB};

    ChunkCoord coord{3, 0, 0};
    Rigel::Persistence::ChunkData payload = buildPayload(coord, registry, palette, true, std::nullopt, true);

    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 0;
    stream.genQueueLimit = 0;
    stream.meshQueueLimit = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 0;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);

    streamer.setChunkLoader([&](ChunkLoadRequest loadRequest) {
        ChunkCoord request = loadRequest.coord;
        if (request != coord) {
            return ChunkLoadRequestResult::Missing;
        }
        Chunk& target = manager.getOrCreateChunk(request);
        Rigel::Persistence::applyChunkData(payload, target, registry);
        target.setWorldGenVersion(generator->config().world.version);
        target.clearPersistDirty();
        return ChunkLoadRequestResult::Queued;
    });

    streamer.update(coord.toWorldCenter());
    streamer.processCompletions();

    Chunk* loaded = manager.getChunk(coord);
    CHECK(loaded != nullptr);
    if (!loaded) {
        return;
    }
    verifyPayloadMatches(*loaded, payload);
}

TEST_CASE(ChunkStreamer_LoadsEncodedChunkPayload_Deterministic) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    BlockID testA = registerTestBlock(registry, "rigel:test_encoded_a");
    BlockID testB = registerTestBlock(registry, "rigel:test_encoded_b");
    std::vector<BlockID> palette = {BlockRegistry::airId(), testA, testB};

    ChunkCoord coord{0, 0, 0};
    Rigel::Persistence::ChunkData payload = buildPayload(coord, registry, palette, false, std::nullopt, true);

    Rigel::Persistence::ChunkSnapshot snapshot;
    snapshot.key = Rigel::Persistence::ChunkKey{"zone-main", coord.x, coord.y, coord.z};
    snapshot.data = payload;

    Rigel::Persistence::FormatRegistry formats;
    formats.registerFormat(
        Rigel::Persistence::Backends::Memory::descriptor(),
        Rigel::Persistence::Backends::Memory::factory(),
        Rigel::Persistence::Backends::Memory::probe());
    Rigel::Persistence::PersistenceService service(formats);

    Rigel::Test::TemporaryDirectory directory("rigel_chunk_payload");

    auto storage = std::make_shared<Rigel::Persistence::FilesystemBackend>();
    Rigel::Persistence::PersistenceContext context;
    context.rootPath = directory.path().string();
    context.preferredFormat = "memory";
    context.storage = storage;

    auto format = service.openFormat(context);
    Rigel::Persistence::RegionKey regionKey =
        format->regionLayout().regionForChunk(snapshot.key.zoneId, coord);
    Rigel::Persistence::ChunkRegionSnapshot region;
    region.key = regionKey;
    region.chunks.push_back(snapshot);
    format->chunkContainer().saveRegion(region);

    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 0;
    stream.genQueueLimit = 0;
    stream.meshQueueLimit = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 0;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);

    streamer.setChunkLoader([&](ChunkLoadRequest loadRequest) {
        ChunkCoord request = loadRequest.coord;
        Rigel::Persistence::ChunkRegionSnapshot loaded = service.loadRegion(regionKey, context);
        for (const auto& chunk : loaded.chunks) {
            if (chunk.key.x == request.x &&
                chunk.key.y == request.y &&
                chunk.key.z == request.z) {
                Chunk& target = manager.getOrCreateChunk(request);
                Rigel::Persistence::applyChunkData(chunk.data, target, registry);
                target.setWorldGenVersion(generator->config().world.version);
                target.clearPersistDirty();
                return ChunkLoadRequestResult::Queued;
            }
        }
        return ChunkLoadRequestResult::Missing;
    });

    streamer.update(coord.toWorldCenter());
    streamer.processCompletions();

    Chunk* loaded = manager.getChunk(coord);
    CHECK(loaded != nullptr);
    if (!loaded) {
        return;
    }
    verifyPayloadMatches(*loaded, payload);
}

TEST_CASE(ChunkStreamer_LoadsEncodedChunkPayload_Random) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    BlockID testA = registerTestBlock(registry, "rigel:test_encoded_random_a");
    BlockID testB = registerTestBlock(registry, "rigel:test_encoded_random_b");
    std::vector<BlockID> palette = {BlockRegistry::airId(), testA, testB};

    ChunkCoord coord{1, 0, 0};
    Rigel::Persistence::ChunkData payload = buildPayload(coord, registry, palette, true, std::nullopt, true);

    Rigel::Persistence::ChunkSnapshot snapshot;
    snapshot.key = Rigel::Persistence::ChunkKey{"zone-main", coord.x, coord.y, coord.z};
    snapshot.data = payload;

    Rigel::Persistence::FormatRegistry formats;
    formats.registerFormat(
        Rigel::Persistence::Backends::Memory::descriptor(),
        Rigel::Persistence::Backends::Memory::factory(),
        Rigel::Persistence::Backends::Memory::probe());
    Rigel::Persistence::PersistenceService service(formats);

    Rigel::Test::TemporaryDirectory directory("rigel_chunk_payload_random");

    auto storage = std::make_shared<Rigel::Persistence::FilesystemBackend>();
    Rigel::Persistence::PersistenceContext context;
    context.rootPath = directory.path().string();
    context.preferredFormat = "memory";
    context.storage = storage;

    auto format = service.openFormat(context);
    Rigel::Persistence::RegionKey regionKey =
        format->regionLayout().regionForChunk(snapshot.key.zoneId, coord);
    Rigel::Persistence::ChunkRegionSnapshot region;
    region.key = regionKey;
    region.chunks.push_back(snapshot);
    format->chunkContainer().saveRegion(region);

    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 0;
    stream.genQueueLimit = 0;
    stream.meshQueueLimit = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 0;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);

    streamer.setChunkLoader([&](ChunkLoadRequest loadRequest) {
        ChunkCoord request = loadRequest.coord;
        Rigel::Persistence::ChunkRegionSnapshot loaded = service.loadRegion(regionKey, context);
        for (const auto& chunk : loaded.chunks) {
            if (chunk.key.x == request.x &&
                chunk.key.y == request.y &&
                chunk.key.z == request.z) {
                Chunk& target = manager.getOrCreateChunk(request);
                Rigel::Persistence::applyChunkData(chunk.data, target, registry);
                target.setWorldGenVersion(generator->config().world.version);
                target.clearPersistDirty();
                return ChunkLoadRequestResult::Queued;
            }
        }
        return ChunkLoadRequestResult::Missing;
    });

    streamer.update(coord.toWorldCenter());
    streamer.processCompletions();

    Chunk* loaded = manager.getChunk(coord);
    CHECK(loaded != nullptr);
    if (!loaded) {
        return;
    }
    verifyPayloadMatches(*loaded, payload);
}

TEST_CASE(ChunkStreamer_LoadsEncodedChunkPayload_CR_Deterministic) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    BlockID testA = registerTestBlock(registry, "base:test_cr_a");
    BlockID testB = registerTestBlock(registry, "base:test_cr_b");
    std::vector<BlockID> palette = {BlockRegistry::airId(), testA, testB};

    ChunkCoord coord{0, 0, 0};
    Rigel::Persistence::ChunkSpan span;
    span.chunkX = coord.x;
    span.chunkY = coord.y;
    span.chunkZ = coord.z;
    span.offsetX = 0;
    span.offsetY = 0;
    span.offsetZ = 0;
    span.sizeX = 16;
    span.sizeY = 16;
    span.sizeZ = 16;
    Rigel::Persistence::ChunkData sourcePayload = buildPayload(coord, registry, palette, false, span, false);

    Rigel::Persistence::ChunkSnapshot snapshot;
    auto crKey = Rigel::Persistence::Backends::CR::toCRChunk({coord.x, coord.y, coord.z, 0});
    snapshot.key = crKey;
    snapshot.key.zoneId = "zone-main";
    snapshot.data = sourcePayload;

    Rigel::Persistence::FormatRegistry formats;
    formats.registerFormat(
        Rigel::Persistence::Backends::CR::descriptor(),
        Rigel::Persistence::Backends::CR::factory(),
        Rigel::Persistence::Backends::CR::probe());
    Rigel::Persistence::PersistenceService service(formats);

    Rigel::Test::TemporaryDirectory directory("rigel_chunk_payload_cr");

    auto storage = std::make_shared<Rigel::Persistence::FilesystemBackend>();
    auto providers = std::make_shared<Rigel::Persistence::ProviderRegistry>();
    providers->add(
        Rigel::Persistence::kBlockRegistryProviderId,
        std::make_shared<Rigel::Persistence::BlockRegistryProvider>(&registry));

    Rigel::Persistence::PersistenceContext context;
    context.rootPath = directory.path().string();
    context.preferredFormat = "cr";
    context.storage = storage;
    context.providers = providers;

    auto format = service.openFormat(context);
    Rigel::Persistence::RegionKey regionKey =
        format->regionLayout().regionForChunk(snapshot.key.zoneId, coord);
    Rigel::Persistence::ChunkRegionSnapshot region;
    region.key = regionKey;
    region.chunks.push_back(snapshot);
    service.saveRegion(region, context);

    Rigel::Persistence::ChunkRegionSnapshot decodedRegion = service.loadRegion(regionKey, context);
    CHECK(!decodedRegion.chunks.empty());
    if (decodedRegion.chunks.empty()) {
        return;
    }
    Rigel::Persistence::ChunkData payload = decodedRegion.chunks.front().data;

    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 0;
    stream.genQueueLimit = 0;
    stream.meshQueueLimit = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 0;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);

    streamer.setChunkLoader([&](ChunkLoadRequest loadRequest) {
        ChunkCoord request = loadRequest.coord;
        if (request != coord) {
            return ChunkLoadRequestResult::Missing;
        }
        Chunk& target = manager.getOrCreateChunk(request);
        Rigel::Persistence::applyChunkData(payload, target, registry);
        target.setWorldGenVersion(generator->config().world.version);
        target.clearPersistDirty();
        return ChunkLoadRequestResult::Queued;
    });

    streamer.update(coord.toWorldCenter());
    streamer.processCompletions();

    Chunk* loaded = manager.getChunk(coord);
    CHECK(loaded != nullptr);
    if (!loaded) {
        return;
    }
    verifyPayloadMatches(*loaded, payload);
}

TEST_CASE(ChunkStreamer_LoadsEncodedChunkPayload_CR_Random) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    BlockID testA = registerTestBlock(registry, "base:test_cr_random_a");
    BlockID testB = registerTestBlock(registry, "base:test_cr_random_b");
    std::vector<BlockID> palette = {BlockRegistry::airId(), testA, testB};

    ChunkCoord coord{1, 0, 0};
    Rigel::Persistence::ChunkSpan span;
    span.chunkX = coord.x;
    span.chunkY = coord.y;
    span.chunkZ = coord.z;
    span.offsetX = 0;
    span.offsetY = 0;
    span.offsetZ = 0;
    span.sizeX = 16;
    span.sizeY = 16;
    span.sizeZ = 16;
    Rigel::Persistence::ChunkData sourcePayload = buildPayload(coord, registry, palette, true, span, false);

    Rigel::Persistence::ChunkSnapshot snapshot;
    auto crKey = Rigel::Persistence::Backends::CR::toCRChunk({coord.x, coord.y, coord.z, 0});
    snapshot.key = crKey;
    snapshot.key.zoneId = "zone-main";
    snapshot.data = sourcePayload;

    Rigel::Persistence::FormatRegistry formats;
    formats.registerFormat(
        Rigel::Persistence::Backends::CR::descriptor(),
        Rigel::Persistence::Backends::CR::factory(),
        Rigel::Persistence::Backends::CR::probe());
    Rigel::Persistence::PersistenceService service(formats);

    Rigel::Test::TemporaryDirectory directory("rigel_chunk_payload_cr_random");

    auto storage = std::make_shared<Rigel::Persistence::FilesystemBackend>();
    auto providers = std::make_shared<Rigel::Persistence::ProviderRegistry>();
    providers->add(
        Rigel::Persistence::kBlockRegistryProviderId,
        std::make_shared<Rigel::Persistence::BlockRegistryProvider>(&registry));

    Rigel::Persistence::PersistenceContext context;
    context.rootPath = directory.path().string();
    context.preferredFormat = "cr";
    context.storage = storage;
    context.providers = providers;

    auto format = service.openFormat(context);
    Rigel::Persistence::RegionKey regionKey =
        format->regionLayout().regionForChunk(snapshot.key.zoneId, coord);
    Rigel::Persistence::ChunkRegionSnapshot region;
    region.key = regionKey;
    region.chunks.push_back(snapshot);
    service.saveRegion(region, context);

    Rigel::Persistence::ChunkRegionSnapshot decodedRegion = service.loadRegion(regionKey, context);
    CHECK(!decodedRegion.chunks.empty());
    if (decodedRegion.chunks.empty()) {
        return;
    }
    Rigel::Persistence::ChunkData payload = decodedRegion.chunks.front().data;

    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 0;
    stream.genQueueLimit = 0;
    stream.meshQueueLimit = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 0;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);

    streamer.setChunkLoader([&](ChunkLoadRequest loadRequest) {
        ChunkCoord request = loadRequest.coord;
        if (request != coord) {
            return ChunkLoadRequestResult::Missing;
        }
        Chunk& target = manager.getOrCreateChunk(request);
        Rigel::Persistence::applyChunkData(payload, target, registry);
        target.setWorldGenVersion(generator->config().world.version);
        target.clearPersistDirty();
        return ChunkLoadRequestResult::Queued;
    });

    streamer.update(coord.toWorldCenter());
    streamer.processCompletions();

    Chunk* loaded = manager.getChunk(coord);
    CHECK(loaded != nullptr);
    if (!loaded) {
        return;
    }
    verifyPayloadMatches(*loaded, payload);
}

TEST_CASE(ChunkStreamer_WorkMetrics_CountGenerationAndSchedulerInspection) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);

    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 1;
    stream.unloadDistanceChunks = 1;
    stream.genQueueLimit = 0;
    stream.meshQueueLimit = 0;
    stream.updateBudgetPerFrame = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 0;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);

    streamer.update(glm::vec3(0.0f));

    const auto& metrics = streamer.workMetrics();
    CHECK_EQ(metrics.generationJobsStarted, static_cast<uint64_t>(7));
    CHECK_EQ(metrics.chunkLoadRequestsStarted, static_cast<uint64_t>(0));
    CHECK_EQ(metrics.meshJobsStarted, static_cast<uint64_t>(0));
    CHECK_EQ(metrics.desiredBuildCoordinatesInspected, static_cast<uint64_t>(27));
    CHECK_EQ(metrics.schedulerCoordinatesInspected, static_cast<uint64_t>(7));
    CHECK_EQ(metrics.lastUpdateDesiredBuildCoordinatesInspected, static_cast<size_t>(27));
    CHECK_EQ(metrics.lastUpdateSchedulerCoordinatesInspected, static_cast<size_t>(7));
}

TEST_CASE(ChunkStreamer_GenerationCapacityWaitsForCompletion) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);

    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 1;
    stream.unloadDistanceChunks = 1;
    stream.genQueueLimit = 1;
    stream.meshQueueLimit = 0;
    stream.updateBudgetPerFrame = 0;
    stream.applyBudgetPerFrame = 1;
    stream.workerThreads = 0;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);

    streamer.update(glm::vec3(0.0f));
    CHECK_EQ(streamer.workMetrics().generationJobsStarted, static_cast<uint64_t>(1));
    CHECK_EQ(streamer.diagnostics().generation.inFlight, static_cast<size_t>(1));
    CHECK_EQ(streamer.diagnostics().generation.pending, static_cast<size_t>(6));

    streamer.update(glm::vec3(0.0f));
    CHECK_EQ(streamer.workMetrics().lastUpdateSchedulerCoordinatesInspected,
             static_cast<uint64_t>(0));
    streamer.update(glm::vec3(0.0f));
    CHECK_EQ(streamer.workMetrics().lastUpdateSchedulerCoordinatesInspected,
             static_cast<uint64_t>(0));

    streamer.processCompletions();
    streamer.update(glm::vec3(0.0f));
    CHECK_EQ(streamer.workMetrics().generationJobsStarted, static_cast<uint64_t>(2));

    streamer.update(glm::vec3(0.0f));
    CHECK_EQ(streamer.workMetrics().lastUpdateSchedulerCoordinatesInspected,
             static_cast<uint64_t>(0));
}

TEST_CASE(ChunkStreamer_GenerationFailureCompletesJob) {
    for (int workerThreads : {0, 2}) {
        ChunkManager manager;
        BlockRegistry registry;
        WorldMeshStore meshStore;
        auto generator = makeGenerator(registry);

        ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
        StreamingConfig stream;
        stream.viewDistanceChunks = 0;
        stream.unloadDistanceChunks = 0;
        stream.genQueueLimit = 1;
        stream.meshQueueLimit = 0;
        stream.updateBudgetPerFrame = 0;
        stream.applyBudgetPerFrame = 0;
        stream.workerThreads = workerThreads;
        stream.maxResidentChunks = 0;
        streamer.setConfig(stream);
        streamer.markSpawnDiscoveryComplete();
        Rigel::Voxel::detail::ChunkStreamerTestAccess::setGenerationStartCallback(
            streamer,
            []() { throw std::runtime_error("injected generation failure"); });

        const ChunkCoord coord{0, 0, 0};
        streamer.update(coord.toWorldCenter());
        CHECK(waitForGenerationCompletion(streamer));
        streamer.processCompletions();

        CHECK_EQ(streamer.workMetrics().generationJobsStarted,
                 static_cast<uint64_t>(1));
        CHECK_EQ(streamer.workMetrics().generationJobsFailed,
                 static_cast<uint64_t>(1));
        CHECK_EQ(streamer.diagnostics().generation.inFlight,
                 static_cast<size_t>(0));
        CHECK_EQ(streamer.diagnostics().generation.pending,
                 static_cast<size_t>(0));
        CHECK_EQ(streamer.diagnostics().generation.terminalErrors,
                 static_cast<size_t>(1));
        CHECK(streamer.diagnostics().generation.lastError.find("generation") !=
              std::string::npos);
        CHECK(streamer.diagnostics().generation.lastError.find("(0, 0, 0)") !=
              std::string::npos);
        CHECK(!manager.hasChunk(coord));

        std::vector<ChunkStreamer::DebugChunkState> states;
        streamer.getDebugStates(states);
        CHECK_EQ(states.size(), static_cast<size_t>(1));
        CHECK_EQ(states.front().coord, coord);
        CHECK_EQ(states.front().state,
                 ChunkStreamer::DebugState::GenerationFailed);

        for (uint32_t update = 0;
             update <= StreamingDiagnosticSnapshot::QuiescenceUpdateWindow;
             ++update) {
            streamer.update(coord.toWorldCenter());
            streamer.processCompletions();
            CHECK_EQ(streamer.diagnostics().state,
                     StreamingLifecycleState::Streaming);
            CHECK_EQ(streamer.diagnostics().generation.terminalErrors,
                     static_cast<size_t>(1));
            CHECK_EQ(
                streamer.workMetrics().lastUpdateDesiredBuildCoordinatesInspected,
                static_cast<uint64_t>(0));
            CHECK_EQ(streamer.workMetrics().lastUpdateSchedulerCoordinatesInspected,
                     static_cast<uint64_t>(0));
        }
        CHECK_EQ(streamer.workMetrics().generationJobsStarted,
                 static_cast<uint64_t>(1));

        const ChunkCoord nextCoord{1, 0, 0};
        streamer.update(nextCoord.toWorldCenter());
        CHECK(waitForGenerationCompletion(streamer));
        streamer.processCompletions();

        CHECK_EQ(streamer.diagnostics().generation.inFlight,
                 static_cast<size_t>(0));
        CHECK_EQ(streamer.diagnostics().generation.pending,
                 static_cast<size_t>(0));
        CHECK_EQ(streamer.diagnostics().generation.terminalErrors,
                 static_cast<size_t>(1));
        CHECK(streamer.diagnostics().generation.lastError.find("(1, 0, 0)") !=
              std::string::npos);
        states.clear();
        streamer.getDebugStates(states);
        CHECK_EQ(states.size(), static_cast<size_t>(1));
        CHECK_EQ(states.front().coord, nextCoord);
        CHECK_EQ(states.front().state,
                 ChunkStreamer::DebugState::GenerationFailed);
    }
}

TEST_CASE(ChunkStreamer_ResetRetainsPreviousGenerationCapacity) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto originalGenerator = makeGenerator(registry);
    BlockID replacementBlock =
        registerTestBlock(registry, "rigel:replacement_generation_solid");

    WorldGenConfig replacementConfig = originalGenerator->config();
    ++replacementConfig.world.version;
    replacementConfig.solidBlock = "rigel:replacement_generation_solid";
    replacementConfig.surfaceBlock = "rigel:replacement_generation_solid";
    replacementConfig.terrain.baseHeight = 64.0f;
    auto replacementGenerator =
        std::make_shared<WorldGenerator>(registry, replacementConfig);

    ChunkStreamer streamer(
        manager, meshStore, registry, nullptr, originalGenerator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 0;
    stream.genQueueLimit = 1;
    stream.meshQueueLimit = 0;
    stream.updateBudgetPerFrame = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 4;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    streamer.markSpawnDiscoveryComplete();

    auto originalGate = std::make_shared<WorkerGate>();
    auto replacementGate = std::make_shared<WorkerGate>();
    WorkerGateRelease releaseOriginalOnExit(originalGate);
    WorkerGateRelease releaseReplacementOnExit(replacementGate);
    std::atomic<size_t> jobsEntered{0};
    Rigel::Voxel::detail::ChunkStreamerTestAccess::setGenerationStartCallback(
        streamer,
        [originalGate, replacementGate, &jobsEntered]() {
            size_t jobIndex = jobsEntered.fetch_add(1, std::memory_order_relaxed);
            if (jobIndex == 0) {
                originalGate->enterAndWait();
            } else if (jobIndex == 1) {
                replacementGate->enterAndWait();
            }
        });

    const ChunkCoord coord{0, 0, 0};
    streamer.update(coord.toWorldCenter());
    CHECK(originalGate->waitUntilEntered());
    CHECK_EQ(streamer.diagnostics().generation.inFlight, static_cast<size_t>(1));

    Rigel::Voxel::detail::ChunkStreamerTestAccess::reset(streamer);
    CHECK_EQ(streamer.workMetrics().generationJobsStarted, static_cast<uint64_t>(1));
    CHECK_EQ(streamer.diagnostics().generation.inFlight, static_cast<size_t>(1));
    streamer.setGenerator(replacementGenerator);
    streamer.update(coord.toWorldCenter());

    CHECK_EQ(jobsEntered.load(std::memory_order_relaxed), static_cast<size_t>(1));
    CHECK_EQ(streamer.workMetrics().generationJobsStarted, static_cast<uint64_t>(1));
    CHECK_EQ(streamer.diagnostics().generation.inFlight, static_cast<size_t>(1));
    CHECK_EQ(streamer.diagnostics().generation.pending, static_cast<size_t>(1));
    streamer.update(coord.toWorldCenter());
    CHECK_EQ(streamer.workMetrics().lastUpdateSchedulerCoordinatesInspected,
             static_cast<uint64_t>(0));
    CHECK_EQ(streamer.diagnostics().state, StreamingLifecycleState::Streaming);

    for (uint32_t i = 0;
         i < StreamingDiagnosticSnapshot::QuiescenceUpdateWindow;
         ++i) {
        streamer.processCompletions();
        streamer.update(coord.toWorldCenter());
    }
    CHECK_EQ(jobsEntered.load(std::memory_order_relaxed), static_cast<size_t>(1));
    CHECK_EQ(streamer.diagnostics().state, StreamingLifecycleState::Streaming);

    originalGate->release();
    CHECK(waitForGenerationCompletion(streamer));
    CHECK_EQ(
        Rigel::Voxel::detail::ChunkStreamerTestAccess::generationCompletionCount(
            streamer),
        static_cast<size_t>(1));
    streamer.processCompletions();

    CHECK_EQ(streamer.workMetrics().generationJobsStarted, static_cast<uint64_t>(1));
    CHECK_EQ(streamer.diagnostics().generation.inFlight, static_cast<size_t>(0));
    CHECK_EQ(streamer.diagnostics().generation.pending, static_cast<size_t>(1));
    CHECK_EQ(streamer.diagnostics().state, StreamingLifecycleState::Streaming);
    CHECK(!manager.hasChunk(coord));

    streamer.update(coord.toWorldCenter());
    CHECK(replacementGate->waitUntilEntered());
    CHECK_EQ(streamer.workMetrics().generationJobsStarted, static_cast<uint64_t>(2));
    CHECK_EQ(streamer.diagnostics().generation.inFlight, static_cast<size_t>(1));

    replacementGate->release();
    CHECK(waitForGenerationCompletion(streamer));
    CHECK_EQ(
        Rigel::Voxel::detail::ChunkStreamerTestAccess::generationCompletionCount(
            streamer),
        static_cast<size_t>(1));
    streamer.processCompletions();

    CHECK_EQ(jobsEntered.load(std::memory_order_relaxed), static_cast<size_t>(2));
    CHECK_EQ(streamer.workMetrics().generationJobsStarted, static_cast<uint64_t>(2));
    CHECK_EQ(streamer.diagnostics().generation.inFlight, static_cast<size_t>(0));
    CHECK_EQ(streamer.diagnostics().generation.pending, static_cast<size_t>(0));
    Chunk* accepted = manager.getChunk(coord);
    CHECK(accepted != nullptr);
    if (!accepted) {
        return;
    }
    CHECK_EQ(accepted->worldGenVersion(), replacementConfig.world.version);
    CHECK_EQ(accepted->getBlock(0, 0, 0).id, replacementBlock);
}

TEST_CASE(ChunkStreamer_SameVersionGeneratorReplacementSupersedesOutstandingGeneration) {
    ChunkManager manager;
    WorldMeshStore meshStore;
    BlockRegistry registry;
    TextureAtlas atlas;
    const std::string originalTexture =
        "textures/original_generator_solid.png";
    const std::string replacementTexture =
        "textures/replacement_generator_solid.png";
    addTestTexture(atlas, originalTexture);
    addTestTexture(atlas, replacementTexture);
    const TextureHandle originalTextureHandle =
        atlas.findTexture(originalTexture);
    const TextureHandle replacementTextureHandle =
        atlas.findTexture(replacementTexture);
    BlockID originalBlock =
        registerTexturedTestBlock(
            registry, "rigel:original_generator_solid", originalTexture);

    WorldGenConfig originalConfig;
    originalConfig.solidBlock = "rigel:original_generator_solid";
    originalConfig.surfaceBlock = "rigel:original_generator_solid";
    originalConfig.terrain.baseHeight = 64.0f;
    originalConfig.terrain.heightVariation = 0.0f;
    auto originalGenerator =
        std::make_shared<WorldGenerator>(registry, originalConfig);

    BlockID replacementBlock =
        registerTexturedTestBlock(
            registry, "rigel:replacement_generator_solid", replacementTexture);

    WorldGenConfig replacementConfig = originalConfig;
    replacementConfig.solidBlock = "rigel:replacement_generator_solid";
    replacementConfig.surfaceBlock = "rigel:replacement_generator_solid";
    replacementConfig.terrain.baseHeight = 0.0f;
    auto replacementGenerator =
        std::make_shared<WorldGenerator>(registry, replacementConfig);
    CHECK(originalGenerator != replacementGenerator);
    CHECK_EQ(originalConfig.world.version, replacementConfig.world.version);

    const ChunkCoord coord{0, 0, 0};
    ChunkBuffer originalBlocks;
    ChunkBuffer replacementBlocks;
    originalGenerator->generate(coord, originalBlocks);
    replacementGenerator->generate(coord, replacementBlocks);
    CHECK_NE(originalBlocks.blocks, replacementBlocks.blocks);
    CHECK(std::all_of(
        originalBlocks.blocks.begin(),
        originalBlocks.blocks.end(),
        [originalBlock](BlockState block) {
            return block.id == originalBlock;
        }));
    CHECK(std::any_of(
        replacementBlocks.blocks.begin(),
        replacementBlocks.blocks.end(),
        [replacementBlock](BlockState block) {
            return block.id == replacementBlock;
        }));
    CHECK(std::any_of(
        replacementBlocks.blocks.begin(),
        replacementBlocks.blocks.end(),
        [](BlockState block) { return block.isAir(); }));

    Chunk originalChunk(coord);
    originalChunk.copyFrom(originalBlocks.blocks, registry);
    Chunk replacementChunk(coord);
    replacementChunk.copyFrom(replacementBlocks.blocks, registry);
    MeshBuilder meshBuilder;
    ChunkMesh originalMesh = meshBuilder.build({
        .chunk = originalChunk,
        .registry = registry,
        .atlas = &atlas,
        .neighbors = {}
    });
    ChunkMesh replacementMesh = meshBuilder.build({
        .chunk = replacementChunk,
        .registry = registry,
        .atlas = &atlas,
        .neighbors = {}
    });
    CHECK_NE(originalMesh.indexCount(), replacementMesh.indexCount());

    ChunkStreamer streamer(
        manager, meshStore, registry, &atlas, originalGenerator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 1;
    stream.genQueueLimit = 1;
    stream.meshQueueLimit = 0;
    stream.updateBudgetPerFrame = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 4;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    streamer.markSpawnDiscoveryComplete();
    auto originalGate = std::make_shared<WorkerGate>();
    auto replacementGate = std::make_shared<WorkerGate>();
    WorkerGateRelease releaseOriginalOnExit(originalGate);
    WorkerGateRelease releaseReplacementOnExit(replacementGate);
    std::atomic<size_t> jobsEntered{0};
    Rigel::Voxel::detail::ChunkStreamerTestAccess::setGenerationStartCallback(
        streamer,
        [originalGate, replacementGate, &jobsEntered]() {
            size_t jobIndex = jobsEntered.fetch_add(1, std::memory_order_relaxed);
            if (jobIndex == 0) {
                originalGate->enterAndWait();
            } else if (jobIndex == 1) {
                replacementGate->enterAndWait();
            }
        });

    for (int index = 0; index < DirectionCount; ++index) {
        int dx = 0;
        int dy = 0;
        int dz = 0;
        directionOffset(static_cast<Direction>(index), dx, dy, dz);
        Chunk& neighbor = manager.getOrCreateChunk(coord.offset(dx, dy, dz));
        neighbor.setWorldGenVersion(replacementConfig.world.version);
        neighbor.clearDirty();
    }
    streamer.update(coord.toWorldCenter());
    CHECK(originalGate->waitUntilEntered());
    CHECK_EQ(streamer.diagnostics().generation.inFlight, static_cast<size_t>(1));

    streamer.setGenerator(replacementGenerator);
    originalGenerator.reset();
    streamer.update(coord.toWorldCenter());
    CHECK_EQ(jobsEntered.load(std::memory_order_relaxed), static_cast<size_t>(1));
    CHECK_EQ(streamer.workMetrics().generationJobsStarted, static_cast<uint64_t>(1));
    CHECK_EQ(streamer.diagnostics().generation.inFlight, static_cast<size_t>(1));
    CHECK_EQ(streamer.diagnostics().generation.pending, static_cast<size_t>(1));
    CHECK_EQ(streamer.workMetrics().generationJobsFailed, static_cast<uint64_t>(0));
    CHECK_EQ(streamer.workMetrics().meshJobsAccepted, static_cast<uint64_t>(0));
    CHECK_EQ(streamer.workMetrics().meshJobsFailed, static_cast<uint64_t>(0));
    CHECK_EQ(streamer.diagnostics().state, StreamingLifecycleState::Streaming);

    for (uint32_t update = 0;
         update < StreamingDiagnosticSnapshot::QuiescenceUpdateWindow;
         ++update) {
        streamer.processCompletions();
        streamer.update(coord.toWorldCenter());
        CHECK_EQ(streamer.workMetrics().lastUpdateDesiredBuildCoordinatesInspected,
                 static_cast<uint64_t>(0));
        CHECK_EQ(streamer.workMetrics().lastUpdateSchedulerCoordinatesInspected,
                 static_cast<uint64_t>(0));
        CHECK_EQ(streamer.diagnostics().generation.inFlight, static_cast<size_t>(1));
        CHECK_EQ(streamer.diagnostics().generation.pending, static_cast<size_t>(1));
        CHECK_EQ(streamer.diagnostics().state, StreamingLifecycleState::Streaming);
    }

    originalGate->release();
    CHECK(waitForGenerationCompletion(streamer));
    CHECK_EQ(streamer.diagnostics().generation.inFlight, static_cast<size_t>(1));
    CHECK_EQ(streamer.diagnostics().generation.pending, static_cast<size_t>(1));
    streamer.processCompletions();
    CHECK(!manager.hasChunk(coord));
    CHECK_EQ(streamer.diagnostics().generation.inFlight, static_cast<size_t>(0));
    CHECK_EQ(streamer.diagnostics().generation.pending, static_cast<size_t>(1));
    CHECK_EQ(streamer.workMetrics().generationJobsFailed, static_cast<uint64_t>(0));
    CHECK_EQ(streamer.workMetrics().meshJobsAccepted, static_cast<uint64_t>(0));
    CHECK_EQ(streamer.workMetrics().meshJobsFailed, static_cast<uint64_t>(0));
    CHECK_EQ(streamer.diagnostics().state, StreamingLifecycleState::Streaming);

    streamer.update(coord.toWorldCenter());
    CHECK(replacementGate->waitUntilEntered());
    CHECK_EQ(streamer.workMetrics().generationJobsStarted, static_cast<uint64_t>(2));
    CHECK_EQ(streamer.diagnostics().generation.inFlight, static_cast<size_t>(1));
    CHECK_EQ(streamer.diagnostics().generation.pending, static_cast<size_t>(0));
    CHECK_EQ(streamer.workMetrics().lastUpdateDesiredBuildCoordinatesInspected,
             static_cast<uint64_t>(0));
    CHECK_EQ(streamer.workMetrics().lastUpdateSchedulerCoordinatesInspected,
             static_cast<uint64_t>(1));
    CHECK_EQ(streamer.diagnostics().state, StreamingLifecycleState::Streaming);
    streamer.update(coord.toWorldCenter());
    CHECK_EQ(streamer.workMetrics().lastUpdateDesiredBuildCoordinatesInspected,
             static_cast<uint64_t>(0));
    CHECK_EQ(streamer.workMetrics().lastUpdateSchedulerCoordinatesInspected,
             static_cast<uint64_t>(0));
    CHECK_EQ(streamer.workMetrics().generationJobsStarted, static_cast<uint64_t>(2));
    replacementGate->release();
    CHECK(waitForGenerationCompletion(streamer));
    CHECK_EQ(streamer.diagnostics().generation.inFlight, static_cast<size_t>(1));
    streamer.processCompletions();
    CHECK_EQ(streamer.workMetrics().generationJobsFailed, static_cast<uint64_t>(0));
    CHECK_EQ(streamer.workMetrics().meshJobsAccepted, static_cast<uint64_t>(0));
    CHECK_EQ(streamer.workMetrics().meshJobsFailed, static_cast<uint64_t>(0));
    CHECK_EQ(streamer.diagnostics().state, StreamingLifecycleState::Streaming);

    Chunk* accepted = manager.getChunk(coord);
    CHECK(accepted != nullptr);
    if (!accepted) {
        return;
    }
    CHECK_EQ(accepted->worldGenVersion(), replacementConfig.world.version);
    ChunkBuffer acceptedBlocks;
    accepted->copyBlocks(acceptedBlocks.blocks);
    CHECK_EQ(acceptedBlocks.blocks, replacementBlocks.blocks);
    CHECK(std::none_of(
        acceptedBlocks.blocks.begin(),
        acceptedBlocks.blocks.end(),
        [originalBlock](BlockState block) {
            return block.id == originalBlock;
        }));

    streamer.update(coord.toWorldCenter());
    CHECK(waitForMeshCompletions(streamer, 1));
    CHECK_EQ(streamer.workMetrics().generationJobsStarted, static_cast<uint64_t>(2));
    CHECK_EQ(streamer.workMetrics().meshJobsAccepted, static_cast<uint64_t>(1));
    CHECK_EQ(streamer.workMetrics().meshJobsFailed, static_cast<uint64_t>(0));
    CHECK_EQ(streamer.workMetrics().meshJobsRejectedStale, static_cast<uint64_t>(0));

    bool foundMesh = false;
    meshStore.forEach([&](const WorldMeshEntry& entry) {
        if (entry.coord != coord) {
            return;
        }
        foundMesh = true;
        CHECK_EQ(entry.mesh.indexCount(), replacementMesh.indexCount());
        CHECK_NE(entry.mesh.indexCount(), originalMesh.indexCount());
        CHECK(!entry.mesh.vertices.empty());
        for (const VoxelVertex& vertex : entry.mesh.vertices) {
            CHECK_EQ(vertex.textureLayer,
                     static_cast<uint8_t>(replacementTextureHandle.index));
            CHECK(vertex.textureLayer !=
                  static_cast<uint8_t>(originalTextureHandle.index));
        }
    });
    CHECK(foundMesh);

    const uint64_t settledGenerationJobs =
        streamer.workMetrics().generationJobsStarted;
    const uint64_t settledMeshJobs = streamer.workMetrics().meshJobsStarted;

    for (uint32_t stable = 1;
         stable <= StreamingDiagnosticSnapshot::QuiescenceUpdateWindow;
         ++stable) {
        streamer.update(coord.toWorldCenter());
        CHECK_EQ(streamer.workMetrics().lastUpdateDesiredBuildCoordinatesInspected,
                 static_cast<uint64_t>(0));
        CHECK_EQ(streamer.workMetrics().lastUpdateSchedulerCoordinatesInspected,
                 static_cast<uint64_t>(0));
        CHECK_EQ(streamer.workMetrics().generationJobsStarted,
                 settledGenerationJobs);
        CHECK_EQ(streamer.workMetrics().meshJobsStarted, settledMeshJobs);
        streamer.processCompletions();
        CHECK_EQ(streamer.diagnostics().stableUpdates, stable);
    }
    CHECK_EQ(streamer.diagnostics().state, StreamingLifecycleState::Quiescent);
}

TEST_CASE(ChunkStreamer_MissingMeshCapacityWaitsForCompletion) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    BlockID solid = registerTestBlock(registry, "rigel:mesh_capacity_solid");

    const std::array<ChunkCoord, 7> desired{
        ChunkCoord{0, 0, 0},
        ChunkCoord{1, 0, 0},
        ChunkCoord{-1, 0, 0},
        ChunkCoord{0, 1, 0},
        ChunkCoord{0, -1, 0},
        ChunkCoord{0, 0, 1},
        ChunkCoord{0, 0, -1}
    };
    for (const ChunkCoord& coord : desired) {
        Chunk& chunk = manager.getOrCreateChunk(coord);
        chunk.setBlock(0, 0, 0, BlockState{solid}, registry);
        chunk.setWorldGenVersion(generator->config().world.version);
        chunk.setLoadedFromDisk(true);
    }

    auto gate = std::make_shared<WorkerGate>();
    std::atomic<size_t> buildsEntered{0};
    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 1;
    stream.unloadDistanceChunks = 1;
    stream.genQueueLimit = 0;
    stream.meshQueueLimit = 1;
    stream.updateBudgetPerFrame = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 2;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    Rigel::Voxel::detail::ChunkStreamerTestAccess::setMeshBuildStartCallback(
        streamer,
        [gate, &buildsEntered]() {
            if (buildsEntered.fetch_add(1, std::memory_order_relaxed) == 0) {
                gate->enterAndWait();
            }
        });
    WorkerGateRelease releaseOnExit(gate);

    streamer.update(glm::vec3(0.0f));
    bool firstBuildEntered = gate->waitUntilEntered();
    if (!firstBuildEntered) {
        gate->release();
    }
    CHECK(firstBuildEntered);
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(1));
    CHECK_EQ(streamer.diagnostics().mesh.inFlight, static_cast<size_t>(1));
    CHECK_EQ(streamer.diagnostics().mesh.pending, static_cast<size_t>(6));

    streamer.update(glm::vec3(0.0f));
    streamer.update(glm::vec3(0.0f));
    CHECK_EQ(streamer.workMetrics().lastUpdateSchedulerCoordinatesInspected,
             static_cast<uint64_t>(0));
    streamer.update(glm::vec3(0.0f));
    CHECK_EQ(streamer.workMetrics().lastUpdateSchedulerCoordinatesInspected,
             static_cast<uint64_t>(0));
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(1));

    gate->release();
    CHECK(waitForMeshCompletions(streamer, 1));
    streamer.update(glm::vec3(0.0f));
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(2));
}

TEST_CASE(ChunkStreamer_SingleMeshSlotAlternatesMissingAndDirtyWork) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    BlockID solid = registerTestBlock(registry, "rigel:single_mesh_slot_solid");

    const ChunkCoord firstMissing{0, 0, 0};
    const ChunkCoord secondMissing{1, 0, 0};
    const ChunkCoord dirtyCoord{-1, 0, 0};
    const std::array<ChunkCoord, 7> desired{
        firstMissing,
        secondMissing,
        dirtyCoord,
        ChunkCoord{0, 1, 0},
        ChunkCoord{0, -1, 0},
        ChunkCoord{0, 0, 1},
        ChunkCoord{0, 0, -1}
    };
    for (const ChunkCoord& coord : desired) {
        Chunk& chunk = manager.getOrCreateChunk(coord);
        chunk.setBlock(0, 0, 0, BlockState{solid}, registry);
        chunk.setWorldGenVersion(generator->config().world.version);
        chunk.setLoadedFromDisk(true);
        chunk.clearDirty();
        if (coord != firstMissing && coord != secondMissing) {
            meshStore.set(coord, {});
        }
    }
    Chunk& dirty = *manager.getChunk(dirtyCoord);
    dirty.invalidateMesh();

    auto gate = std::make_shared<WorkerGate>();
    std::atomic<size_t> buildsEntered{0};
    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 1;
    stream.unloadDistanceChunks = 1;
    stream.genQueueLimit = 0;
    stream.meshQueueLimit = 1;
    stream.updateBudgetPerFrame = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 2;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    Rigel::Voxel::detail::ChunkStreamerTestAccess::setMeshBuildStartCallback(
        streamer,
        [gate, &buildsEntered]() {
            if (buildsEntered.fetch_add(1, std::memory_order_relaxed) == 0) {
                gate->enterAndWait();
            }
        });
    WorkerGateRelease releaseOnExit(gate);

    streamer.update(glm::vec3(0.0f));
    bool firstBuildEntered = gate->waitUntilEntered();
    if (!firstBuildEntered) {
        gate->release();
    }
    CHECK(firstBuildEntered);
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(1));
    CHECK(dirty.isDirty());

    streamer.update(glm::vec3(0.0f));
    streamer.update(glm::vec3(0.0f));
    CHECK_EQ(streamer.workMetrics().lastUpdateDesiredBuildCoordinatesInspected,
             static_cast<uint64_t>(0));
    CHECK_EQ(streamer.workMetrics().lastUpdateSchedulerCoordinatesInspected,
             static_cast<uint64_t>(0));

    gate->release();
    CHECK(waitForMeshCompletions(streamer, 1));
    streamer.update(glm::vec3(0.0f));
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(2));
    CHECK(!dirty.isDirty());

    CHECK(waitForMeshCompletions(streamer, 2));
    CHECK_EQ(streamer.workMetrics().meshJobsAccepted, static_cast<uint64_t>(2));
    streamer.update(glm::vec3(0.0f));
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(3));
    CHECK(waitForMeshCompletions(streamer, 3));
    CHECK_EQ(streamer.workMetrics().meshJobsAccepted, static_cast<uint64_t>(3));
    CHECK(meshStore.contains(firstMissing));
    CHECK(meshStore.contains(secondMissing));

    streamer.update(glm::vec3(0.0f));
    streamer.update(glm::vec3(0.0f));
    CHECK_EQ(streamer.workMetrics().lastUpdateDesiredBuildCoordinatesInspected,
             static_cast<uint64_t>(0));
    CHECK_EQ(streamer.workMetrics().lastUpdateSchedulerCoordinatesInspected,
             static_cast<uint64_t>(0));
}

TEST_CASE(ChunkStreamer_DirtyMeshCapacityPreservesNearestFirstPriority) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    BlockID solid = registerTestBlock(registry, "rigel:dirty_capacity_solid");

    const std::array<ChunkCoord, 7> desired{
        ChunkCoord{0, 0, 0},
        ChunkCoord{1, 0, 0},
        ChunkCoord{-1, 0, 0},
        ChunkCoord{0, 1, 0},
        ChunkCoord{0, -1, 0},
        ChunkCoord{0, 0, 1},
        ChunkCoord{0, 0, -1}
    };
    for (const ChunkCoord& coord : desired) {
        Chunk& chunk = manager.getOrCreateChunk(coord);
        chunk.setBlock(0, 0, 0, BlockState{solid}, registry);
        chunk.setWorldGenVersion(generator->config().world.version);
        chunk.setLoadedFromDisk(true);
        chunk.clearDirty();
        meshStore.set(coord, {});
    }

    auto gate = std::make_shared<WorkerGate>();
    std::atomic<size_t> buildsEntered{0};
    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 1;
    stream.unloadDistanceChunks = 1;
    stream.genQueueLimit = 0;
    stream.meshQueueLimit = 2;
    stream.updateBudgetPerFrame = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 2;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    Rigel::Voxel::detail::ChunkStreamerTestAccess::setMeshBuildStartCallback(
        streamer,
        [gate, &buildsEntered]() {
            if (buildsEntered.fetch_add(1, std::memory_order_relaxed) == 0) {
                gate->enterAndWait();
            }
        });
    WorkerGateRelease releaseOnExit(gate);

    for (size_t update = 0; update <= desired.size(); ++update) {
        streamer.update(glm::vec3(0.0f));
    }
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(0));
    streamer.update(glm::vec3(0.0f));
    CHECK_EQ(streamer.workMetrics().lastUpdateSchedulerCoordinatesInspected,
             static_cast<uint64_t>(0));

    Chunk& nearest = *manager.getChunk({0, 0, 0});
    Chunk& farther = *manager.getChunk({1, 0, 0});
    farther.setBlock(1, 0, 0, BlockState{solid}, registry);
    nearest.setBlock(1, 0, 0, BlockState{solid}, registry);
    farther.invalidateMesh();
    nearest.invalidateMesh();

    streamer.update(glm::vec3(0.0f));
    bool firstBuildEntered = gate->waitUntilEntered();
    if (!firstBuildEntered) {
        gate->release();
    }
    CHECK(firstBuildEntered);
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(1));
    CHECK(!nearest.isDirty());
    CHECK(farther.isDirty());
    CHECK_EQ(streamer.workMetrics().lastUpdateSchedulerCoordinatesInspected,
             static_cast<uint64_t>(1));

    streamer.update(glm::vec3(0.0f));
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(1));
    CHECK_EQ(streamer.workMetrics().lastUpdateSchedulerCoordinatesInspected,
             static_cast<uint64_t>(0));

    gate->release();
    CHECK(waitForMeshCompletions(streamer, 1));
    streamer.update(glm::vec3(0.0f));
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(2));
    CHECK(!farther.isDirty());
    CHECK(waitForMeshCompletions(streamer, 2));
}

TEST_CASE(ChunkStreamer_ExplicitMeshPriorityPrecedesDistancePriority) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    BlockID solid = registerTestBlock(registry, "rigel:explicit_priority_solid");

    const ChunkCoord nearestCoord{0, 0, 0};
    const ChunkCoord prioritizedCoord{1, 0, 0};
    const std::array<ChunkCoord, 7> desired{
        nearestCoord,
        prioritizedCoord,
        ChunkCoord{-1, 0, 0},
        ChunkCoord{0, 1, 0},
        ChunkCoord{0, -1, 0},
        ChunkCoord{0, 0, 1},
        ChunkCoord{0, 0, -1}
    };
    for (const ChunkCoord& coord : desired) {
        Chunk& chunk = manager.getOrCreateChunk(coord);
        chunk.setWorldGenVersion(generator->config().world.version);
        chunk.setLoadedFromDisk(true);
        if (coord == nearestCoord || coord == prioritizedCoord) {
            chunk.setBlock(0, 0, 0, BlockState{solid}, registry);
            meshStore.set(coord, {});
        }
        chunk.clearDirty();
    }

    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 1;
    stream.unloadDistanceChunks = 1;
    stream.genQueueLimit = 0;
    stream.meshQueueLimit = 1;
    stream.updateBudgetPerFrame = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 0;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);

    streamer.update(glm::vec3(0.0f));
    streamer.processCompletions();
    const uint64_t jobsStarted = streamer.workMetrics().meshJobsStarted;

    Chunk& nearest = *manager.getChunk(nearestCoord);
    Chunk& prioritized = *manager.getChunk(prioritizedCoord);
    nearest.setBlock(1, 0, 0, BlockState{solid}, registry);
    prioritized.setBlock(1, 0, 0, BlockState{solid}, registry);
    streamer.prioritizeMesh(prioritizedCoord);
    streamer.prioritizeMesh(prioritizedCoord);

    streamer.update(glm::vec3(0.0f));
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, jobsStarted + 1);
    CHECK(nearest.isDirty());
    CHECK(!prioritized.isDirty());

    streamer.processCompletions();
    streamer.update(glm::vec3(0.0f));
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, jobsStarted + 2);
    CHECK(!nearest.isDirty());
    streamer.processCompletions();
    CHECK_EQ(streamer.workMetrics().meshJobsAccepted, jobsStarted + 2);
}

TEST_CASE(ChunkStreamer_ExplicitMeshPriorityPromotesPendingInitialMesh) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    BlockID solid = registerTestBlock(registry, "rigel:pending_priority_solid");

    const ChunkCoord ordinaryCoord{0, 0, 0};
    const ChunkCoord pendingCoord{1, 0, 0};
    const ChunkCoord initialMeshCoord{-1, 0, 0};
    const std::array<ChunkCoord, 7> desired{
        ordinaryCoord,
        pendingCoord,
        initialMeshCoord,
        ChunkCoord{0, 1, 0},
        ChunkCoord{0, -1, 0},
        ChunkCoord{0, 0, 1},
        ChunkCoord{0, 0, -1}
    };
    for (const ChunkCoord& coord : desired) {
        if (coord == pendingCoord) {
            continue;
        }
        Chunk& chunk = manager.getOrCreateChunk(coord);
        chunk.setWorldGenVersion(generator->config().world.version);
        chunk.setLoadedFromDisk(true);
        if (coord == ordinaryCoord || coord == initialMeshCoord) {
            chunk.setBlock(0, 0, 0, BlockState{solid}, registry);
        }
        chunk.clearDirty();
    }
    meshStore.set(ordinaryCoord, {});

    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 1;
    stream.unloadDistanceChunks = 1;
    stream.genQueueLimit = 0;
    stream.meshQueueLimit = 1;
    stream.updateBudgetPerFrame = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 0;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    streamer.setChunkLoader([&](ChunkLoadRequest request) {
        return request.coord == pendingCoord
            ? ChunkLoadRequestResult::Queued
            : ChunkLoadRequestResult::Missing;
    });

    streamer.update(glm::vec3(0.0f));
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(1));
    streamer.processCompletions();

    streamer.prioritizeMesh(pendingCoord);
    Chunk& pending = manager.getOrCreateChunk(pendingCoord);
    pending.setWorldGenVersion(generator->config().world.version);
    pending.setLoadedFromDisk(true);
    pending.setBlock(0, 0, 0, BlockState{solid}, registry);
    Chunk& ordinary = *manager.getChunk(ordinaryCoord);
    ordinary.setBlock(1, 0, 0, BlockState{solid}, registry);

    streamer.update(glm::vec3(0.0f));
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(2));
    CHECK(!pending.isDirty());
    CHECK(ordinary.isDirty());

    streamer.processCompletions();
    CHECK(meshStore.contains(pendingCoord));
    streamer.update(glm::vec3(0.0f));
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(3));
    CHECK(!ordinary.isDirty());
    streamer.processCompletions();
}

TEST_CASE(ChunkStreamer_ExplicitMeshPrioritySurvivesDependencyWait) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    BlockID solid = registerTestBlock(registry, "rigel:waiting_priority_solid");

    const ChunkCoord ordinaryCoord{0, 0, 0};
    const ChunkCoord prioritizedCoord{1, 0, 0};
    const ChunkCoord missingDependency{2, 0, 0};
    for (int z = -2; z <= 2; ++z) {
        for (int y = -2; y <= 2; ++y) {
            for (int x = -2; x <= 2; ++x) {
                if (x * x + y * y + z * z > 4) {
                    continue;
                }
                ChunkCoord coord{x, y, z};
                if (coord == missingDependency) {
                    continue;
                }
                Chunk& chunk = manager.getOrCreateChunk(coord);
                chunk.setWorldGenVersion(generator->config().world.version);
                chunk.setLoadedFromDisk(true);
                if (coord == ordinaryCoord || coord == prioritizedCoord) {
                    chunk.setBlock(0, 0, 0, BlockState{solid}, registry);
                    meshStore.set(coord, {});
                }
                chunk.clearDirty();
            }
        }
    }

    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 2;
    stream.unloadDistanceChunks = 2;
    stream.genQueueLimit = 1;
    stream.meshQueueLimit = 1;
    stream.updateBudgetPerFrame = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 0;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);

    streamer.update(glm::vec3(0.0f));
    CHECK_EQ(streamer.workMetrics().generationJobsStarted, static_cast<uint64_t>(1));
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(0));

    Chunk& ordinary = *manager.getChunk(ordinaryCoord);
    Chunk& prioritized = *manager.getChunk(prioritizedCoord);
    ordinary.setBlock(1, 0, 0, BlockState{solid}, registry);
    prioritized.setBlock(1, 0, 0, BlockState{solid}, registry);
    streamer.prioritizeMesh(prioritizedCoord);

    streamer.update(glm::vec3(0.0f));
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(1));
    CHECK(prioritized.isDirty());
    CHECK(!ordinary.isDirty());

    streamer.processCompletions();
    ordinary.setBlock(2, 0, 0, BlockState{solid}, registry);
    streamer.update(glm::vec3(0.0f));
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(2));
    CHECK(!prioritized.isDirty());
    CHECK(ordinary.isDirty());

    streamer.processCompletions();
    CHECK(meshStore.contains(prioritizedCoord));
}

TEST_CASE(ChunkStreamer_WorkMetrics_CoalescePendingLoadRequests) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);

    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
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

    size_t callbackCount = 0;
    std::vector<ChunkLoadRequestId> requestIds;
    streamer.setChunkLoader([&](ChunkLoadRequest request) {
        ++callbackCount;
        requestIds.push_back(request.requestId);
        return ChunkLoadRequestResult::Queued;
    });

    streamer.update(glm::vec3(0.0f));
    streamer.update(glm::vec3(0.0f));

    const auto& metrics = streamer.workMetrics();
    CHECK_EQ(callbackCount, static_cast<size_t>(2));
    CHECK_EQ(requestIds.front(), requestIds.back());
    CHECK_EQ(metrics.chunkLoadRequestsStarted, static_cast<uint64_t>(1));
    CHECK_EQ(metrics.desiredBuildCoordinatesInspected, static_cast<uint64_t>(1));
    CHECK_EQ(metrics.schedulerCoordinatesInspected, static_cast<uint64_t>(3));
    CHECK_EQ(metrics.lastUpdateDesiredBuildCoordinatesInspected, static_cast<size_t>(0));
    CHECK_EQ(metrics.lastUpdateSchedulerCoordinatesInspected, static_cast<size_t>(2));
}

TEST_CASE(ChunkStreamer_MissingLoadResolutionStartsGeneration) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);

    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
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

    const ChunkCoord coord{0, 0, 0};
    size_t loadAttempts = 0;
    ChunkLoadRequestId loadRequestId = 0;
    streamer.setChunkLoader([&](ChunkLoadRequest request) {
        CHECK_EQ(request.coord, coord);
        loadRequestId = request.requestId;
        return ++loadAttempts == 1
            ? ChunkLoadRequestResult::Queued
            : ChunkLoadRequestResult::Missing;
    });
    bool resolved = false;
    streamer.setChunkLoadDrain([&](size_t) {
        if (resolved) {
            return std::vector<ChunkLoadCompletion>{};
        }
        resolved = true;
        return std::vector<ChunkLoadCompletion>{
            {coord, loadRequestId, ChunkLoadOutcome::Missing}
        };
    });

    streamer.update(coord.toWorldCenter());
    CHECK_EQ(loadAttempts, static_cast<size_t>(1));
    CHECK_EQ(streamer.workMetrics().generationJobsStarted, static_cast<uint64_t>(0));

    streamer.processCompletions();
    streamer.update(coord.toWorldCenter());
    CHECK_EQ(loadAttempts, static_cast<size_t>(2));
    CHECK_EQ(streamer.workMetrics().generationJobsStarted, static_cast<uint64_t>(1));

    streamer.processCompletions();
    CHECK(manager.hasChunk(coord));
}

TEST_CASE(ChunkStreamer_FailedLoadResolutionDoesNotStartGeneration) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);

    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
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
    streamer.markSpawnDiscoveryComplete();

    const ChunkCoord coord{0, 0, 0};
    size_t loadAttempts = 0;
    ChunkLoadRequestId loadRequestId = 0;
    streamer.setChunkLoader([&](ChunkLoadRequest request) {
        CHECK_EQ(request.coord, coord);
        loadRequestId = request.requestId;
        ++loadAttempts;
        return ChunkLoadRequestResult::Queued;
    });
    bool resolved = false;
    streamer.setChunkLoadDrain([&](size_t) {
        if (resolved) {
            return std::vector<ChunkLoadCompletion>{};
        }
        resolved = true;
        return std::vector<ChunkLoadCompletion>{
            {coord,
             loadRequestId,
             ChunkLoadOutcome::Failed,
             "injected load failure"}
        };
    });

    streamer.update(coord.toWorldCenter());
    streamer.processCompletions();
    streamer.update(coord.toWorldCenter());

    CHECK_EQ(loadAttempts, static_cast<size_t>(1));
    CHECK_EQ(streamer.workMetrics().generationJobsStarted,
             static_cast<uint64_t>(0));
    CHECK(!manager.hasChunk(coord));
    CHECK_EQ(streamer.diagnostics().chunkLoad.terminalErrors,
             static_cast<size_t>(1));
    CHECK(streamer.diagnostics().chunkLoad.lastError.find("load") !=
          std::string::npos);
    CHECK(streamer.diagnostics().chunkLoad.lastError.find("(0, 0, 0)") !=
          std::string::npos);

    for (uint32_t update = 0;
         update <= StreamingDiagnosticSnapshot::QuiescenceUpdateWindow;
         ++update) {
        streamer.update(coord.toWorldCenter());
        streamer.processCompletions();
        CHECK_EQ(streamer.diagnostics().state,
                 StreamingLifecycleState::Streaming);
        CHECK_EQ(streamer.diagnostics().chunkLoad.terminalErrors,
                 static_cast<size_t>(1));
        CHECK_EQ(streamer.workMetrics().lastUpdateDesiredBuildCoordinatesInspected,
                 static_cast<uint64_t>(0));
        CHECK_EQ(streamer.workMetrics().lastUpdateSchedulerCoordinatesInspected,
                 static_cast<uint64_t>(0));
    }
    CHECK_EQ(loadAttempts, static_cast<size_t>(1));
    CHECK_EQ(streamer.workMetrics().generationJobsStarted,
             static_cast<uint64_t>(0));

    streamer.setChunkLoader([&](ChunkLoadRequest request) {
        CHECK_EQ(request.coord, coord);
        Chunk& loaded = manager.getOrCreateChunk(coord);
        loaded.setWorldGenVersion(generator->config().world.version);
        loaded.setLoadedFromDisk(true);
        return ChunkLoadRequestResult::Queued;
    });
    streamer.update(coord.toWorldCenter());
    CHECK_EQ(streamer.diagnostics().chunkLoad.terminalErrors,
             static_cast<size_t>(0));
    streamer.processCompletions();
    CHECK(manager.hasChunk(coord));
    CHECK(manager.getChunk(coord)->loadedFromDisk());
    CHECK_EQ(streamer.workMetrics().generationJobsStarted,
             static_cast<uint64_t>(0));
}

TEST_CASE(ChunkStreamer_LateFailedLoadCannotReplaceActiveRequest) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);

    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
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
    streamer.markSpawnDiscoveryComplete();

    const ChunkCoord coord{0, 0, 0};
    const ChunkCoord away{4, 0, 0};
    std::vector<ChunkLoadRequest> requests;
    std::vector<ChunkLoadCompletion> completions;
    size_t cancellations = 0;
    streamer.setChunkLoader([&](ChunkLoadRequest request) {
        if (request.coord != coord) {
            return ChunkLoadRequestResult::Missing;
        }
        if (requests.empty() ||
            requests.back().requestId != request.requestId) {
            requests.push_back(request);
        }
        return ChunkLoadRequestResult::Queued;
    });
    streamer.setChunkLoadDrain([&](size_t) {
        std::vector<ChunkLoadCompletion> drained = std::move(completions);
        completions.clear();
        return drained;
    });
    streamer.setChunkLoadCancel([&](ChunkCoord cancelled) {
        if (cancelled == coord) {
            ++cancellations;
        }
    });

    streamer.update(coord.toWorldCenter());
    CHECK_EQ(requests.size(), static_cast<size_t>(1));
    const ChunkLoadRequest firstRequest = requests.front();

    streamer.update(away.toWorldCenter());
    CHECK_EQ(cancellations, static_cast<size_t>(1));

    streamer.update(coord.toWorldCenter());
    CHECK_EQ(requests.size(), static_cast<size_t>(2));
    const ChunkLoadRequest replacementRequest = requests.back();
    CHECK_NE(firstRequest.requestId, replacementRequest.requestId);

    completions.push_back({firstRequest.coord,
                           firstRequest.requestId,
                           ChunkLoadOutcome::Failed,
                           "late load failure"});
    streamer.processCompletions();
    CHECK_EQ(streamer.diagnostics().chunkLoad.pending, static_cast<size_t>(1));
    CHECK_EQ(streamer.diagnostics().chunkLoad.terminalErrors,
             static_cast<size_t>(0));
    CHECK(streamer.diagnostics().chunkLoad.lastError.empty());

    for (uint32_t update = 0;
         update <= StreamingDiagnosticSnapshot::QuiescenceUpdateWindow;
         ++update) {
        streamer.update(coord.toWorldCenter());
        streamer.processCompletions();
        CHECK_EQ(streamer.diagnostics().state,
                 StreamingLifecycleState::Streaming);
        CHECK_EQ(streamer.diagnostics().chunkLoad.pending,
                 static_cast<size_t>(1));
        CHECK_EQ(streamer.diagnostics().chunkLoad.terminalErrors,
                 static_cast<size_t>(0));
        CHECK_EQ(streamer.workMetrics().lastUpdateDesiredBuildCoordinatesInspected,
                 static_cast<uint64_t>(0));
        CHECK_EQ(streamer.workMetrics().lastUpdateSchedulerCoordinatesInspected,
                 static_cast<uint64_t>(0));
    }

    Chunk& loaded = manager.getOrCreateChunk(coord);
    loaded.setWorldGenVersion(generator->config().world.version);
    loaded.setLoadedFromDisk(true);
    completions.push_back({replacementRequest.coord,
                           replacementRequest.requestId,
                           ChunkLoadOutcome::Loaded});
    streamer.processCompletions();
    streamer.update(coord.toWorldCenter());

    CHECK_EQ(streamer.diagnostics().chunkLoad.pending, static_cast<size_t>(0));
    CHECK_EQ(streamer.diagnostics().chunkLoad.terminalErrors,
             static_cast<size_t>(0));
    CHECK(streamer.diagnostics().chunkLoad.lastError.empty());
    CHECK(manager.hasChunk(coord));
}

TEST_CASE(ChunkStreamer_MovementRequestsOnlyNewDesiredFrontier) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);

    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 2;
    stream.unloadDistanceChunks = 2;
    stream.genQueueLimit = 0;
    stream.meshQueueLimit = 0;
    stream.updateBudgetPerFrame = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 0;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);

    std::unordered_map<ChunkCoord, size_t, ChunkCoordHash> requestCounts;
    std::unordered_set<ChunkCoord, ChunkCoordHash> cancelled;
    streamer.setChunkLoader([&](ChunkLoadRequest request) {
        ++requestCounts[request.coord];
        return ChunkLoadRequestResult::Queued;
    });
    streamer.setChunkPendingCallback([](ChunkCoord) { return true; });
    streamer.setChunkLoadDrain([](size_t) {
        return std::vector<ChunkLoadCompletion>{};
    });
    streamer.setChunkLoadCancel([&](ChunkCoord coord) {
        cancelled.insert(coord);
    });

    auto desiredSet = [](ChunkCoord center, int radius) {
        std::unordered_set<ChunkCoord, ChunkCoordHash> result;
        int radiusSq = radius * radius;
        for (int dz = -radius; dz <= radius; ++dz) {
            for (int dy = -radius; dy <= radius; ++dy) {
                for (int dx = -radius; dx <= radius; ++dx) {
                    ChunkCoord coord = center.offset(dx, dy, dz);
                    if (dx * dx + dy * dy + dz * dz <= radiusSq) {
                        result.insert(coord);
                    }
                }
            }
        }
        return result;
    };

    const ChunkCoord firstCenter{0, 0, 0};
    const ChunkCoord secondCenter{1, 0, 0};
    const auto firstDesired = desiredSet(firstCenter, stream.viewDistanceChunks);
    const auto secondDesired = desiredSet(secondCenter, stream.viewDistanceChunks);

    streamer.update(firstCenter.toWorldCenter());
    CHECK_EQ(requestCounts.size(), firstDesired.size());
    for (const ChunkCoord& coord : firstDesired) {
        CHECK_EQ(requestCounts[coord], static_cast<size_t>(1));
    }

    streamer.update(secondCenter.toWorldCenter());

    size_t frontierSize = 0;
    for (const ChunkCoord& coord : secondDesired) {
        if (firstDesired.find(coord) == firstDesired.end()) {
            ++frontierSize;
        }
        CHECK_EQ(requestCounts[coord], static_cast<size_t>(1));
    }
    CHECK_EQ(requestCounts.size(), firstDesired.size() + frontierSize);
    CHECK_EQ(streamer.workMetrics().lastUpdateSchedulerCoordinatesInspected,
             static_cast<uint64_t>(frontierSize));

    size_t departedSize = 0;
    for (const ChunkCoord& coord : firstDesired) {
        if (secondDesired.find(coord) != secondDesired.end()) {
            continue;
        }
        ++departedSize;
        CHECK(cancelled.find(coord) != cancelled.end());
    }
    CHECK_EQ(cancelled.size(), departedSize);
}

TEST_CASE(ChunkStreamer_MovementCancelsDepartedGeneration) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);

    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
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

    const ChunkCoord departed{0, 0, 0};
    const ChunkCoord desired{4, 0, 0};
    streamer.update(departed.toWorldCenter());
    CHECK_EQ(streamer.workMetrics().generationJobsStarted, static_cast<uint64_t>(1));

    streamer.update(desired.toWorldCenter());
    CHECK_EQ(streamer.workMetrics().generationJobsStarted, static_cast<uint64_t>(2));
    streamer.processCompletions();

    CHECK(!manager.hasChunk(departed));
    CHECK(manager.hasChunk(desired));
}

TEST_CASE(ChunkStreamer_DepartedFrontierReleasesWaitingMesh) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    BlockID solid = registerTestBlock(registry, "rigel:frontier_solid");

    const ChunkCoord firstCenter{0, 0, 0};
    const ChunkCoord secondCenter{1, 0, 0};
    Chunk& waiting = manager.getOrCreateChunk(firstCenter);
    waiting.setBlock(0, 0, 0, BlockState{solid}, registry);
    waiting.setWorldGenVersion(generator->config().world.version);
    waiting.setLoadedFromDisk(true);
    Chunk& sharedNeighbor = manager.getOrCreateChunk(secondCenter);
    sharedNeighbor.setWorldGenVersion(generator->config().world.version);
    sharedNeighbor.setLoadedFromDisk(true);

    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 1;
    stream.unloadDistanceChunks = 1;
    stream.genQueueLimit = 0;
    stream.meshQueueLimit = 0;
    stream.updateBudgetPerFrame = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 0;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    streamer.setChunkLoader([](ChunkLoadRequest) {
        return ChunkLoadRequestResult::Queued;
    });
    streamer.setChunkLoadDrain([](size_t) {
        return std::vector<ChunkLoadCompletion>{};
    });

    streamer.update(firstCenter.toWorldCenter());
    streamer.processCompletions();
    CHECK(!meshStore.contains(firstCenter));
    CHECK_EQ(streamer.diagnostics().mesh.pending, static_cast<size_t>(1));

    streamer.update(secondCenter.toWorldCenter());
    streamer.processCompletions();
    CHECK(meshStore.contains(firstCenter));
    CHECK_EQ(streamer.diagnostics().mesh.pending, static_cast<size_t>(0));
}

TEST_CASE(ChunkStreamer_WorkMetrics_TrackMeshLifecycleAndInvalidation) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    BlockID solid = registerTestBlock(registry, "rigel:metrics_solid");

    Chunk& chunk = manager.getOrCreateChunk({0, 0, 0});
    chunk.setBlock(0, 0, 0, BlockState{solid}, registry);
    chunk.setWorldGenVersion(generator->config().world.version);
    chunk.setLoadedFromDisk(true);

    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
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

    streamer.update(glm::vec3(0.0f));
    streamer.processCompletions();

    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(1));
    CHECK_EQ(streamer.workMetrics().meshJobsCompleted, static_cast<uint64_t>(1));
    CHECK_EQ(streamer.workMetrics().meshJobsAccepted, static_cast<uint64_t>(1));
    CHECK_EQ(streamer.workMetrics().meshJobsRejectedStale, static_cast<uint64_t>(0));

    chunk.setBlock(1, 0, 0, BlockState{solid}, registry);
    streamer.update(glm::vec3(0.0f));
    CHECK_EQ(streamer.workMetrics().meshInvalidations, static_cast<uint64_t>(1));
    streamer.processCompletions();

    chunk.setBlock(2, 0, 0, BlockState{solid}, registry);
    streamer.update(glm::vec3(0.0f));
    chunk.setBlock(3, 0, 0, BlockState{solid}, registry);
    streamer.processCompletions();

    const auto& metrics = streamer.workMetrics();
    CHECK_EQ(metrics.meshJobsStarted, static_cast<uint64_t>(3));
    CHECK_EQ(metrics.meshJobsCompleted, static_cast<uint64_t>(3));
    CHECK_EQ(metrics.meshJobsAccepted, static_cast<uint64_t>(2));
    CHECK_EQ(metrics.meshJobsRejectedStale, static_cast<uint64_t>(1));
    CHECK_EQ(metrics.meshInvalidations, static_cast<uint64_t>(3));
    CHECK_EQ(metrics.meshRequestsCoalesced, static_cast<uint64_t>(1));

    streamer.update(glm::vec3(0.0f));
    CHECK_EQ(metrics.meshJobsStarted, static_cast<uint64_t>(4));
    CHECK_EQ(metrics.meshInvalidations, static_cast<uint64_t>(3));
    streamer.processCompletions();

    CHECK_EQ(metrics.meshJobsCompleted, static_cast<uint64_t>(4));
    CHECK_EQ(metrics.meshJobsAccepted, static_cast<uint64_t>(3));
    CHECK_EQ(metrics.meshJobsRejectedStale, static_cast<uint64_t>(1));
    CHECK_EQ(metrics.meshInvalidations, static_cast<uint64_t>(3));
    CHECK_EQ(metrics.meshRequestsCoalesced, static_cast<uint64_t>(1));
}

TEST_CASE(ChunkStreamer_MeshFailureCompletesJob) {
    for (int workerThreads : {0, 2}) {
        ChunkManager manager;
        BlockRegistry registry;
        WorldMeshStore meshStore;
        auto generator = makeGenerator(registry);
        BlockID solid = registerTestBlock(registry, "rigel:mesh_failure_solid");

        const ChunkCoord coord{0, 0, 0};
        Chunk& chunk = manager.getOrCreateChunk(coord);
        chunk.setBlock(0, 0, 0, BlockState{solid}, registry);
        chunk.setWorldGenVersion(generator->config().world.version);
        chunk.setLoadedFromDisk(true);

        ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
        StreamingConfig stream;
        stream.viewDistanceChunks = 0;
        stream.unloadDistanceChunks = 0;
        stream.genQueueLimit = 0;
        stream.meshQueueLimit = 1;
        stream.updateBudgetPerFrame = 0;
        stream.applyBudgetPerFrame = 0;
        stream.workerThreads = workerThreads;
        stream.maxResidentChunks = 0;
        streamer.setConfig(stream);
        streamer.markSpawnDiscoveryComplete();
        Rigel::Voxel::detail::ChunkStreamerTestAccess::setMeshBuildStartCallback(
            streamer,
            []() { throw std::runtime_error("injected mesh failure"); });

        streamer.update(coord.toWorldCenter());
        CHECK(waitForMeshCompletions(streamer, 1));

        CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(1));
        CHECK_EQ(streamer.workMetrics().meshJobsCompleted, static_cast<uint64_t>(1));
        CHECK_EQ(streamer.workMetrics().meshJobsAccepted, static_cast<uint64_t>(0));
        CHECK_EQ(streamer.workMetrics().meshJobsFailed, static_cast<uint64_t>(1));
        CHECK_EQ(streamer.diagnostics().mesh.inFlight, static_cast<size_t>(0));
        CHECK_EQ(streamer.diagnostics().mesh.pending, static_cast<size_t>(0));
        CHECK_EQ(streamer.diagnostics().mesh.terminalErrors,
                 static_cast<size_t>(1));
        CHECK(streamer.diagnostics().mesh.lastError.find("mesh build") !=
              std::string::npos);
        CHECK(streamer.diagnostics().mesh.lastError.find("(0, 0, 0)") !=
              std::string::npos);
        CHECK(!meshStore.contains(coord));

        std::vector<ChunkStreamer::DebugChunkState> states;
        streamer.getDebugStates(states);
        CHECK_EQ(states.size(), static_cast<size_t>(1));
        CHECK_EQ(states.front().coord, coord);
        CHECK_EQ(states.front().state, ChunkStreamer::DebugState::MeshFailed);

        for (uint32_t update = 0;
             update <= StreamingDiagnosticSnapshot::QuiescenceUpdateWindow;
             ++update) {
            streamer.update(coord.toWorldCenter());
            streamer.processCompletions();
            CHECK_EQ(streamer.diagnostics().state,
                     StreamingLifecycleState::Streaming);
            CHECK_EQ(streamer.diagnostics().mesh.terminalErrors,
                     static_cast<size_t>(1));
            CHECK_EQ(
                streamer.workMetrics().lastUpdateDesiredBuildCoordinatesInspected,
                static_cast<uint64_t>(0));
            CHECK_EQ(streamer.workMetrics().lastUpdateSchedulerCoordinatesInspected,
                     static_cast<uint64_t>(0));
        }
        CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(1));

        Rigel::Voxel::detail::ChunkStreamerTestAccess::setMeshBuildStartCallback(
            streamer,
            {});
        auto replacementGenerator =
            std::make_shared<WorldGenerator>(registry, generator->config());
        streamer.setGenerator(replacementGenerator);
        streamer.update(coord.toWorldCenter());
        CHECK(waitForMeshCompletions(streamer, 2));
        CHECK_EQ(streamer.diagnostics().mesh.terminalErrors,
                 static_cast<size_t>(0));
        CHECK(meshStore.contains(coord));
    }
}

TEST_CASE(ChunkStreamer_DirtyMeshFailureSurvivesResidentDesiredReentry) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    BlockID solid = registerTestBlock(
        registry, "rigel:resident_dirty_mesh_failure_solid");

    const ChunkCoord coord{0, 0, 0};
    Chunk& chunk = manager.getOrCreateChunk(coord);
    chunk.setBlock(0, 0, 0, BlockState{solid}, registry);
    chunk.setWorldGenVersion(generator->config().world.version);
    chunk.setLoadedFromDisk(true);

    const ChunkCoord departureCoord{1, 0, 0};
    Chunk& departure = manager.getOrCreateChunk(departureCoord);
    departure.setWorldGenVersion(generator->config().world.version);
    departure.setLoadedFromDisk(true);
    departure.clearDirty();

    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 2;
    stream.genQueueLimit = 0;
    stream.meshQueueLimit = 1;
    stream.updateBudgetPerFrame = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 0;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    streamer.markSpawnDiscoveryComplete();

    streamer.update(coord.toWorldCenter());
    streamer.processCompletions();
    CHECK(meshStore.contains(coord));
    CHECK(!chunk.isDirty());
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(1));
    CHECK_EQ(streamer.workMetrics().meshJobsAccepted, static_cast<uint64_t>(1));
    ChunkMesh oldGeometry;
    meshStore.forEach([&](const WorldMeshEntry& entry) {
        if (entry.coord == coord) {
            oldGeometry = entry.mesh;
        }
    });
    CHECK(!oldGeometry.isEmpty());
    const uint64_t oldMeshVersion = meshStore.version();

    Rigel::Voxel::detail::ChunkStreamerTestAccess::setMeshBuildStartCallback(
        streamer,
        []() { throw std::runtime_error("injected dirty mesh failure"); });
    chunk.setBlock(1, 0, 0, BlockState{solid}, registry);
    streamer.update(coord.toWorldCenter());
    streamer.processCompletions();

    CHECK(meshStore.contains(coord));
    CHECK(chunk.isDirty());
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(2));
    CHECK_EQ(streamer.workMetrics().meshJobsFailed, static_cast<uint64_t>(1));
    CHECK_EQ(streamer.diagnostics().mesh.terminalErrors, static_cast<size_t>(1));
    CHECK_EQ(meshStore.version(), oldMeshVersion);
    ChunkMesh retainedGeometry;
    meshStore.forEach([&](const WorldMeshEntry& entry) {
        if (entry.coord == coord) {
            retainedGeometry = entry.mesh;
        }
    });
    CHECK(meshesMatch(retainedGeometry, oldGeometry));

    streamer.update(departureCoord.toWorldCenter());
    streamer.processCompletions();
    CHECK(manager.getChunk(coord) != nullptr);
    CHECK(meshStore.contains(coord));
    CHECK_EQ(streamer.diagnostics().mesh.terminalErrors, static_cast<size_t>(1));

    for (uint32_t update = 0;
         update <= StreamingDiagnosticSnapshot::QuiescenceUpdateWindow;
         ++update) {
        streamer.update(departureCoord.toWorldCenter());
        streamer.processCompletions();
        CHECK_EQ(streamer.diagnostics().state,
                 StreamingLifecycleState::Streaming);
        CHECK_EQ(streamer.diagnostics().mesh.terminalErrors,
                 static_cast<size_t>(1));
        CHECK_EQ(streamer.workMetrics().lastUpdateSchedulerCoordinatesInspected,
                 static_cast<uint64_t>(0));
    }

    streamer.update(coord.toWorldCenter());
    streamer.processCompletions();
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(3));
    CHECK_EQ(streamer.workMetrics().meshJobsFailed, static_cast<uint64_t>(2));
    CHECK_EQ(streamer.diagnostics().mesh.terminalErrors, static_cast<size_t>(1));
    CHECK(streamer.diagnostics().mesh.lastError.find("mesh build") !=
          std::string::npos);
    CHECK(streamer.diagnostics().mesh.lastError.find("(0, 0, 0)") !=
          std::string::npos);

    for (uint32_t update = 0;
         update <= StreamingDiagnosticSnapshot::QuiescenceUpdateWindow;
         ++update) {
        streamer.update(coord.toWorldCenter());
        streamer.processCompletions();
        CHECK_EQ(streamer.diagnostics().state,
                 StreamingLifecycleState::Streaming);
        CHECK_EQ(streamer.diagnostics().mesh.terminalErrors,
                 static_cast<size_t>(1));
        CHECK_EQ(
            streamer.workMetrics().lastUpdateDesiredBuildCoordinatesInspected,
            static_cast<uint64_t>(0));
        CHECK_EQ(streamer.workMetrics().lastUpdateSchedulerCoordinatesInspected,
                 static_cast<uint64_t>(0));
        CHECK_EQ(
            streamer.workMetrics().lastUpdateCacheEvictionCoordinatesInspected,
            static_cast<uint64_t>(0));
        CHECK_EQ(
            streamer.workMetrics().lastUpdateResidentEvictionCoordinatesInspected,
            static_cast<uint64_t>(0));
        CHECK_EQ(
            streamer.workMetrics().lastUpdateDeferredEvictionCoordinatesInspected,
            static_cast<uint64_t>(0));
    }
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(3));

    Rigel::Voxel::detail::ChunkStreamerTestAccess::setMeshBuildStartCallback(
        streamer,
        {});
    auto replacementGenerator =
        std::make_shared<WorldGenerator>(registry, generator->config());
    streamer.setGenerator(replacementGenerator);
    streamer.update(coord.toWorldCenter());
    streamer.processCompletions();

    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(4));
    CHECK_EQ(streamer.workMetrics().meshJobsAccepted, static_cast<uint64_t>(2));
    CHECK_EQ(streamer.diagnostics().mesh.terminalErrors, static_cast<size_t>(0));
    CHECK(meshStore.contains(coord));
    CHECK(!chunk.isDirty());

    ChunkMesh recoveredGeometry;
    meshStore.forEach([&](const WorldMeshEntry& entry) {
        if (entry.coord == coord) {
            recoveredGeometry = entry.mesh;
        }
    });
    CHECK(!recoveredGeometry.isEmpty());
    CHECK(!meshesMatch(recoveredGeometry, oldGeometry));

    for (uint32_t stable = 1;
         stable <= StreamingDiagnosticSnapshot::QuiescenceUpdateWindow;
         ++stable) {
        streamer.update(coord.toWorldCenter());
        streamer.processCompletions();
        CHECK_EQ(streamer.workMetrics().lastUpdateDesiredBuildCoordinatesInspected,
                 static_cast<uint64_t>(0));
        CHECK_EQ(streamer.workMetrics().lastUpdateSchedulerCoordinatesInspected,
                 static_cast<uint64_t>(0));
        CHECK_EQ(
            streamer.workMetrics().lastUpdateCacheEvictionCoordinatesInspected,
            static_cast<uint64_t>(0));
        CHECK_EQ(
            streamer.workMetrics().lastUpdateResidentEvictionCoordinatesInspected,
            static_cast<uint64_t>(0));
        CHECK_EQ(
            streamer.workMetrics().lastUpdateDeferredEvictionCoordinatesInspected,
            static_cast<uint64_t>(0));
    }
    CHECK_EQ(streamer.diagnostics().state,
             StreamingLifecycleState::Quiescent);

    const ChunkStreamer::WorkMetrics recoveredMetrics = streamer.workMetrics();
    for (int update = 0; update < 60; ++update) {
        streamer.update(coord.toWorldCenter());
        streamer.processCompletions();
        CHECK_EQ(streamer.diagnostics().state,
                 StreamingLifecycleState::Quiescent);
        CHECK_EQ(streamer.workMetrics().lastUpdateDesiredBuildCoordinatesInspected,
                 static_cast<uint64_t>(0));
        CHECK_EQ(streamer.workMetrics().lastUpdateSchedulerCoordinatesInspected,
                 static_cast<uint64_t>(0));
        CHECK_EQ(
            streamer.workMetrics().lastUpdateCacheEvictionCoordinatesInspected,
            static_cast<uint64_t>(0));
        CHECK_EQ(
            streamer.workMetrics().lastUpdateResidentEvictionCoordinatesInspected,
            static_cast<uint64_t>(0));
        CHECK_EQ(
            streamer.workMetrics().lastUpdateDeferredEvictionCoordinatesInspected,
            static_cast<uint64_t>(0));
    }
    CHECK_EQ(streamer.workMetrics().meshJobsStarted,
             recoveredMetrics.meshJobsStarted);
    CHECK_EQ(streamer.workMetrics().meshJobsFailed,
             recoveredMetrics.meshJobsFailed);
    CHECK_EQ(streamer.workMetrics().meshJobsAccepted,
             recoveredMetrics.meshJobsAccepted);
}

TEST_CASE(ChunkStreamer_StaleMeshFailureRetriesLatestRevision) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    BlockID solid = registerTestBlock(registry, "rigel:stale_mesh_failure_solid");

    const ChunkCoord coord{0, 0, 0};
    Chunk& chunk = manager.getOrCreateChunk(coord);
    chunk.setBlock(0, 0, 0, BlockState{solid}, registry);
    chunk.setWorldGenVersion(generator->config().world.version);
    chunk.setLoadedFromDisk(true);

    auto gate = std::make_shared<WorkerGate>();
    std::atomic<size_t> buildsEntered{0};
    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 0;
    stream.genQueueLimit = 0;
    stream.meshQueueLimit = 0;
    stream.updateBudgetPerFrame = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 2;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    Rigel::Voxel::detail::ChunkStreamerTestAccess::setMeshBuildStartCallback(
        streamer,
        [gate, &buildsEntered]() {
            if (buildsEntered.fetch_add(1, std::memory_order_relaxed) == 0) {
                gate->enterAndWait();
                throw std::runtime_error("injected stale mesh failure");
            }
        });
    WorkerGateRelease releaseOnExit(gate);

    streamer.update(coord.toWorldCenter());
    bool firstBuildEntered = gate->waitUntilEntered();
    if (!firstBuildEntered) {
        gate->release();
    }
    CHECK(firstBuildEntered);
    const uint32_t queuedRevision = chunk.meshRevision();

    manager.setBlock(1, 0, 0, BlockState{solid});
    streamer.update(coord.toWorldCenter());
    CHECK(chunk.meshRevision() != queuedRevision);
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(1));
    CHECK_EQ(streamer.workMetrics().meshRequestsCoalesced,
             static_cast<uint64_t>(1));

    gate->release();
    CHECK(waitForMeshCompletions(streamer, 1));
    CHECK_EQ(streamer.workMetrics().meshJobsRejectedStale,
             static_cast<uint64_t>(1));
    CHECK_EQ(streamer.workMetrics().meshJobsFailed, static_cast<uint64_t>(0));
    CHECK_EQ(streamer.diagnostics().mesh.inFlight, static_cast<size_t>(0));
    CHECK_EQ(streamer.diagnostics().mesh.pending, static_cast<size_t>(1));

    std::vector<ChunkStreamer::DebugChunkState> states;
    streamer.getDebugStates(states);
    CHECK_EQ(states.size(), static_cast<size_t>(1));
    CHECK_EQ(states.front().state, ChunkStreamer::DebugState::LoadedFromDisk);

    streamer.update(coord.toWorldCenter());
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(2));
    CHECK(waitForMeshCompletions(streamer, 2));

    const auto& metrics = streamer.workMetrics();
    CHECK_EQ(metrics.meshJobsCompleted, static_cast<uint64_t>(2));
    CHECK_EQ(metrics.meshJobsAccepted, static_cast<uint64_t>(1));
    CHECK_EQ(metrics.meshJobsRejectedStale, static_cast<uint64_t>(1));
    CHECK_EQ(metrics.meshJobsFailed, static_cast<uint64_t>(0));
    CHECK_EQ(streamer.diagnostics().mesh.inFlight, static_cast<size_t>(0));
    CHECK_EQ(streamer.diagnostics().mesh.pending, static_cast<size_t>(0));

    size_t installedIndexCount = 0;
    meshStore.forEach([&](const WorldMeshEntry& entry) {
        if (entry.coord == coord) {
            installedIndexCount = entry.mesh.indexCount();
        }
    });
    CHECK_EQ(installedIndexCount, static_cast<size_t>(60));
}

TEST_CASE(ChunkStreamer_DependencyChangesDuringInFlightMeshCoalesceFollowUp) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    BlockID solid = registerTestBlock(registry, "rigel:dependency_solid");

    Chunk& chunk = manager.getOrCreateChunk({0, 0, 0});
    chunk.setBlock(Chunk::SIZE - 1, 0, 0, BlockState{solid}, registry);
    chunk.setWorldGenVersion(generator->config().world.version);
    chunk.setLoadedFromDisk(true);

    auto gate = std::make_shared<WorkerGate>();
    std::atomic<size_t> buildsEntered{0};
    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 0;
    stream.genQueueLimit = 0;
    stream.meshQueueLimit = 0;
    stream.updateBudgetPerFrame = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 2;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    Rigel::Voxel::detail::ChunkStreamerTestAccess::setMeshBuildStartCallback(
        streamer,
        [gate, &buildsEntered]() {
            if (buildsEntered.fetch_add(1, std::memory_order_relaxed) == 0) {
                gate->enterAndWait();
            }
        });
    WorkerGateRelease releaseOnExit(gate);

    streamer.update(glm::vec3(0.0f));
    bool firstBuildEntered = gate->waitUntilEntered();
    if (!firstBuildEntered) {
        gate->release();
    }
    CHECK(firstBuildEntered);
    const uint32_t queuedRevision = chunk.meshRevision();

    manager.setBlock(Chunk::SIZE - 2, 0, 0, BlockState{solid});
    streamer.update(glm::vec3(0.0f));
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(1));
    manager.setBlock(Chunk::SIZE, 0, 0, BlockState{solid});
    streamer.update(glm::vec3(0.0f));
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(1));
    manager.setBlock(Chunk::SIZE, 1, 0, BlockState{solid});
    streamer.update(glm::vec3(0.0f));
    CHECK(chunk.meshRevision() != queuedRevision);
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(1));
    CHECK_EQ(buildsEntered.load(std::memory_order_relaxed), static_cast<size_t>(1));

    streamer.processCompletions();
    CHECK(!meshStore.contains({0, 0, 0}));
    CHECK_EQ(streamer.workMetrics().meshJobsCompleted, static_cast<uint64_t>(0));

    gate->release();
    CHECK(waitForMeshCompletions(streamer, 1));
    CHECK(!meshStore.contains({0, 0, 0}));
    streamer.update(glm::vec3(0.0f));
    streamer.update(glm::vec3(0.0f));
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(2));
    CHECK(waitForMeshCompletions(streamer, 2));

    const auto& metrics = streamer.workMetrics();
    CHECK_EQ(metrics.meshJobsStarted, static_cast<uint64_t>(2));
    CHECK_EQ(metrics.meshJobsCompleted, static_cast<uint64_t>(2));
    CHECK_EQ(metrics.meshJobsAccepted, static_cast<uint64_t>(1));
    CHECK_EQ(metrics.meshJobsRejectedStale, static_cast<uint64_t>(1));
    CHECK_EQ(metrics.meshRequestsCoalesced, static_cast<uint64_t>(1));
    CHECK_EQ(buildsEntered.load(std::memory_order_relaxed), static_cast<size_t>(2));

    size_t installedIndexCount = 0;
    meshStore.forEach([&](const WorldMeshEntry& entry) {
        if (entry.coord == ChunkCoord{0, 0, 0}) {
            installedIndexCount = entry.mesh.indexCount();
        }
    });
    CHECK_EQ(installedIndexCount, static_cast<size_t>(54));
}

TEST_CASE(ChunkStreamer_EmptyChunkIgnoresGeneratedNeighborArrival) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    const ChunkCoord survivingCoord{0, 2, 0};
    const ChunkCoord arrivingCoord{1, 2, 0};

    Chunk& surviving = manager.getOrCreateChunk(survivingCoord);
    surviving.setWorldGenVersion(generator->config().world.version);
    surviving.setLoadedFromDisk(true);
    surviving.clearDirty();

    for (int i = 0; i < DirectionCount; ++i) {
        Direction direction = static_cast<Direction>(i);
        int dx = 0;
        int dy = 0;
        int dz = 0;
        directionOffset(direction, dx, dy, dz);
        const ChunkCoord neighborCoord = survivingCoord.offset(dx, dy, dz);
        if (neighborCoord == arrivingCoord) {
            continue;
        }
        Chunk& neighbor = manager.getOrCreateChunk(neighborCoord);
        neighbor.setWorldGenVersion(generator->config().world.version);
        neighbor.setLoadedFromDisk(true);
        neighbor.clearDirty();
    }

    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
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

    streamer.update(survivingCoord.toWorldCenter());
    streamer.processCompletions();
    CHECK(surviving.isEmpty());
    CHECK(!surviving.isDirty());
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(0));
    const uint32_t settledRevision = surviving.meshRevision();

    stream.viewDistanceChunks = 1;
    streamer.setConfig(stream);
    streamer.update(survivingCoord.toWorldCenter());
    CHECK_EQ(streamer.workMetrics().generationJobsStarted,
             static_cast<uint64_t>(1));
    streamer.processCompletions();

    Chunk* arrived = manager.getChunk(arrivingCoord);
    CHECK(arrived != nullptr);
    CHECK(arrived->isEmpty());
    CHECK(surviving.isEmpty());
    CHECK(!surviving.isDirty());
    CHECK_EQ(surviving.meshRevision(), settledRevision);

    streamer.update(survivingCoord.toWorldCenter());
    streamer.processCompletions();
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(0));
    CHECK_EQ(streamer.diagnostics().mesh.pending, static_cast<size_t>(0));
}

TEST_CASE(ChunkStreamer_EmptyChunkIgnoresPersistedNeighborArrival) {
    WorldResources resources;
    World world(resources);
    auto& registry = resources.registry();
    auto generator = makeGenerator(registry);
    world.setGenerator(generator);
    auto& manager = world.chunkManager();
    WorldMeshStore meshStore;
    const ChunkCoord survivingCoord{0, 2, 0};
    const ChunkCoord arrivingCoord{1, 2, 0};

    Chunk& surviving = manager.getOrCreateChunk(survivingCoord);
    surviving.setWorldGenVersion(generator->config().world.version);
    surviving.setLoadedFromDisk(true);
    surviving.clearPersistDirty();
    surviving.clearDirty();
    addLoadedNeighborShell(
        manager,
        survivingCoord,
        arrivingCoord,
        generator->config().world.version);

    PersistedChunkContext persistence;
    persistence.context.providers = world.persistenceProvidersHandle();
    persistence.save(
        arrivingCoord,
        buildPayload(
            arrivingCoord,
            registry,
            {BlockRegistry::airId()},
            false,
            std::nullopt,
            false));
    auto loader = std::make_shared<Rigel::Persistence::AsyncChunkLoader>(
        persistence.service,
        persistence.context,
        world,
        generator->config().world.version,
        0,
        0,
        1,
        generator);
    loader->setPrefetchRadius(0);

    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
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
    configurePersistedChunkLoader(streamer, loader);

    streamer.update(survivingCoord.toWorldCenter());
    streamer.processCompletions();
    CHECK(surviving.isEmpty());
    CHECK(!surviving.isDirty());
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(0));
    const uint32_t settledRevision = surviving.meshRevision();

    stream.viewDistanceChunks = 1;
    streamer.setConfig(stream);
    streamer.update(survivingCoord.toWorldCenter());
    CHECK(loader->isPending(arrivingCoord));
    streamer.processCompletions();

    const Chunk* arrived = manager.getChunk(arrivingCoord);
    CHECK(arrived != nullptr);
    CHECK(arrived->loadedFromDisk());
    CHECK(arrived->isEmpty());
    CHECK(surviving.isEmpty());
    CHECK(!surviving.isDirty());
    CHECK_EQ(surviving.meshRevision(), settledRevision);
    CHECK(!loader->isPending(arrivingCoord));
    CHECK_EQ(loader->workCount().pending, static_cast<size_t>(0));
    CHECK_EQ(loader->workCount().inFlight, static_cast<size_t>(0));

    streamer.update(survivingCoord.toWorldCenter());
    streamer.processCompletions();
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(0));
    CHECK_EQ(streamer.diagnostics().mesh.pending, static_cast<size_t>(0));
    CHECK_EQ(streamer.diagnostics().mesh.inFlight, static_cast<size_t>(0));
}

TEST_CASE(ChunkStreamer_NonEmptyChunkRemeshesAfterPersistedNeighborArrival) {
    WorldResources resources;
    World world(resources);
    auto& registry = resources.registry();
    auto generator = makeGenerator(registry);
    world.setGenerator(generator);
    auto& manager = world.chunkManager();
    WorldMeshStore meshStore;
    const BlockID solid =
        registerTestBlock(registry, "rigel:persisted_boundary_solid");
    const ChunkCoord survivingCoord{0, 2, 0};
    const ChunkCoord arrivingCoord{1, 2, 0};

    Chunk& surviving = manager.getOrCreateChunk(survivingCoord);
    surviving.setBlock(
        Chunk::SIZE - 1, 1, 1, BlockState{solid}, registry);
    surviving.setWorldGenVersion(generator->config().world.version);
    surviving.setLoadedFromDisk(true);
    surviving.clearPersistDirty();
    addLoadedNeighborShell(
        manager,
        survivingCoord,
        arrivingCoord,
        generator->config().world.version);

    Chunk arriving(arrivingCoord);
    arriving.setBlock(0, 1, 1, BlockState{solid}, registry);
    PersistedChunkContext persistence;
    persistence.context.providers = world.persistenceProvidersHandle();
    persistence.save(
        arrivingCoord, Rigel::Persistence::serializeChunk(arriving));
    auto loader = std::make_shared<Rigel::Persistence::AsyncChunkLoader>(
        persistence.service,
        persistence.context,
        world,
        generator->config().world.version,
        0,
        0,
        1,
        generator);
    loader->setPrefetchRadius(0);

    auto gate = std::make_shared<WorkerGate>();
    std::atomic<size_t> buildsEntered{0};
    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 1;
    stream.genQueueLimit = 0;
    stream.meshQueueLimit = 1;
    stream.updateBudgetPerFrame = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 2;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    configurePersistedChunkLoader(streamer, loader);
    Rigel::Voxel::detail::ChunkStreamerTestAccess::setMeshBuildStartCallback(
        streamer,
        [gate, &buildsEntered]() {
            if (buildsEntered.fetch_add(1, std::memory_order_relaxed) == 0) {
                gate->enterAndWait();
            }
        });
    WorkerGateRelease releaseOnExit(gate);

    streamer.update(survivingCoord.toWorldCenter());
    bool firstBuildEntered = gate->waitUntilEntered();
    if (!firstBuildEntered) {
        gate->release();
    }
    CHECK(firstBuildEntered);
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(1));
    const uint32_t queuedRevision = surviving.meshRevision();

    stream.viewDistanceChunks = 1;
    streamer.setConfig(stream);
    streamer.update(survivingCoord.toWorldCenter());
    CHECK(loader->isPending(arrivingCoord));
    streamer.processCompletions();

    const Chunk* arrived = manager.getChunk(arrivingCoord);
    CHECK(arrived != nullptr);
    CHECK(arrived->loadedFromDisk());
    CHECK_EQ(arrived->getBlock(0, 1, 1).id, solid);
    CHECK_EQ(surviving.meshRevision(), queuedRevision + 1);
    CHECK(surviving.isDirty());

    streamer.update(survivingCoord.toWorldCenter());
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(1));
    CHECK_EQ(streamer.workMetrics().meshRequestsCoalesced,
             static_cast<uint64_t>(0));
    CHECK_EQ(buildsEntered.load(std::memory_order_relaxed),
             static_cast<size_t>(1));

    gate->release();
    CHECK(waitForMeshCompletions(streamer, 1));
    CHECK(!meshStore.contains(survivingCoord));
    CHECK_EQ(streamer.workMetrics().meshJobsRejectedStale,
             static_cast<uint64_t>(1));
    CHECK_EQ(streamer.workMetrics().meshRequestsCoalesced,
             static_cast<uint64_t>(1));

    streamer.update(survivingCoord.toWorldCenter());
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(2));
    CHECK(waitForPendingMeshCompletion(streamer));
    const auto replacementIndexCount =
        Rigel::Voxel::detail::ChunkStreamerTestAccess::pendingMeshIndexCount(
            streamer);
    CHECK(replacementIndexCount.has_value());
    CHECK_EQ(*replacementIndexCount, static_cast<size_t>(30));
    CHECK(waitForMeshCompletions(streamer, 2));

    const auto& metrics = streamer.workMetrics();
    CHECK_EQ(metrics.meshJobsStarted, static_cast<uint64_t>(2));
    CHECK_EQ(metrics.meshJobsCompleted, static_cast<uint64_t>(2));
    CHECK_EQ(metrics.meshJobsAccepted, static_cast<uint64_t>(1));
    CHECK_EQ(metrics.meshJobsRejectedStale, static_cast<uint64_t>(1));
    CHECK_EQ(metrics.meshRequestsCoalesced, static_cast<uint64_t>(1));
    CHECK_EQ(buildsEntered.load(std::memory_order_relaxed),
             static_cast<size_t>(2));

    size_t installedIndexCount = 0;
    meshStore.forEach([&](const WorldMeshEntry& entry) {
        if (entry.coord == survivingCoord) {
            installedIndexCount = entry.mesh.indexCount();
        }
    });
    CHECK_EQ(installedIndexCount, static_cast<size_t>(30));
    CHECK(!surviving.isDirty());
}

TEST_CASE(ChunkStreamer_NonEmptyChunkRemeshesAfterGeneratedNeighborArrival) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    BlockID solid = registerTestBlock(registry, "rigel:arrival_boundary_solid");
    const ChunkCoord survivingCoord{0, 2, 0};
    const ChunkCoord arrivingCoord{1, 2, 0};

    Chunk& surviving = manager.getOrCreateChunk(survivingCoord);
    surviving.setBlock(
        Chunk::SIZE - 1, 1, 1, BlockState{solid}, registry);
    surviving.setWorldGenVersion(generator->config().world.version);
    surviving.setLoadedFromDisk(true);

    for (int i = 0; i < DirectionCount; ++i) {
        Direction direction = static_cast<Direction>(i);
        int dx = 0;
        int dy = 0;
        int dz = 0;
        directionOffset(direction, dx, dy, dz);
        const ChunkCoord neighborCoord = survivingCoord.offset(dx, dy, dz);
        if (neighborCoord == arrivingCoord) {
            continue;
        }
        Chunk& neighbor = manager.getOrCreateChunk(neighborCoord);
        neighbor.setWorldGenVersion(generator->config().world.version);
        neighbor.setLoadedFromDisk(true);
        neighbor.clearDirty();
    }

    auto gate = std::make_shared<WorkerGate>();
    std::atomic<size_t> buildsEntered{0};
    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 1;
    stream.genQueueLimit = 0;
    stream.meshQueueLimit = 1;
    stream.updateBudgetPerFrame = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 2;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    Rigel::Voxel::detail::ChunkStreamerTestAccess::setMeshBuildStartCallback(
        streamer,
        [gate, &buildsEntered]() {
            if (buildsEntered.fetch_add(1, std::memory_order_relaxed) == 0) {
                gate->enterAndWait();
            }
        });
    WorkerGateRelease releaseOnExit(gate);

    streamer.update(survivingCoord.toWorldCenter());
    bool firstBuildEntered = gate->waitUntilEntered();
    if (!firstBuildEntered) {
        gate->release();
    }
    CHECK(firstBuildEntered);
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(1));
    const uint32_t queuedRevision = surviving.meshRevision();

    stream.viewDistanceChunks = 1;
    streamer.setConfig(stream);
    streamer.update(survivingCoord.toWorldCenter());
    CHECK_EQ(streamer.workMetrics().generationJobsStarted,
             static_cast<uint64_t>(1));
    CHECK(waitForGenerationCompletion(streamer));
    streamer.processCompletions();
    CHECK(manager.hasChunk(arrivingCoord));
    CHECK_EQ(surviving.meshRevision(), queuedRevision + 1);

    streamer.update(survivingCoord.toWorldCenter());
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(1));
    CHECK_EQ(buildsEntered.load(std::memory_order_relaxed), static_cast<size_t>(1));

    gate->release();
    CHECK(waitForMeshCompletions(streamer, 1));
    CHECK(!meshStore.contains(survivingCoord));
    CHECK_EQ(streamer.workMetrics().meshJobsRejectedStale,
             static_cast<uint64_t>(1));

    streamer.update(survivingCoord.toWorldCenter());
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(2));
    CHECK(waitForPendingMeshCompletion(streamer));
    const auto replacementIndexCount =
        Rigel::Voxel::detail::ChunkStreamerTestAccess::pendingMeshIndexCount(
            streamer);
    CHECK(replacementIndexCount.has_value());
    CHECK(*replacementIndexCount > 0);
    CHECK(waitForMeshCompletions(streamer, 2));

    const auto& metrics = streamer.workMetrics();
    CHECK_EQ(metrics.meshJobsStarted, static_cast<uint64_t>(2));
    CHECK_EQ(metrics.meshJobsCompleted, static_cast<uint64_t>(2));
    CHECK_EQ(metrics.meshJobsAccepted, static_cast<uint64_t>(1));
    CHECK_EQ(metrics.meshJobsRejectedStale, static_cast<uint64_t>(1));
    CHECK_EQ(metrics.meshRequestsCoalesced, static_cast<uint64_t>(1));
    CHECK_EQ(buildsEntered.load(std::memory_order_relaxed), static_cast<size_t>(2));
    CHECK(meshStore.contains(survivingCoord));
    CHECK(!surviving.isDirty());
}

TEST_CASE(ChunkStreamer_EmptyChunkIgnoresNeighborDeparture) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    const ChunkCoord survivingCoord{0, 2, 0};
    const ChunkCoord removedCoord{1, 2, 0};

    Chunk& surviving = manager.getOrCreateChunk(survivingCoord);
    surviving.setWorldGenVersion(generator->config().world.version);
    surviving.setLoadedFromDisk(true);
    surviving.clearDirty();
    Chunk& removed = manager.getOrCreateChunk(removedCoord);
    removed.setWorldGenVersion(generator->config().world.version);
    removed.setLoadedFromDisk(true);
    removed.clearDirty();

    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
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

    streamer.update(removedCoord.toWorldCenter());
    streamer.processCompletions();
    CHECK(manager.hasChunk(survivingCoord));
    CHECK(!surviving.isDirty());
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(0));
    const uint32_t settledRevision = surviving.meshRevision();

    stream.unloadDistanceChunks = 0;
    streamer.setConfig(stream);
    streamer.update(survivingCoord.toWorldCenter());
    CHECK(!manager.hasChunk(removedCoord));
    CHECK(!surviving.isDirty());
    CHECK_EQ(surviving.meshRevision(), settledRevision);

    streamer.update(survivingCoord.toWorldCenter());
    streamer.processCompletions();
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(0));
    CHECK_EQ(streamer.diagnostics().mesh.pending, static_cast<size_t>(0));
}

TEST_CASE(ChunkStreamer_RemeshesRetainedNeighborAfterDistanceEviction) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    BlockID solid =
        registerTestBlock(registry, "rigel:retained_unload_boundary_solid");
    const ChunkCoord survivingCoord{0, 0, 0};
    const ChunkCoord removedCoord{1, 0, 0};
    const ChunkCoord cameraCoord{-2, 0, 0};

    Chunk& surviving = manager.getOrCreateChunk(survivingCoord);
    surviving.setBlock(
        Chunk::SIZE - 1, 1, 1, BlockState{solid}, registry);
    surviving.setWorldGenVersion(generator->config().world.version);
    surviving.setLoadedFromDisk(true);

    Chunk& removed = manager.getOrCreateChunk(removedCoord);
    removed.setBlock(0, 1, 1, BlockState{solid}, registry);
    removed.setWorldGenVersion(generator->config().world.version);
    removed.setLoadedFromDisk(true);
    removed.clearPersistDirty();

    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 2;
    stream.genQueueLimit = 0;
    stream.meshQueueLimit = 0;
    stream.updateBudgetPerFrame = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 0;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    streamer.markSpawnDiscoveryComplete();

    streamer.update(removedCoord.toWorldCenter());
    streamer.processCompletions();
    streamer.update(survivingCoord.toWorldCenter());
    streamer.processCompletions();

    CHECK_EQ(installedMeshIndexCount(meshStore, survivingCoord),
             static_cast<size_t>(30));
    const uint64_t installedRevision =
        installedMeshRevision(meshStore, survivingCoord);

    Chunk& cameraChunk = manager.getOrCreateChunk(cameraCoord);
    cameraChunk.setWorldGenVersion(generator->config().world.version);
    cameraChunk.setLoadedFromDisk(true);
    cameraChunk.clearPersistDirty();
    cameraChunk.clearDirty();

    streamer.update(cameraCoord.toWorldCenter());
    streamer.processCompletions();
    CHECK(manager.hasChunk(survivingCoord));
    CHECK(!manager.hasChunk(removedCoord));
    CHECK_EQ(installedMeshIndexCount(meshStore, survivingCoord),
             static_cast<size_t>(30));
    CHECK_EQ(installedMeshRevision(meshStore, survivingCoord),
             installedRevision);
    CHECK_EQ(streamer.diagnostics().mesh.pending, static_cast<size_t>(1));
    CHECK_EQ(streamer.diagnostics().mesh.inFlight, static_cast<size_t>(0));

    streamer.update(cameraCoord.toWorldCenter());
    CHECK_EQ(streamer.diagnostics().mesh.pending, static_cast<size_t>(0));
    CHECK_EQ(streamer.diagnostics().mesh.inFlight, static_cast<size_t>(1));
    CHECK_EQ(installedMeshRevision(meshStore, survivingCoord),
             installedRevision);
    streamer.processCompletions();

    CHECK_EQ(installedMeshIndexCount(meshStore, survivingCoord),
             static_cast<size_t>(36));
    CHECK(installedMeshRevision(meshStore, survivingCoord) > installedRevision);
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(3));
    CHECK_EQ(streamer.workMetrics().meshJobsAccepted, static_cast<uint64_t>(3));

    const ChunkStreamer::WorkMetrics settled = streamer.workMetrics();
    for (int i = 0; i < 8; ++i) {
        streamer.update(cameraCoord.toWorldCenter());
        streamer.processCompletions();
    }
    CHECK_EQ(streamer.diagnostics().state,
             StreamingLifecycleState::Quiescent);
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, settled.meshJobsStarted);
    CHECK_EQ(streamer.workMetrics().meshJobsCompleted,
             settled.meshJobsCompleted);
    CHECK_EQ(streamer.workMetrics().schedulerCoordinatesInspected,
             settled.schedulerCoordinatesInspected);
    CHECK_EQ(streamer.workMetrics().lastUpdateDesiredBuildCoordinatesInspected,
             static_cast<uint64_t>(0));
    CHECK_EQ(streamer.workMetrics().lastUpdateSchedulerCoordinatesInspected,
             static_cast<uint64_t>(0));
}

TEST_CASE(ChunkStreamer_RemeshesSurvivingNeighborAfterDistanceEviction) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    BlockID solid = registerTestBlock(registry, "rigel:unload_boundary_solid");
    const ChunkCoord survivingCoord{0, 0, 0};
    const ChunkCoord removedCoord{1, 0, 0};
    const ChunkCoord cameraCoord{-2, 0, 0};

    Chunk& surviving = manager.getOrCreateChunk(survivingCoord);
    surviving.setBlock(
        Chunk::SIZE - 1, 1, 1, BlockState{solid}, registry);
    surviving.setWorldGenVersion(generator->config().world.version);
    surviving.setLoadedFromDisk(true);

    Chunk& removed = manager.getOrCreateChunk(removedCoord);
    removed.setBlock(0, 1, 1, BlockState{solid}, registry);
    removed.setWorldGenVersion(generator->config().world.version);
    removed.setLoadedFromDisk(true);
    removed.clearPersistDirty();

    auto gate = std::make_shared<WorkerGate>();
    std::atomic<size_t> buildsEntered{0};
    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 2;
    stream.genQueueLimit = 0;
    stream.meshQueueLimit = 0;
    stream.updateBudgetPerFrame = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 2;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);

    streamer.update(removedCoord.toWorldCenter());
    CHECK(waitForMeshCompletions(streamer, 1));
    CHECK(manager.hasChunk(removedCoord));
    CHECK(meshStore.contains(removedCoord));
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(1));
    CHECK_EQ(streamer.workMetrics().meshJobsAccepted, static_cast<uint64_t>(1));

    streamer.update(survivingCoord.toWorldCenter());
    CHECK(waitForMeshCompletions(streamer, 2));
    CHECK(manager.hasChunk(survivingCoord));
    CHECK(manager.hasChunk(removedCoord));
    CHECK(meshStore.contains(survivingCoord));
    CHECK(meshStore.contains(removedCoord));
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(2));
    CHECK_EQ(streamer.workMetrics().meshJobsAccepted, static_cast<uint64_t>(2));

    std::vector<ChunkStreamer::DebugChunkState> states;
    auto stateFor = [&](ChunkCoord coord)
        -> std::optional<ChunkStreamer::DebugState> {
        auto it = std::find_if(
            states.begin(), states.end(),
            [coord](const ChunkStreamer::DebugChunkState& state) {
                return state.coord == coord;
            });
        if (it == states.end()) {
            return std::nullopt;
        }
        return it->state;
    };
    streamer.getDebugStates(states);
    const auto survivingState = stateFor(survivingCoord);
    const auto removedState = stateFor(removedCoord);
    CHECK(survivingState.has_value());
    CHECK(removedState.has_value());
    CHECK_EQ(*survivingState, ChunkStreamer::DebugState::ReadyMesh);
    CHECK_EQ(*removedState, ChunkStreamer::DebugState::ReadyMesh);

    CHECK_EQ(installedMeshIndexCount(meshStore, survivingCoord),
             static_cast<size_t>(30));

    Rigel::Voxel::detail::ChunkStreamerTestAccess::setMeshBuildStartCallback(
        streamer,
        [gate, &buildsEntered]() {
            if (buildsEntered.fetch_add(1, std::memory_order_relaxed) == 0) {
                gate->enterAndWait();
            }
        });
    WorkerGateRelease releaseOnExit(gate);

    surviving.markDirty();
    streamer.update(survivingCoord.toWorldCenter());
    bool firstBuildEntered = gate->waitUntilEntered();
    if (!firstBuildEntered) {
        gate->release();
    }
    CHECK(firstBuildEntered);
    CHECK(manager.hasChunk(survivingCoord));
    CHECK(manager.hasChunk(removedCoord));
    CHECK_EQ(installedMeshIndexCount(meshStore, survivingCoord),
             static_cast<size_t>(30));
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(3));
    CHECK_EQ(buildsEntered.load(std::memory_order_relaxed), static_cast<size_t>(1));

    const uint32_t revisionBeforeRemoval = surviving.meshRevision();
    const uint64_t installedRevision =
        installedMeshRevision(meshStore, survivingCoord);
    Chunk& cameraChunk = manager.getOrCreateChunk(cameraCoord);
    cameraChunk.setWorldGenVersion(generator->config().world.version);
    cameraChunk.setLoadedFromDisk(true);
    cameraChunk.clearPersistDirty();
    cameraChunk.clearDirty();
    streamer.update(cameraCoord.toWorldCenter());

    CHECK(manager.hasChunk(survivingCoord));
    CHECK(!manager.hasChunk(removedCoord));
    CHECK_EQ(surviving.meshRevision(), revisionBeforeRemoval + 1);
    CHECK(!meshStore.contains(removedCoord));
    CHECK_EQ(installedMeshRevision(meshStore, survivingCoord),
             installedRevision);
    CHECK_EQ(streamer.diagnostics().mesh.inFlight, static_cast<size_t>(1));
    streamer.getDebugStates(states);
    CHECK(!stateFor(removedCoord).has_value());

    streamer.update(cameraCoord.toWorldCenter());
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(3));
    CHECK_EQ(streamer.workMetrics().meshRequestsCoalesced,
             static_cast<uint64_t>(1));
    CHECK_EQ(buildsEntered.load(std::memory_order_relaxed), static_cast<size_t>(1));

    gate->release();
    CHECK(waitForPendingMeshCompletion(streamer));
    const auto staleIndexCount =
        Rigel::Voxel::detail::ChunkStreamerTestAccess::pendingMeshIndexCount(
            streamer);
    CHECK(staleIndexCount.has_value());
    CHECK_EQ(*staleIndexCount, static_cast<size_t>(30));
    CHECK(waitForMeshCompletions(streamer, 3));
    CHECK_EQ(installedMeshIndexCount(meshStore, survivingCoord),
             static_cast<size_t>(30));
    CHECK_EQ(streamer.workMetrics().meshJobsAccepted, static_cast<uint64_t>(2));
    CHECK_EQ(streamer.workMetrics().meshJobsRejectedStale,
             static_cast<uint64_t>(1));
    CHECK_EQ(streamer.diagnostics().mesh.pending, static_cast<size_t>(1));
    CHECK_EQ(streamer.diagnostics().mesh.inFlight, static_cast<size_t>(0));
    CHECK_EQ(installedMeshRevision(meshStore, survivingCoord),
             installedRevision);

    streamer.update(cameraCoord.toWorldCenter());
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(4));
    CHECK_EQ(streamer.diagnostics().mesh.inFlight, static_cast<size_t>(1));
    CHECK(waitForMeshCompletions(streamer, 4));

    const auto& metrics = streamer.workMetrics();
    CHECK_EQ(metrics.meshJobsStarted, static_cast<uint64_t>(4));
    CHECK_EQ(metrics.meshJobsCompleted, static_cast<uint64_t>(4));
    CHECK_EQ(metrics.meshJobsAccepted, static_cast<uint64_t>(3));
    CHECK_EQ(metrics.meshJobsRejectedStale, static_cast<uint64_t>(1));
    CHECK_EQ(metrics.meshRequestsCoalesced, static_cast<uint64_t>(1));
    CHECK_EQ(buildsEntered.load(std::memory_order_relaxed), static_cast<size_t>(2));

    CHECK_EQ(installedMeshIndexCount(meshStore, survivingCoord),
             static_cast<size_t>(36));
    CHECK(installedMeshRevision(meshStore, survivingCoord) > installedRevision);
    streamer.getDebugStates(states);
    const auto finalSurvivingState = stateFor(survivingCoord);
    CHECK(finalSurvivingState.has_value());
    CHECK_EQ(*finalSurvivingState, ChunkStreamer::DebugState::ReadyMesh);

    for (int i = 0; i < 8; ++i) {
        streamer.update(cameraCoord.toWorldCenter());
        streamer.processCompletions();
    }
    CHECK_EQ(metrics.meshJobsStarted, static_cast<uint64_t>(4));
    CHECK_EQ(metrics.meshJobsCompleted, static_cast<uint64_t>(4));
    CHECK_EQ(metrics.meshJobsAccepted, static_cast<uint64_t>(3));
    CHECK_EQ(metrics.meshJobsRejectedStale, static_cast<uint64_t>(1));
    CHECK_EQ(metrics.meshRequestsCoalesced, static_cast<uint64_t>(1));
    CHECK_EQ(metrics.lastUpdateDesiredBuildCoordinatesInspected,
             static_cast<uint64_t>(0));
    CHECK_EQ(metrics.lastUpdateSchedulerCoordinatesInspected,
             static_cast<uint64_t>(0));
}

TEST_CASE(ChunkStreamer_ResetSupersedesOutstandingMeshRequest) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    BlockID solid = registerTestBlock(registry, "rigel:replacement_solid");
    const ChunkCoord coord{0, 0, 0};

    Chunk& original = manager.getOrCreateChunk(coord);
    original.setBlock(0, 0, 0, BlockState{solid}, registry);
    original.setWorldGenVersion(generator->config().world.version);
    original.setLoadedFromDisk(true);

    auto gate = std::make_shared<WorkerGate>();
    std::atomic<size_t> buildsEntered{0};
    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 0;
    stream.genQueueLimit = 0;
    stream.meshQueueLimit = 0;
    stream.updateBudgetPerFrame = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 2;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    Rigel::Voxel::detail::ChunkStreamerTestAccess::setMeshBuildStartCallback(
        streamer,
        [gate, &buildsEntered]() {
            if (buildsEntered.fetch_add(1, std::memory_order_relaxed) == 0) {
                gate->enterAndWait();
            }
        });
    WorkerGateRelease releaseOnExit(gate);

    streamer.update(glm::vec3(0.0f));
    bool firstBuildEntered = gate->waitUntilEntered();
    if (!firstBuildEntered) {
        gate->release();
    }
    CHECK(firstBuildEntered);
    const uint32_t queuedRevision = original.meshRevision();

    Rigel::Voxel::detail::ChunkStreamerTestAccess::reset(streamer);
    original.clearPersistDirty();
    CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::evictChunk(
        streamer, coord));
    Chunk& replacement = manager.getOrCreateChunk(coord);
    std::array<BlockState, Chunk::VOLUME> replacementBlocks{};
    replacementBlocks[1] = BlockState{solid};
    replacementBlocks[2] = BlockState{solid};
    replacement.copyFrom(replacementBlocks, registry);
    replacement.setWorldGenVersion(generator->config().world.version);
    replacement.setLoadedFromDisk(true);
    CHECK_EQ(replacement.meshRevision(), queuedRevision);

    streamer.update(glm::vec3(0.0f));
    streamer.update(glm::vec3(0.0f));
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(1));
    CHECK_EQ(buildsEntered.load(std::memory_order_relaxed), static_cast<size_t>(1));
    CHECK(!meshStore.contains(coord));

    gate->release();
    CHECK(waitForMeshCompletions(streamer, 1));
    CHECK_EQ(streamer.workMetrics().meshJobsRejectedStale, static_cast<uint64_t>(1));
    CHECK(!meshStore.contains(coord));
    streamer.update(glm::vec3(0.0f));
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(2));
    CHECK(waitForMeshCompletions(streamer, 2));

    const auto& metrics = streamer.workMetrics();
    CHECK_EQ(metrics.meshJobsStarted, static_cast<uint64_t>(2));
    CHECK_EQ(metrics.meshJobsCompleted, static_cast<uint64_t>(2));
    CHECK_EQ(metrics.meshJobsAccepted, static_cast<uint64_t>(1));
    CHECK_EQ(metrics.meshJobsRejectedStale, static_cast<uint64_t>(1));
    CHECK_EQ(buildsEntered.load(std::memory_order_relaxed), static_cast<size_t>(2));

    size_t installedIndexCount = 0;
    meshStore.forEach([&](const WorldMeshEntry& entry) {
        if (entry.coord == coord) {
            installedIndexCount = entry.mesh.indexCount();
        }
    });
    CHECK_EQ(installedIndexCount, static_cast<size_t>(60));
}

TEST_CASE(ChunkStreamer_GeneratorReplacementRetainsDirtyMeshCapacity) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto originalGenerator = makeGenerator(registry);
    auto replacementGenerator = std::make_shared<WorldGenerator>(
        registry, originalGenerator->config());
    BlockID solid =
        registerTestBlock(registry, "rigel:generator_replacement_mesh_solid");
    const ChunkCoord coord{0, 0, 0};
    const ChunkCoord cameraCoord{-1, 0, 0};

    Chunk& chunk = manager.getOrCreateChunk(coord);
    chunk.setBlock(0, 0, 0, BlockState{solid}, registry);
    chunk.setWorldGenVersion(originalGenerator->config().world.version);
    chunk.setLoadedFromDisk(true);
    meshStore.set(coord, {});
    Chunk& cameraChunk = manager.getOrCreateChunk(cameraCoord);
    cameraChunk.setWorldGenVersion(originalGenerator->config().world.version);
    cameraChunk.setLoadedFromDisk(true);
    cameraChunk.clearPersistDirty();
    cameraChunk.clearDirty();

    ChunkStreamer streamer(
        manager, meshStore, registry, nullptr, originalGenerator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 1;
    stream.genQueueLimit = 0;
    stream.meshQueueLimit = 2;
    stream.updateBudgetPerFrame = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 2;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);

    auto originalGate = std::make_shared<WorkerGate>();
    auto replacementGate = std::make_shared<WorkerGate>();
    WorkerGateRelease releaseOriginalOnExit(originalGate);
    WorkerGateRelease releaseReplacementOnExit(replacementGate);
    std::atomic<size_t> buildsEntered{0};
    Rigel::Voxel::detail::ChunkStreamerTestAccess::setMeshBuildStartCallback(
        streamer,
        [originalGate, replacementGate, &buildsEntered]() {
            size_t buildIndex = buildsEntered.fetch_add(1, std::memory_order_relaxed);
            if (buildIndex == 0) {
                originalGate->enterAndWait();
            } else if (buildIndex == 1) {
                replacementGate->enterAndWait();
            }
        });

    streamer.update(coord.toWorldCenter());
    CHECK(originalGate->waitUntilEntered());
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(1));
    CHECK_EQ(streamer.diagnostics().mesh.inFlight, static_cast<size_t>(1));
    CHECK_EQ(
        Rigel::Voxel::detail::ChunkStreamerTestAccess::inFlightMeshDirty(streamer),
        static_cast<size_t>(1));
    CHECK_EQ(
        Rigel::Voxel::detail::ChunkStreamerTestAccess::inFlightMeshMissing(streamer),
        static_cast<size_t>(0));

    streamer.update(cameraCoord.toWorldCenter());
    CHECK(manager.hasChunk(coord));
    streamer.setGenerator(replacementGenerator);
    CHECK(chunk.isDirty());
    streamer.update(cameraCoord.toWorldCenter());

    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(1));
    CHECK_EQ(streamer.diagnostics().mesh.inFlight, static_cast<size_t>(1));
    CHECK_EQ(
        Rigel::Voxel::detail::ChunkStreamerTestAccess::inFlightMeshDirty(streamer),
        static_cast<size_t>(1));

    originalGate->release();
    CHECK(waitForMeshCompletions(streamer, 1));
    CHECK_EQ(streamer.workMetrics().meshJobsRejectedStale, static_cast<uint64_t>(1));
    CHECK_EQ(streamer.diagnostics().mesh.inFlight, static_cast<size_t>(0));
    CHECK_EQ(
        Rigel::Voxel::detail::ChunkStreamerTestAccess::inFlightMeshDirty(streamer),
        static_cast<size_t>(0));

    streamer.update(cameraCoord.toWorldCenter());
    CHECK(replacementGate->waitUntilEntered());
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(2));
    CHECK_EQ(streamer.diagnostics().mesh.inFlight, static_cast<size_t>(1));
    CHECK_EQ(
        Rigel::Voxel::detail::ChunkStreamerTestAccess::inFlightMeshDirty(streamer),
        static_cast<size_t>(1));

    streamer.update(cameraCoord.toWorldCenter());
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(2));

    replacementGate->release();
    CHECK(waitForMeshCompletions(streamer, 2));
    CHECK_EQ(streamer.workMetrics().meshJobsAccepted, static_cast<uint64_t>(1));
    CHECK_EQ(streamer.workMetrics().meshJobsRejectedStale, static_cast<uint64_t>(1));
    CHECK_EQ(streamer.diagnostics().mesh.inFlight, static_cast<size_t>(0));
    CHECK_EQ(
        Rigel::Voxel::detail::ChunkStreamerTestAccess::inFlightMeshDirty(streamer),
        static_cast<size_t>(0));
    CHECK(meshStore.contains(coord));

    streamer.update(cameraCoord.toWorldCenter());
    streamer.processCompletions();
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(2));

    streamer.update(cameraCoord.toWorldCenter());
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(2));
    CHECK_EQ(streamer.workMetrics().lastUpdateDesiredBuildCoordinatesInspected,
             static_cast<uint64_t>(0));
    CHECK_EQ(streamer.workMetrics().lastUpdateSchedulerCoordinatesInspected,
             static_cast<uint64_t>(0));
}

TEST_CASE(ChunkStreamer_GeneratorReplacementInstallsOnlyCurrentMesh) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    TextureAtlas atlas;
    const std::string originalTexture = "textures/replacement_original.png";
    const std::string replacementTexture = "textures/replacement_current.png";
    addTestTexture(atlas, originalTexture);
    addTestTexture(atlas, replacementTexture);
    const TextureHandle originalTextureHandle =
        atlas.findTexture(originalTexture);
    const TextureHandle replacementTextureHandle =
        atlas.findTexture(replacementTexture);
    auto originalGenerator = makeGenerator(registry);
    BlockID originalSolid =
        registerTexturedTestBlock(
            registry, "rigel:overlap_original_solid", originalTexture);
    BlockID replacementSolid =
        registerTexturedTestBlock(
            registry, "rigel:overlap_replacement_solid", replacementTexture);

    WorldGenConfig replacementConfig = originalGenerator->config();
    ++replacementConfig.world.version;
    replacementConfig.solidBlock = "rigel:overlap_replacement_solid";
    replacementConfig.surfaceBlock = "rigel:overlap_replacement_solid";
    replacementConfig.terrain.baseHeight = 64.0f;
    auto replacementGenerator =
        std::make_shared<WorldGenerator>(registry, replacementConfig);

    const ChunkCoord coord{0, 0, 0};
    Chunk& original = manager.getOrCreateChunk(coord);
    original.setBlock(0, 0, 0, BlockState{originalSolid}, registry);
    original.setWorldGenVersion(originalGenerator->config().world.version);
    original.setLoadedFromDisk(true);
    original.clearPersistDirty();

    ChunkStreamer streamer(
        manager, meshStore, registry, &atlas, originalGenerator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 0;
    stream.genQueueLimit = 1;
    stream.meshQueueLimit = 1;
    stream.updateBudgetPerFrame = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 4;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);

    auto meshGate = std::make_shared<WorkerGate>();
    WorkerGateRelease releaseMeshOnExit(meshGate);
    std::atomic<size_t> meshBuildsEntered{0};
    Rigel::Voxel::detail::ChunkStreamerTestAccess::setMeshBuildStartCallback(
        streamer,
        [meshGate, &meshBuildsEntered]() {
            if (meshBuildsEntered.fetch_add(1, std::memory_order_relaxed) == 0) {
                meshGate->enterAndWait();
            }
        });
    streamer.update(coord.toWorldCenter());
    CHECK(meshGate->waitUntilEntered());
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(1));

    streamer.setGenerator(replacementGenerator);
    streamer.update(coord.toWorldCenter());
    CHECK(waitForGenerationCompletion(streamer));
    streamer.processCompletions();
    CHECK_EQ(streamer.workMetrics().generationJobsStarted,
             static_cast<uint64_t>(1));
    CHECK_EQ(streamer.diagnostics().generation.inFlight, static_cast<size_t>(0));
    Chunk* replacement = manager.getChunk(coord);
    CHECK(replacement != nullptr);
    if (!replacement) {
        return;
    }
    CHECK_EQ(replacement->worldGenVersion(), replacementConfig.world.version);
    CHECK_EQ(replacement->getBlock(0, 0, 0).id, replacementSolid);
    CHECK_EQ(replacement->getBlock(
                 Chunk::SIZE - 1, Chunk::SIZE - 1, Chunk::SIZE - 1).id,
             replacementSolid);

    streamer.update(coord.toWorldCenter());
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(1));
    CHECK_EQ(streamer.diagnostics().mesh.inFlight, static_cast<size_t>(1));
    CHECK(!meshStore.contains(coord));

    streamer.update(coord.toWorldCenter());
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(1));
    CHECK_EQ(streamer.workMetrics().lastUpdateDesiredBuildCoordinatesInspected,
             static_cast<uint64_t>(0));
    CHECK_EQ(streamer.workMetrics().lastUpdateSchedulerCoordinatesInspected,
             static_cast<uint64_t>(0));

    meshGate->release();
    CHECK(waitForMeshCompletions(streamer, 1));
    CHECK_EQ(streamer.workMetrics().meshJobsRejectedStale,
             static_cast<uint64_t>(1));
    CHECK_EQ(streamer.workMetrics().meshJobsAccepted, static_cast<uint64_t>(0));
    CHECK_EQ(streamer.diagnostics().mesh.inFlight, static_cast<size_t>(0));
    CHECK(!meshStore.contains(coord));

    streamer.update(coord.toWorldCenter());
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(2));
    CHECK(waitForMeshCompletions(streamer, 2));
    CHECK_EQ(streamer.workMetrics().generationJobsStarted,
             static_cast<uint64_t>(1));
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(2));
    CHECK_EQ(streamer.workMetrics().meshJobsAccepted, static_cast<uint64_t>(1));
    CHECK_EQ(streamer.workMetrics().meshJobsRejectedStale,
             static_cast<uint64_t>(1));

    bool foundMesh = false;
    meshStore.forEach([&](const WorldMeshEntry& entry) {
        if (entry.coord != coord) {
            return;
        }
        foundMesh = true;
        CHECK(!entry.mesh.vertices.empty());
        for (const VoxelVertex& vertex : entry.mesh.vertices) {
            CHECK_EQ(vertex.textureLayer,
                     static_cast<uint8_t>(replacementTextureHandle.index));
            CHECK(vertex.textureLayer !=
                  static_cast<uint8_t>(originalTextureHandle.index));
        }
    });
    CHECK(foundMesh);

    streamer.update(coord.toWorldCenter());
    streamer.processCompletions();
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(2));
    CHECK_EQ(streamer.workMetrics().lastUpdateDesiredBuildCoordinatesInspected,
             static_cast<uint64_t>(0));
    CHECK_EQ(streamer.workMetrics().lastUpdateSchedulerCoordinatesInspected,
             static_cast<uint64_t>(0));
}

TEST_CASE(ChunkStreamer_MeshRetirementPreservesReplacementGenerationFailure) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto originalGenerator = makeGenerator(registry);
    WorldGenConfig replacementConfig = originalGenerator->config();
    ++replacementConfig.world.version;
    auto replacementGenerator =
        std::make_shared<WorldGenerator>(registry, replacementConfig);
    BlockID solid =
        registerTestBlock(registry, "rigel:failed_replacement_original_solid");
    const ChunkCoord coord{0, 0, 0};

    Chunk& original = manager.getOrCreateChunk(coord);
    original.setBlock(0, 0, 0, BlockState{solid}, registry);
    original.setWorldGenVersion(originalGenerator->config().world.version);
    original.setLoadedFromDisk(true);
    original.clearPersistDirty();

    ChunkStreamer streamer(
        manager, meshStore, registry, nullptr, originalGenerator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 0;
    stream.genQueueLimit = 1;
    stream.meshQueueLimit = 1;
    stream.updateBudgetPerFrame = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 4;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);

    auto meshGate = std::make_shared<WorkerGate>();
    WorkerGateRelease releaseMeshOnExit(meshGate);
    std::atomic<size_t> meshBuildsEntered{0};
    Rigel::Voxel::detail::ChunkStreamerTestAccess::setMeshBuildStartCallback(
        streamer,
        [meshGate, &meshBuildsEntered]() {
            if (meshBuildsEntered.fetch_add(1, std::memory_order_relaxed) == 0) {
                meshGate->enterAndWait();
            }
        });
    Rigel::Voxel::detail::ChunkStreamerTestAccess::setGenerationStartCallback(
        streamer,
        []() { throw std::runtime_error("injected replacement failure"); });

    streamer.update(coord.toWorldCenter());
    CHECK(meshGate->waitUntilEntered());
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(1));

    streamer.setGenerator(replacementGenerator);
    streamer.update(coord.toWorldCenter());
    CHECK(waitForGenerationCompletion(streamer));
    streamer.processCompletions();
    CHECK_EQ(streamer.workMetrics().generationJobsStarted,
             static_cast<uint64_t>(1));
    CHECK_EQ(streamer.workMetrics().generationJobsFailed,
             static_cast<uint64_t>(1));
    CHECK(!manager.hasChunk(coord));

    std::vector<ChunkStreamer::DebugChunkState> states;
    streamer.getDebugStates(states);
    CHECK_EQ(states.size(), static_cast<size_t>(1));
    CHECK_EQ(states.front().coord, coord);
    CHECK_EQ(states.front().state,
             ChunkStreamer::DebugState::GenerationFailed);

    const uint64_t acceptedBeforeRetirement =
        streamer.workMetrics().meshJobsAccepted;
    meshGate->release();
    CHECK(waitForMeshCompletions(streamer, 1));
    CHECK_EQ(streamer.workMetrics().meshJobsAccepted,
             acceptedBeforeRetirement);
    CHECK_EQ(streamer.workMetrics().meshJobsRejectedStale,
             static_cast<uint64_t>(1));
    CHECK_EQ(streamer.diagnostics().mesh.inFlight, static_cast<size_t>(0));

    states.clear();
    streamer.getDebugStates(states);
    CHECK_EQ(states.size(), static_cast<size_t>(1));
    CHECK_EQ(states.front().coord, coord);
    CHECK_EQ(states.front().state,
             ChunkStreamer::DebugState::GenerationFailed);

    streamer.update(coord.toWorldCenter());
    CHECK_EQ(streamer.workMetrics().generationJobsStarted,
             static_cast<uint64_t>(1));
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(1));
    CHECK_EQ(streamer.workMetrics().meshJobsAccepted,
             acceptedBeforeRetirement);
    CHECK_EQ(streamer.workMetrics().lastUpdateDesiredBuildCoordinatesInspected,
             static_cast<uint64_t>(0));
    CHECK_EQ(streamer.workMetrics().lastUpdateSchedulerCoordinatesInspected,
             static_cast<uint64_t>(0));
    states.clear();
    streamer.getDebugStates(states);
    CHECK_EQ(states.size(), static_cast<size_t>(1));
    CHECK_EQ(states.front().state,
             ChunkStreamer::DebugState::GenerationFailed);
}

TEST_CASE(ChunkStreamer_DirtyNotificationCoalescesWithInFlightMesh) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    BlockID solid = registerTestBlock(registry, "rigel:notification_solid");

    Chunk& chunk = manager.getOrCreateChunk({0, 0, 0});
    chunk.setBlock(0, 0, 0, BlockState{solid}, registry);
    chunk.setWorldGenVersion(generator->config().world.version);
    chunk.setLoadedFromDisk(true);

    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
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

    streamer.update(glm::vec3(0.0f));
    const uint32_t queuedRevision = chunk.meshRevision();

    chunk.markDirty();
    chunk.markDirty();
    CHECK_EQ(chunk.meshRevision(), queuedRevision);

    streamer.processCompletions();
    CHECK_EQ(streamer.workMetrics().meshJobsAccepted, static_cast<uint64_t>(1));
    CHECK_EQ(streamer.workMetrics().meshJobsRejectedStale, static_cast<uint64_t>(0));

    streamer.update(glm::vec3(0.0f));
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(1));
}

TEST_CASE(ChunkStreamer_DiskLoadedChunksWaitForNeighborFrontierAcrossArrivalOrder) {
    struct Result {
        ChunkStreamer::WorkMetrics metrics;
        ChunkMesh mesh;
    };

    auto run = [](const std::array<Direction, DirectionCount>& arrivalOrder) {
        ChunkManager manager;
        BlockRegistry registry;
        WorldMeshStore meshStore;
        auto generator = makeGenerator(registry);
        BlockID solid = registerTestBlock(registry, "rigel:neighbor_order_solid");

        const ChunkCoord centerCoord{0, 0, 0};
        Chunk& center = manager.getOrCreateChunk(centerCoord);
        const int middle = Chunk::SIZE / 2;
        for (size_t i = 0; i < DirectionCount; ++i) {
            int dx = 0;
            int dy = 0;
            int dz = 0;
            directionOffset(static_cast<Direction>(i), dx, dy, dz);
            center.setBlock(
                dx < 0 ? 0 : (dx > 0 ? Chunk::SIZE - 1 : middle),
                dy < 0 ? 0 : (dy > 0 ? Chunk::SIZE - 1 : middle),
                dz < 0 ? 0 : (dz > 0 ? Chunk::SIZE - 1 : middle),
                BlockState{solid},
                registry);
        }
        center.setWorldGenVersion(generator->config().world.version);
        center.setLoadedFromDisk(true);
        center.clearDirty();

        ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
        StreamingConfig stream;
        stream.viewDistanceChunks = 1;
        stream.unloadDistanceChunks = 1;
        stream.genQueueLimit = 0;
        stream.meshQueueLimit = 0;
        stream.updateBudgetPerFrame = 0;
        stream.applyBudgetPerFrame = 0;
        stream.workerThreads = 0;
        stream.maxResidentChunks = 0;
        streamer.setConfig(stream);
        streamer.setChunkLoader([](ChunkLoadRequest) {
            return ChunkLoadRequestResult::Queued;
        });

        streamer.update(glm::vec3(0.0f));
        streamer.processCompletions();
        CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(0));
        CHECK(!meshStore.contains(centerCoord));

        size_t arrived = 0;
        for (Direction direction : arrivalOrder) {
            int dx = 0;
            int dy = 0;
            int dz = 0;
            directionOffset(direction, dx, dy, dz);
            Chunk& neighbor = manager.getOrCreateChunk(centerCoord.offset(dx, dy, dz));
            neighbor.setBlock(
                dx < 0 ? Chunk::SIZE - 1 : (dx > 0 ? 0 : middle),
                dy < 0 ? Chunk::SIZE - 1 : (dy > 0 ? 0 : middle),
                dz < 0 ? Chunk::SIZE - 1 : (dz > 0 ? 0 : middle),
                BlockState{solid},
                registry);
            neighbor.setWorldGenVersion(generator->config().world.version);
            neighbor.setLoadedFromDisk(true);
            neighbor.clearDirty();
            center.invalidateMesh();

            streamer.update(glm::vec3(0.0f));
            streamer.processCompletions();
            ++arrived;

            const uint64_t expectedJobs = static_cast<uint64_t>(arrived) +
                (arrived == arrivalOrder.size() ? 1 : 0);
            CHECK_EQ(streamer.workMetrics().meshJobsStarted, expectedJobs);
            CHECK_EQ(meshStore.contains(centerCoord), arrived == arrivalOrder.size());
        }

        Result result;
        result.metrics = streamer.workMetrics();
        meshStore.forEach([&](const WorldMeshEntry& entry) {
            if (entry.coord == centerCoord) {
                result.mesh = entry.mesh;
            }
        });
        return result;
    };

    const std::array<Direction, DirectionCount> forward{
        Direction::PosX,
        Direction::NegX,
        Direction::PosY,
        Direction::NegY,
        Direction::PosZ,
        Direction::NegZ
    };
    const std::array<Direction, DirectionCount> reverse{
        Direction::NegZ,
        Direction::PosZ,
        Direction::NegY,
        Direction::PosY,
        Direction::NegX,
        Direction::PosX
    };

    Result forwardResult = run(forward);
    Result reverseResult = run(reverse);

    CHECK_EQ(forwardResult.metrics.meshJobsStarted,
             static_cast<uint64_t>(DirectionCount + 1));
    CHECK_EQ(reverseResult.metrics.meshJobsStarted,
             static_cast<uint64_t>(DirectionCount + 1));
    CHECK_EQ(forwardResult.metrics.meshJobsAccepted,
             forwardResult.metrics.meshJobsStarted);
    CHECK_EQ(reverseResult.metrics.meshJobsAccepted,
             reverseResult.metrics.meshJobsStarted);
    CHECK(!forwardResult.mesh.isEmpty());
    CHECK(meshesMatch(forwardResult.mesh, reverseResult.mesh));
}

TEST_CASE(ChunkStreamer_SettledWorld_RemainsQuiescent) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    BlockID solid = registerTestBlock(registry, "rigel:quiescence_solid");

    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 1;
    stream.unloadDistanceChunks = 1;
    stream.genQueueLimit = 0;
    stream.meshQueueLimit = 0;
    stream.updateBudgetPerFrame = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 0;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);

    size_t loadAttempts = 0;
    streamer.setChunkLoader([&](ChunkLoadRequest request) {
        ++loadAttempts;
        ChunkCoord coord = request.coord;
        if (coord != ChunkCoord{0, 0, 0}) {
            return ChunkLoadRequestResult::Missing;
        }
        Chunk& chunk = manager.getOrCreateChunk(coord);
        chunk.setBlock(0, 0, 0, BlockState{solid}, registry);
        chunk.setWorldGenVersion(generator->config().world.version);
        chunk.setLoadedFromDisk(true);
        return ChunkLoadRequestResult::Queued;
    });

    for (int update = 0; update < 4; ++update) {
        streamer.update(glm::vec3(0.0f));
        streamer.processCompletions();
    }

    const ChunkStreamer::WorkMetrics settled = streamer.workMetrics();
    const size_t settledLoadAttempts = loadAttempts;
    CHECK(settled.generationJobsStarted > 0);
    CHECK(settled.chunkLoadRequestsStarted > 0);
    CHECK(settled.meshJobsStarted > 0);
    CHECK_EQ(settled.meshJobsStarted, settled.meshJobsCompleted);

    for (int update = 0; update < 5; ++update) {
        streamer.update(glm::vec3(0.0f));
        streamer.processCompletions();
    }

    const auto& quiescent = streamer.workMetrics();
    CHECK_EQ(quiescent.generationJobsStarted, settled.generationJobsStarted);
    CHECK_EQ(quiescent.chunkLoadRequestsStarted, settled.chunkLoadRequestsStarted);
    CHECK_EQ(quiescent.meshJobsStarted, settled.meshJobsStarted);
    CHECK_EQ(loadAttempts, settledLoadAttempts);
    CHECK_EQ(quiescent.lastUpdateDesiredBuildCoordinatesInspected, static_cast<uint64_t>(0));
    CHECK_EQ(quiescent.lastUpdateSchedulerCoordinatesInspected, static_cast<uint64_t>(0));
    CHECK_EQ(quiescent.desiredBuildCoordinatesInspected,
             settled.desiredBuildCoordinatesInspected);
    CHECK_EQ(quiescent.schedulerCoordinatesInspected,
             settled.schedulerCoordinatesInspected);

    Chunk* center = manager.getChunk({0, 0, 0});
    CHECK(center != nullptr);
    if (!center) {
        return;
    }
    center->setBlock(1, 0, 0, BlockState{solid}, registry);
    center->invalidateMesh();
    center->markDirty();
    streamer.update(glm::vec3(0.0f));
    CHECK_EQ(quiescent.meshJobsStarted, settled.meshJobsStarted + 1);
    CHECK_EQ(quiescent.lastUpdateSchedulerCoordinatesInspected, static_cast<uint64_t>(1));
    streamer.processCompletions();

    streamer.update(glm::vec3(0.0f));
    streamer.processCompletions();
    streamer.update(glm::vec3(0.0f));
    CHECK_EQ(quiescent.lastUpdateSchedulerCoordinatesInspected, static_cast<uint64_t>(0));
}

TEST_CASE(ChunkStreamer_QuiescenceRequiresStableIdleUpdates) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    BlockID solid = registerTestBlock(registry, "rigel:diagnostic_solid");

    Chunk& center = manager.getOrCreateChunk({0, 0, 0});
    center.setBlock(0, 0, 0, BlockState{solid}, registry);
    center.setWorldGenVersion(generator->config().world.version);
    center.setLoadedFromDisk(true);

    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
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

    StreamingWorkCount loadWork{
        .pending = 1,
        .inFlight = 0,
        .started = 1
    };
    streamer.setChunkLoadWorkCallback([&loadWork]() { return loadWork; });

    CHECK_EQ(streamer.diagnostics().state,
             StreamingLifecycleState::DiscoveringSpawn);
    streamer.processCompletions();
    CHECK_EQ(streamer.diagnostics().state,
             StreamingLifecycleState::DiscoveringSpawn);

    streamer.markSpawnDiscoveryComplete();
    CHECK_EQ(streamer.diagnostics().state,
             StreamingLifecycleState::AwaitingInitialStream);
    streamer.processCompletions();
    CHECK_EQ(streamer.diagnostics().state,
             StreamingLifecycleState::AwaitingInitialStream);

    streamer.update(glm::vec3(0.0f));
    streamer.processCompletions();
    CHECK_EQ(streamer.diagnostics().state, StreamingLifecycleState::Streaming);
    CHECK_EQ(streamer.diagnostics().chunkLoad.pending, static_cast<size_t>(1));
    CHECK_EQ(streamer.diagnostics().chunkLoad.inFlight, static_cast<size_t>(0));

    loadWork.pending = 0;
    loadWork.inFlight = 1;
    streamer.update(glm::vec3(0.0f));
    streamer.processCompletions();
    CHECK_EQ(streamer.diagnostics().state, StreamingLifecycleState::Streaming);
    CHECK_EQ(streamer.diagnostics().chunkLoad.pending, static_cast<size_t>(0));
    CHECK_EQ(streamer.diagnostics().chunkLoad.inFlight, static_cast<size_t>(1));

    loadWork.inFlight = 0;
    streamer.update(glm::vec3(0.0f));
    streamer.processCompletions();
    CHECK_EQ(streamer.diagnostics().state, StreamingLifecycleState::Stabilizing);
    CHECK_EQ(streamer.diagnostics().stableUpdates, static_cast<uint32_t>(1));

    for (uint32_t stable = 2;
         stable < StreamingDiagnosticSnapshot::QuiescenceUpdateWindow;
         ++stable) {
        streamer.update(glm::vec3(0.0f));
        streamer.processCompletions();
        CHECK_EQ(streamer.diagnostics().state,
                 StreamingLifecycleState::Stabilizing);
        CHECK_EQ(streamer.diagnostics().stableUpdates, stable);
    }

    streamer.update(glm::vec3(0.0f));
    streamer.processCompletions();
    CHECK_EQ(streamer.diagnostics().state, StreamingLifecycleState::Quiescent);
    CHECK_EQ(streamer.diagnostics().stableUpdates,
             StreamingDiagnosticSnapshot::QuiescenceUpdateWindow);
    CHECK(streamer.diagnostics().workEmpty());

    const auto settledMetrics = streamer.workMetrics();
    const auto settledLoadStarted = streamer.diagnostics().chunkLoad.started;
    for (int update = 0; update < 5; ++update) {
        streamer.update(glm::vec3(0.0f));
        streamer.processCompletions();
        CHECK_EQ(streamer.diagnostics().state, StreamingLifecycleState::Quiescent);
    }
    CHECK_EQ(streamer.workMetrics().generationJobsStarted,
             settledMetrics.generationJobsStarted);
    CHECK_EQ(streamer.diagnostics().chunkLoad.started, settledLoadStarted);
    CHECK_EQ(streamer.workMetrics().meshJobsStarted,
             settledMetrics.meshJobsStarted);
    CHECK_EQ(streamer.workMetrics().lastUpdateDesiredBuildCoordinatesInspected,
             static_cast<uint64_t>(0));
    CHECK_EQ(streamer.workMetrics().lastUpdateSchedulerCoordinatesInspected,
             static_cast<uint64_t>(0));

    center.setBlock(1, 0, 0, BlockState{solid}, registry);
    center.invalidateMesh();
    center.markDirty();
    streamer.update(glm::vec3(0.0f));
    streamer.processCompletions();
    CHECK_EQ(streamer.diagnostics().state, StreamingLifecycleState::Streaming);
    CHECK_EQ(streamer.diagnostics().stableUpdates, static_cast<uint32_t>(0));
    CHECK_EQ(streamer.workMetrics().meshJobsStarted,
             settledMetrics.meshJobsStarted + 1);

    for (uint32_t stable = 1;
         stable <= StreamingDiagnosticSnapshot::QuiescenceUpdateWindow;
         ++stable) {
        streamer.update(glm::vec3(0.0f));
        streamer.processCompletions();
        CHECK_EQ(streamer.diagnostics().stableUpdates, stable);
        CHECK_EQ(streamer.diagnostics().state,
                 stable == StreamingDiagnosticSnapshot::QuiescenceUpdateWindow
                 ? StreamingLifecycleState::Quiescent
                 : StreamingLifecycleState::Stabilizing);
    }
}

TEST_CASE(StreamingDiagnostics_FailureSignatureChangesOnlyWithFailures) {
    StreamingDiagnosticSnapshot previous;
    StreamingDiagnosticSnapshot current;

    current.generation.pending = 1;
    current.mesh.inFlight = 1;
    current.chunkLoad.started = 2;
    CHECK(!streamingFailureSignatureChanged(previous, current));

    current.generation.terminalErrors = 1;
    current.generation.lastError = "Chunk generation failed at (0, 0, 0)";
    CHECK(streamingFailureSignatureChanged(previous, current));
    previous = current;
    CHECK(!streamingFailureSignatureChanged(previous, current));

    current.chunkLoad.terminalErrors = 1;
    current.chunkLoad.lastError = "Chunk load failed at (1, 0, 0)";
    CHECK(streamingFailureSignatureChanged(previous, current));
    previous = current;

    current.mesh.terminalErrors = 1;
    current.mesh.lastError = "Chunk mesh build failed at (2, 0, 0)";
    CHECK(streamingFailureSignatureChanged(previous, current));
    previous = current;

    current.eviction.pending = 1;
    current.eviction.lastError =
        "Chunk eviction persistence failed at (3, 0, 0)";
    CHECK(streamingFailureSignatureChanged(previous, current));
    previous = current;
    CHECK(!streamingFailureSignatureChanged(previous, current));

    current.eviction.lastError.clear();
    CHECK(streamingFailureSignatureChanged(previous, current));
    previous = current;
    current.eviction.pending = 0;
    CHECK(streamingFailureSignatureChanged(previous, current));
}

TEST_CASE(ChunkStreamer_FailureSignatureTracksNonRepresentativeLoadChange) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);

    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 2;
    stream.unloadDistanceChunks = 2;
    stream.genQueueLimit = 0;
    stream.meshQueueLimit = 0;
    stream.updateBudgetPerFrame = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 0;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);

    const ChunkCoord representative{-1, 0, 0};
    const ChunkCoord departing{0, 0, -2};
    const ChunkCoord arriving{0, 0, 3};
    std::unordered_map<ChunkCoord, ChunkLoadRequest, ChunkCoordHash> requests;
    std::vector<ChunkLoadCompletion> completions;
    streamer.setChunkLoader([&](ChunkLoadRequest request) {
        if (request.coord != representative && request.coord != departing &&
            request.coord != arriving) {
            return ChunkLoadRequestResult::Missing;
        }
        requests[request.coord] = request;
        return ChunkLoadRequestResult::Queued;
    });
    streamer.setChunkLoadDrain([&](size_t) {
        std::vector<ChunkLoadCompletion> drained = std::move(completions);
        completions.clear();
        return drained;
    });

    streamer.update(ChunkCoord{0, 0, 0}.toWorldCenter());
    CHECK(requests.find(representative) != requests.end());
    CHECK(requests.find(departing) != requests.end());
    completions.push_back({representative,
                           requests[representative].requestId,
                           ChunkLoadOutcome::Failed,
                           "representative load failure"});
    completions.push_back({departing,
                           requests[departing].requestId,
                           ChunkLoadOutcome::Failed,
                           "departing load failure"});
    streamer.processCompletions();

    const StreamingDiagnosticSnapshot previous = streamer.diagnostics();
    CHECK_EQ(previous.chunkLoad.terminalErrors, static_cast<size_t>(2));
    CHECK(previous.chunkLoad.lastError.find("(-1, 0, 0)") !=
          std::string::npos);

    streamer.update(ChunkCoord{0, 0, 1}.toWorldCenter());
    CHECK(requests.find(arriving) != requests.end());
    completions.push_back({arriving,
                           requests[arriving].requestId,
                           ChunkLoadOutcome::Failed,
                           "arriving load failure"});
    streamer.processCompletions();

    const StreamingDiagnosticSnapshot current = streamer.diagnostics();
    CHECK_EQ(current.chunkLoad.terminalErrors,
             previous.chunkLoad.terminalErrors);
    CHECK_EQ(current.chunkLoad.lastError, previous.chunkLoad.lastError);
    CHECK(streamingFailureSignatureChanged(previous, current));

    streamer.update(ChunkCoord{0, 0, 1}.toWorldCenter());
    streamer.processCompletions();
    CHECK(!streamingFailureSignatureChanged(current, streamer.diagnostics()));
}

TEST_CASE(ChunkStreamer_SteadyStateSchedulerWorkDoesNotScaleWithViewVolume) {
    auto settleAndMeasure = [](int viewDistance) {
        ChunkManager manager;
        BlockRegistry registry;
        WorldMeshStore meshStore;
        auto generator = makeGenerator(registry);

        ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
        StreamingConfig stream;
        stream.viewDistanceChunks = viewDistance;
        stream.unloadDistanceChunks = viewDistance;
        stream.genQueueLimit = 0;
        stream.meshQueueLimit = 0;
        stream.updateBudgetPerFrame = 0;
        stream.applyBudgetPerFrame = 0;
        stream.workerThreads = 0;
        stream.maxResidentChunks = 0;
        streamer.setConfig(stream);

        size_t loadAttempts = 0;
        streamer.setChunkLoader([&](ChunkLoadRequest request) {
            ++loadAttempts;
            ChunkCoord coord = request.coord;
            Chunk& chunk = manager.getOrCreateChunk(coord);
            chunk.setWorldGenVersion(generator->config().world.version);
            chunk.setLoadedFromDisk(true);
            return ChunkLoadRequestResult::Queued;
        });

        for (int update = 0; update < 4; ++update) {
            streamer.update(glm::vec3(0.0f));
            streamer.processCompletions();
        }

        const ChunkStreamer::WorkMetrics settled = streamer.workMetrics();
        const size_t settledLoadAttempts = loadAttempts;
        CHECK(settled.chunkLoadRequestsStarted > 0);
        CHECK_EQ(settled.generationJobsStarted, static_cast<uint64_t>(0));
        CHECK_EQ(settled.meshJobsStarted, static_cast<uint64_t>(0));
        CHECK_EQ(settled.lastUpdateDesiredBuildCoordinatesInspected,
                 static_cast<uint64_t>(0));
        CHECK_EQ(settled.lastUpdateSchedulerCoordinatesInspected,
                 static_cast<uint64_t>(0));

        uint64_t steadyStateCoordinatesInspected = 0;
        for (int update = 0; update < 8; ++update) {
            streamer.update(glm::vec3(0.0f));
            const auto& current = streamer.workMetrics();
            steadyStateCoordinatesInspected +=
                current.lastUpdateDesiredBuildCoordinatesInspected;
            steadyStateCoordinatesInspected +=
                current.lastUpdateSchedulerCoordinatesInspected;
            streamer.processCompletions();
        }

        const auto& quiescent = streamer.workMetrics();
        CHECK_EQ(steadyStateCoordinatesInspected, static_cast<uint64_t>(0));
        CHECK_EQ(quiescent.generationJobsStarted, settled.generationJobsStarted);
        CHECK_EQ(quiescent.chunkLoadRequestsStarted, settled.chunkLoadRequestsStarted);
        CHECK_EQ(quiescent.meshJobsStarted, settled.meshJobsStarted);
        CHECK_EQ(quiescent.desiredBuildCoordinatesInspected,
                 settled.desiredBuildCoordinatesInspected);
        CHECK_EQ(quiescent.schedulerCoordinatesInspected,
                 settled.schedulerCoordinatesInspected);
        CHECK_EQ(loadAttempts, settledLoadAttempts);
        return settled.desiredBuildCoordinatesInspected;
    };

    const uint64_t smallViewBuildCoordinates = settleAndMeasure(1);
    const uint64_t largeViewBuildCoordinates = settleAndMeasure(12);
    CHECK(largeViewBuildCoordinates > smallViewBuildCoordinates * 100);
}

TEST_CASE(ChunkStreamer_UnloadHysteresisAvoidsOneChunkReversalChurn) {
    struct MovementResult {
        size_t initialResidents = 0;
        uint64_t entered = 0;
        size_t moveEvicted = 0;
        size_t movedResidents = 0;
        uint64_t reloaded = 0;
        uint64_t regenerated = 0;
        size_t reversalEvicted = 0;
        uint64_t remeshed = 0;
        size_t reversedResidents = 0;
    };

    auto run = [](int unloadDistance) {
        ChunkManager manager;
        BlockRegistry registry;
        WorldMeshStore meshStore;
        auto generator = makeGenerator(registry);
        const BlockID solid =
            registerTestBlock(registry, "rigel:hysteresis_boundary_solid");

        constexpr int viewDistance = 12;
        constexpr int viewRadiusSq = viewDistance * viewDistance;
        const ChunkCoord origin{0, 0, 0};
        const ChunkCoord boundaryNeighbor{-11, 0, 0};
        for (int z = -viewDistance; z <= viewDistance; ++z) {
            for (int y = -viewDistance; y <= viewDistance; ++y) {
                for (int x = -viewDistance; x <= viewDistance; ++x) {
                    if (x * x + y * y + z * z > viewRadiusSq) {
                        continue;
                    }
                    const ChunkCoord coord{x, y, z};
                    Chunk& chunk = manager.getOrCreateChunk(coord);
                    chunk.setWorldGenVersion(generator->config().world.version);
                    chunk.setLoadedFromDisk(true);
                    chunk.clearDirty();
                }
            }
        }
        Chunk* remeshProbe = manager.getChunk(boundaryNeighbor);
        CHECK(remeshProbe != nullptr);
        if (remeshProbe) {
            remeshProbe->setBlock(0, 0, 0, BlockState{solid}, registry);
            remeshProbe->clearPersistDirty();
        }

        ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
        StreamingConfig stream;
        stream.viewDistanceChunks = viewDistance;
        stream.unloadDistanceChunks = unloadDistance;
        stream.genQueueLimit = 0;
        stream.meshQueueLimit = 0;
        stream.updateBudgetPerFrame = 0;
        stream.applyBudgetPerFrame = 0;
        stream.workerThreads = 0;
        stream.maxResidentChunks = 0;
        streamer.setConfig(stream);
        streamer.markSpawnDiscoveryComplete();

        streamer.setChunkLoader([&](ChunkLoadRequest request) {
            Chunk& chunk = manager.getOrCreateChunk(request.coord);
            chunk.setWorldGenVersion(generator->config().world.version);
            chunk.setLoadedFromDisk(true);
            chunk.clearDirty();
            return ChunkLoadRequestResult::Queued;
        });

        auto residentCoordinates = [&]() {
            std::unordered_set<ChunkCoord, ChunkCoordHash> resident;
            resident.reserve(manager.loadedChunkCount());
            manager.forEachChunk([&](ChunkCoord coord, const Chunk&) {
                resident.insert(coord);
            });
            return resident;
        };
        auto removedCoordinates = [](
            const std::unordered_set<ChunkCoord, ChunkCoordHash>& before,
            const std::unordered_set<ChunkCoord, ChunkCoordHash>& after) {
            return static_cast<size_t>(std::count_if(
                before.begin(), before.end(), [&](ChunkCoord coord) {
                    return after.find(coord) == after.end();
                }));
        };
        auto settle = [&](ChunkCoord center) {
            bool quiescent = false;
            for (int update = 0; update < 32; ++update) {
                streamer.update(center.toWorldCenter());
                streamer.processCompletions();
                if (streamer.diagnostics().state ==
                    StreamingLifecycleState::Quiescent) {
                    quiescent = true;
                    break;
                }
            }
            CHECK(quiescent);

            const ChunkStreamer::WorkMetrics settled = streamer.workMetrics();
            const size_t settledResidents = manager.loadedChunkCount();
            for (int update = 0; update < 3; ++update) {
                streamer.update(center.toWorldCenter());
                streamer.processCompletions();
            }
            const ChunkStreamer::WorkMetrics stationary = streamer.workMetrics();
            CHECK_EQ(stationary.generationJobsStarted,
                     settled.generationJobsStarted);
            CHECK_EQ(stationary.chunkLoadRequestsStarted,
                     settled.chunkLoadRequestsStarted);
            CHECK_EQ(stationary.meshJobsStarted, settled.meshJobsStarted);
            CHECK_EQ(stationary.desiredBuildCoordinatesInspected,
                     settled.desiredBuildCoordinatesInspected);
            CHECK_EQ(stationary.schedulerCoordinatesInspected,
                     settled.schedulerCoordinatesInspected);
            CHECK_EQ(stationary.residentEvictionCoordinatesInspected,
                     settled.residentEvictionCoordinatesInspected);
            CHECK_EQ(manager.loadedChunkCount(), settledResidents);
        };

        settle(origin);
        const auto initial = residentCoordinates();
        const ChunkStreamer::WorkMetrics initialWork = streamer.workMetrics();

        const ChunkCoord moved{1, 0, 0};
        settle(moved);
        const auto afterMove = residentCoordinates();
        const ChunkStreamer::WorkMetrics moveWork = streamer.workMetrics();

        settle(origin);
        const auto afterReversal = residentCoordinates();
        const ChunkStreamer::WorkMetrics reversalWork = streamer.workMetrics();

        MovementResult result;
        result.initialResidents = initial.size();
        result.entered = moveWork.chunkLoadRequestsStarted -
            initialWork.chunkLoadRequestsStarted;
        result.moveEvicted = removedCoordinates(initial, afterMove);
        result.movedResidents = afterMove.size();
        result.reloaded = reversalWork.chunkLoadRequestsStarted -
            moveWork.chunkLoadRequestsStarted;
        result.regenerated = reversalWork.generationJobsStarted -
            moveWork.generationJobsStarted;
        result.reversalEvicted = removedCoordinates(afterMove, afterReversal);
        result.remeshed = reversalWork.meshJobsStarted - moveWork.meshJobsStarted;
        result.reversedResidents = afterReversal.size();
        return result;
    };

    const MovementResult radius12 = run(12);
    CHECK_EQ(radius12.initialResidents, static_cast<size_t>(7153));
    CHECK_EQ(radius12.entered, static_cast<uint64_t>(441));
    CHECK_EQ(radius12.moveEvicted, static_cast<size_t>(441));
    CHECK_EQ(radius12.movedResidents, radius12.initialResidents);
    CHECK_EQ(radius12.reloaded, static_cast<uint64_t>(441));
    CHECK_EQ(radius12.regenerated, static_cast<uint64_t>(0));
    CHECK_EQ(radius12.reversalEvicted, static_cast<size_t>(441));
    CHECK_EQ(radius12.remeshed, static_cast<uint64_t>(0));
    CHECK_EQ(radius12.reversedResidents, radius12.initialResidents);

    const MovementResult radius13 = run(13);
    CHECK_EQ(radius13.initialResidents, radius12.initialResidents);
    CHECK_EQ(radius13.entered, radius12.entered);
    CHECK_EQ(radius13.moveEvicted, static_cast<size_t>(0));
    CHECK_EQ(radius13.movedResidents,
             radius13.initialResidents + static_cast<size_t>(441));
    CHECK_EQ(radius13.reloaded, static_cast<uint64_t>(0));
    CHECK_EQ(radius13.regenerated, static_cast<uint64_t>(0));
    CHECK_EQ(radius13.reversalEvicted, static_cast<size_t>(0));
    CHECK_EQ(radius13.remeshed, static_cast<uint64_t>(0));
    CHECK_EQ(radius13.reversedResidents, radius13.movedResidents);
}

TEST_CASE(ChunkStreamer_SettledWorld_RegeneratesAfterVersionChange) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);

    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
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

    for (int update = 0; update < 4; ++update) {
        streamer.update(glm::vec3(0.0f));
        streamer.processCompletions();
    }

    streamer.update(glm::vec3(0.0f));
    const ChunkStreamer::WorkMetrics settled = streamer.workMetrics();
    CHECK_EQ(settled.lastUpdateSchedulerCoordinatesInspected, static_cast<uint64_t>(0));

    WorldGenConfig changedConfig = generator->config();
    ++changedConfig.world.version;
    generator = std::make_shared<WorldGenerator>(
        registry, std::move(changedConfig));
    streamer.setGenerator(generator);

    streamer.update(glm::vec3(0.0f));
    const auto& changed = streamer.workMetrics();
    CHECK_EQ(changed.generationJobsStarted, settled.generationJobsStarted + 1);
    CHECK(changed.lastUpdateSchedulerCoordinatesInspected > 0);
    streamer.processCompletions();

    for (int update = 0; update < 3; ++update) {
        streamer.update(glm::vec3(0.0f));
        streamer.processCompletions();
    }

    Chunk* regenerated = manager.getChunk({0, 0, 0});
    CHECK(regenerated != nullptr);
    if (!regenerated) {
        return;
    }
    CHECK_EQ(regenerated->worldGenVersion(), generator->config().world.version);

    const uint64_t generationJobsStarted = changed.generationJobsStarted;
    const uint64_t meshJobsStarted = changed.meshJobsStarted;
    streamer.update(glm::vec3(0.0f));
    CHECK_EQ(changed.generationJobsStarted, generationJobsStarted);
    CHECK_EQ(changed.meshJobsStarted, meshJobsStarted);
    CHECK_EQ(changed.lastUpdateSchedulerCoordinatesInspected, static_cast<uint64_t>(0));
}
