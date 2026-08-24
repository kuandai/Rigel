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
#include <limits>
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

    static bool hasReadyPendingMesh(const ChunkStreamer& streamer,
                                    ChunkCoord coord) {
        return streamer.m_pendingMeshes.find(coord) !=
            streamer.m_pendingMeshes.end();
    }

    static bool hasDependencyPendingMesh(const ChunkStreamer& streamer,
                                         ChunkCoord coord) {
        return streamer.m_meshDependencyWaiting.find(coord) !=
            streamer.m_meshDependencyWaiting.end();
    }

    static std::optional<uint64_t> pendingMeshSequence(
        const ChunkStreamer& streamer,
        ChunkCoord coord) {
        auto it = streamer.m_pendingMeshes.find(coord);
        return it == streamer.m_pendingMeshes.end()
            ? std::nullopt
            : std::optional<uint64_t>{it->second.sequence};
    }

    static std::optional<size_t> pendingMeshPriority(
        const ChunkStreamer& streamer,
        ChunkCoord coord) {
        auto it = streamer.m_pendingMeshes.find(coord);
        return it == streamer.m_pendingMeshes.end()
            ? std::nullopt
            : std::optional<size_t>{it->second.priority};
    }

    static bool pendingMeshIsPrioritized(const ChunkStreamer& streamer,
                                         ChunkCoord coord) {
        auto it = streamer.m_pendingMeshes.find(coord);
        return it != streamer.m_pendingMeshes.end() && it->second.prioritized;
    }

    static bool hasReplacementPendingMesh(const ChunkStreamer& streamer,
                                          ChunkCoord coord) {
        auto it = streamer.m_meshInFlight.find(coord);
        return it != streamer.m_meshInFlight.end() &&
            it->second.replacementPending;
    }

    static std::optional<uint64_t> inFlightMeshRequestId(
        const ChunkStreamer& streamer,
        ChunkCoord coord) {
        auto it = streamer.m_meshInFlight.find(coord);
        return it == streamer.m_meshInFlight.end()
            ? std::nullopt
            : std::optional<uint64_t>{it->second.requestId};
    }

    static bool inFlightMeshIsPrioritized(const ChunkStreamer& streamer,
                                          ChunkCoord coord) {
        auto it = streamer.m_meshInFlight.find(coord);
        return it != streamer.m_meshInFlight.end() && it->second.prioritized;
    }

    static size_t replacementPendingMeshCount(const ChunkStreamer& streamer) {
        return streamer.m_replacementPendingMeshCount;
    }

    static size_t configRetiredWorkCount(const ChunkStreamer& streamer) {
        return streamer.m_configRetiredWork.size();
    }

    static size_t meshDispatchLimit(const ChunkStreamer& streamer) {
        return streamer.meshDispatchLimit();
    }

    static bool hasConfigRetiredWork(const ChunkStreamer& streamer,
                                     ChunkCoord coord) {
        return streamer.m_configRetiredWork.find(coord) !=
            streamer.m_configRetiredWork.end();
    }

    static std::optional<uint32_t> inFlightMeshObservedRevision(
        const ChunkStreamer& streamer,
        ChunkCoord coord) {
        auto it = streamer.m_meshInFlight.find(coord);
        return it == streamer.m_meshInFlight.end()
            ? std::nullopt
            : std::optional<uint32_t>{it->second.observedRevision};
    }

    static bool desiredContains(const ChunkStreamer& streamer,
                                ChunkCoord coord) {
        return streamer.m_desiredSet.find(coord) != streamer.m_desiredSet.end();
    }

    static bool hasPendingLoad(const ChunkStreamer& streamer,
                               ChunkCoord coord) {
        return streamer.m_loadPending.find(coord) != streamer.m_loadPending.end();
    }

    static bool hasExplicitMeshPriority(const ChunkStreamer& streamer,
                                        ChunkCoord coord) {
        return streamer.m_priorityMeshRequests.find(coord) !=
            streamer.m_priorityMeshRequests.end();
    }

    static bool hasGenerationCapacityWait(const ChunkStreamer& streamer,
                                          ChunkCoord coord) {
        return streamer.m_generationCapacityWaiting.find(coord) !=
            streamer.m_generationCapacityWaiting.end();
    }

    static bool hasSubmittedGeneration(const ChunkStreamer& streamer,
                                       ChunkCoord coord) {
        return streamer.m_genCancel.find(coord) != streamer.m_genCancel.end();
    }

    static bool hasVersionReplacementWait(const ChunkStreamer& streamer,
                                          ChunkCoord coord) {
        return streamer.m_versionReplacementWaiting.find(coord) !=
            streamer.m_versionReplacementWaiting.end();
    }

    static size_t readyPendingMeshCount(const ChunkStreamer& streamer) {
        return streamer.m_pendingMeshes.size();
    }

    static size_t pendingMeshQueueRecordCount(const ChunkStreamer& streamer) {
        return streamer.m_pendingMeshQueues[static_cast<size_t>(
                   ChunkStreamer::MeshRequestKind::Missing)].size() +
            streamer.m_pendingMeshQueues[static_cast<size_t>(
                ChunkStreamer::MeshRequestKind::Dirty)].size();
    }

    static bool queuePendingMissingMesh(ChunkStreamer& streamer,
                                        ChunkCoord coord) {
        return streamer.queuePendingMesh(
            coord, ChunkStreamer::MeshRequestKind::Missing);
    }

    static bool queuePendingDirtyMesh(ChunkStreamer& streamer,
                                      ChunkCoord coord) {
        return streamer.queuePendingMesh(
            coord, ChunkStreamer::MeshRequestKind::Dirty);
    }

    static void waitForMeshDependencies(ChunkStreamer& streamer,
                                        ChunkCoord coord) {
        streamer.waitForMeshDependencies(coord);
    }

    static void retirePendingMesh(ChunkStreamer& streamer, ChunkCoord coord) {
        streamer.retirePendingMesh(coord);
    }

    static uint64_t reprioritizePendingMeshes(ChunkStreamer& streamer) {
        uint64_t inspected = 0;
        streamer.reprioritizePendingMeshes(inspected);
        return inspected;
    }

    static void pushStalePendingMeshHead(ChunkStreamer& streamer,
                                         ChunkCoord coord) {
        auto pendingIt = streamer.m_pendingMeshes.find(coord);
        if (pendingIt == streamer.m_pendingMeshes.end()) {
            return;
        }
        ChunkStreamer::PendingMeshRequest stale = pendingIt->second;
        stale.sequence = 0;
        streamer.m_pendingMeshQueues[static_cast<size_t>(stale.kind)].push(
            stale);
    }

    static void pushStalePendingMeshTail(ChunkStreamer& streamer,
                                         ChunkCoord coord,
                                         size_t count) {
        auto pendingIt = streamer.m_pendingMeshes.find(coord);
        if (pendingIt == streamer.m_pendingMeshes.end()) {
            return;
        }
        ChunkStreamer::PendingMeshRequest stale = pendingIt->second;
        stale.sequence = std::numeric_limits<uint64_t>::max();
        for (size_t i = 0; i < count; ++i) {
            streamer.m_pendingMeshQueues[static_cast<size_t>(stale.kind)].push(
                stale);
        }
    }

    static uint64_t dispatchPendingMeshes(ChunkStreamer& streamer) {
        uint64_t inspected = 0;
        streamer.dispatchPendingMeshes(inspected);
        return inspected;
    }

    static void queueLoadedNeighbors(ChunkStreamer& streamer,
                                     ChunkCoord coord) {
        streamer.queueLoadedNeighbors(coord);
    }

    static void queueLoadGen(ChunkStreamer& streamer, ChunkCoord coord) {
        streamer.queueLoadGen(coord);
    }

    static void injectLoadGenOwner(ChunkStreamer& streamer,
                                   ChunkCoord coord) {
        if (streamer.m_loadGenQueued.insert(coord).second) {
            streamer.m_loadGenQueue.push_back(coord);
        }
    }

    static void rememberConfigRetiredLoadGen(ChunkStreamer& streamer,
                                             ChunkCoord coord) {
        streamer.rememberConfigRetiredWork(
            coord, ChunkStreamer::ConfigRetiredWorkKind::LoadGen);
    }

    static void rememberConfigRetiredDirtyMesh(ChunkStreamer& streamer,
                                               ChunkCoord coord) {
        streamer.rememberConfigRetiredWork(
            coord, ChunkStreamer::ConfigRetiredWorkKind::DirtyMesh);
    }

    static bool isConfigRetiredLoadGen(const ChunkStreamer& streamer,
                                       ChunkCoord coord) {
        auto it = streamer.m_configRetiredWork.find(coord);
        return it != streamer.m_configRetiredWork.end() &&
            it->second == ChunkStreamer::ConfigRetiredWorkKind::LoadGen;
    }

    static bool isConfigRetiredMissingMesh(const ChunkStreamer& streamer,
                                           ChunkCoord coord) {
        auto it = streamer.m_configRetiredWork.find(coord);
        return it != streamer.m_configRetiredWork.end() &&
            it->second == ChunkStreamer::ConfigRetiredWorkKind::MissingMesh;
    }

    static bool isConfigRetiredDirtyMesh(const ChunkStreamer& streamer,
                                         ChunkCoord coord) {
        auto it = streamer.m_configRetiredWork.find(coord);
        return it != streamer.m_configRetiredWork.end() &&
            it->second == ChunkStreamer::ConfigRetiredWorkKind::DirtyMesh;
    }

    static std::vector<ChunkCoord> pendingLoadGenOrder(
        const ChunkStreamer& streamer) {
        return {
            streamer.m_loadGenQueue.begin(),
            streamer.m_loadGenQueue.end()
        };
    }

    static std::vector<ChunkCoord> inFlightMeshDispatchOrder(
        const ChunkStreamer& streamer) {
        std::vector<std::pair<uint64_t, ChunkCoord>> ordered;
        ordered.reserve(streamer.m_meshInFlight.size());
        for (const auto& [coord, flight] : streamer.m_meshInFlight) {
            ordered.emplace_back(flight.requestId, coord);
        }
        std::sort(ordered.begin(), ordered.end());
        std::vector<ChunkCoord> result;
        result.reserve(ordered.size());
        for (const auto& [requestId, coord] : ordered) {
            result.push_back(coord);
        }
        return result;
    }

    static bool evictChunk(ChunkStreamer& streamer, ChunkCoord coord) {
        return streamer.evictChunk(coord);
    }

    static void reset(ChunkStreamer& streamer) {
        streamer.reset();
    }

    static void refreshDiagnostics(ChunkStreamer& streamer) {
        streamer.refreshDiagnostics(false);
    }

    static bool areFaceNeighbors(ChunkCoord lhs, ChunkCoord rhs) {
        return ChunkStreamer::areFaceNeighbors(lhs, rhs);
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

class IncrementingTraceClock {
public:
    ChunkVisibilityTimePoint now() {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto result = m_now;
        m_now += std::chrono::milliseconds(1);
        ++m_reads;
        return result;
    }

    size_t reads() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_reads;
    }

private:
    mutable std::mutex m_mutex;
    ChunkVisibilityTimePoint m_now{};
    size_t m_reads = 0;
};

class ManualTraceClock {
public:
    ChunkVisibilityTimePoint now() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_now;
    }

    void advance(ChunkVisibilityDuration duration) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_now += duration;
    }

private:
    mutable std::mutex m_mutex;
    ChunkVisibilityTimePoint m_now{};
};

class ThrowingTraceClock {
public:
    explicit ThrowingTraceClock(size_t throwOnRead)
        : m_throwOnRead(throwOnRead) {}

    ChunkVisibilityTimePoint now() {
        const size_t read =
            m_reads.fetch_add(1, std::memory_order_relaxed) + 1;
        if (read == m_throwOnRead) {
            throw std::runtime_error("trace clock failure");
        }
        return ChunkVisibilityTimePoint{} + std::chrono::milliseconds(read);
    }

private:
    size_t m_throwOnRead = 0;
    std::atomic<size_t> m_reads{0};
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
    streamer.setChunkLoadDiagnosticsCallback([loader]() {
        return loader->diagnostics();
    });
    streamer.setChunkEvictionCallback([loader](ChunkCoord coord) {
        return loader->persistChunk(coord);
    });
}

void addLoadedNeighborShell(ChunkManager& manager,
                            ChunkCoord center,
                            std::optional<ChunkCoord> omitted,
                            uint32_t worldGenVersion) {
    for (int i = 0; i < DirectionCount; ++i) {
        const Direction direction = static_cast<Direction>(i);
        int dx = 0;
        int dy = 0;
        int dz = 0;
        directionOffset(direction, dx, dy, dz);
        const ChunkCoord coord = center.offset(dx, dy, dz);
        if (omitted && coord == *omitted) {
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

TEST_CASE(ChunkStreamer_VersionReplacementDefersUntilCoordinateReturns) {
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

    const ChunkCoord coord{0, 4, 0};
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

    const ChunkCoord outsideView{1, 4, 0};
    streamer.update(outsideView.toWorldCenter());
    streamer.processCompletions();
    CHECK_EQ(streamer.diagnostics().eviction.pending, static_cast<size_t>(0));
    CHECK(streamer.diagnostics().eviction.lastError.empty());
    CHECK_EQ(persistenceAttempts, static_cast<size_t>(1));

    streamer.update(outsideView.toWorldCenter());
    streamer.processCompletions();
    for (uint32_t update = 0;
         update < StreamingDiagnosticSnapshot::QuiescenceUpdateWindow;
         ++update) {
        streamer.update(outsideView.toWorldCenter());
        streamer.processCompletions();
        CHECK_EQ(streamer.diagnostics().eviction.pending,
                 static_cast<size_t>(0));
        CHECK_EQ(streamer.workMetrics().lastUpdateDesiredBuildCoordinatesInspected,
                 static_cast<uint64_t>(0));
        CHECK_EQ(streamer.workMetrics().lastUpdateSchedulerCoordinatesInspected,
                 static_cast<uint64_t>(0));
    }
    CHECK_EQ(streamer.diagnostics().state,
             StreamingLifecycleState::Quiescent);
    CHECK_EQ(persistenceAttempts, static_cast<size_t>(1));
    CHECK(manager.hasChunk(coord));
    CHECK(manager.getChunk(coord)->isPersistDirty());

    streamer.update(coord.toWorldCenter());
    CHECK_EQ(persistenceAttempts, static_cast<size_t>(2));
    CHECK(!manager.hasChunk(coord));
    CHECK_EQ(streamer.diagnostics().generation.inFlight, static_cast<size_t>(1));
    CHECK_EQ(streamer.diagnostics().eviction.pending, static_cast<size_t>(1));

    streamer.processCompletions();
    CHECK(manager.hasChunk(coord));
    CHECK_EQ(manager.getChunk(coord)->worldGenVersion(),
             replacementGenerator->config().world.version);
    CHECK_EQ(streamer.diagnostics().generation.inFlight, static_cast<size_t>(0));
    CHECK_EQ(streamer.diagnostics().eviction.pending, static_cast<size_t>(0));
}

TEST_CASE(ChunkStreamer_DepartedVersionReplacementPersistsBeforeUnload) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto originalGenerator = makeGenerator(registry);
    WorldGenConfig replacementConfig = originalGenerator->config();
    ++replacementConfig.world.version;
    auto replacementGenerator =
        std::make_shared<WorldGenerator>(registry, replacementConfig);
    const BlockID edited = registerTestBlock(
        registry, "rigel:departed_replacement_durable_edit");
    const ChunkCoord coord{0, 4, 0};
    const ChunkCoord offCamera{1, 4, 0};
    const ChunkCoord outsideUnload{4, 4, 0};

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
    bool secondAttemptObservedDirtyOwner = false;
    streamer.setChunkEvictionCallback([&](ChunkCoord request) {
        CHECK_EQ(request, coord);
        ++persistenceAttempts;
        Chunk* resident = manager.getChunk(request);
        CHECK(resident != nullptr);
        if (persistenceAttempts == 1) {
            return false;
        }
        secondAttemptObservedDirtyOwner =
            resident && resident->isPersistDirty();
        if (resident) {
            resident->clearPersistDirty();
        }
        return true;
    });

    streamer.setGenerator(replacementGenerator);
    streamer.update(coord.toWorldCenter());
    streamer.processCompletions();
    CHECK_EQ(persistenceAttempts, static_cast<size_t>(1));
    CHECK_EQ(streamer.diagnostics().eviction.pending,
             static_cast<size_t>(1));

    Chunk& offCameraChunk = manager.getOrCreateChunk(offCamera);
    offCameraChunk.setWorldGenVersion(replacementConfig.world.version);
    offCameraChunk.setLoadedFromDisk(true);
    offCameraChunk.clearPersistDirty();
    offCameraChunk.clearDirty();
    streamer.update(offCamera.toWorldCenter());
    streamer.processCompletions();
    for (uint32_t stable = 0;
         stable < StreamingDiagnosticSnapshot::QuiescenceUpdateWindow;
         ++stable) {
        streamer.update(offCamera.toWorldCenter());
        streamer.processCompletions();
    }
    CHECK_EQ(streamer.diagnostics().state,
             StreamingLifecycleState::Quiescent);
    CHECK_EQ(streamer.diagnostics().eviction.pending,
             static_cast<size_t>(0));
    CHECK(streamer.diagnostics().eviction.lastError.empty());
    CHECK_EQ(persistenceAttempts, static_cast<size_t>(1));
    CHECK(manager.hasChunk(coord));
    CHECK(manager.getChunk(coord)->isPersistDirty());

    Chunk& outsideCamera = manager.getOrCreateChunk(outsideUnload);
    outsideCamera.setWorldGenVersion(replacementConfig.world.version);
    outsideCamera.setLoadedFromDisk(true);
    outsideCamera.clearPersistDirty();
    outsideCamera.clearDirty();
    streamer.update(outsideUnload.toWorldCenter());
    streamer.processCompletions();

    CHECK_EQ(persistenceAttempts, static_cast<size_t>(2));
    CHECK(secondAttemptObservedDirtyOwner);
    CHECK(!manager.hasChunk(coord));
    CHECK_EQ(streamer.workMetrics().generationJobsStarted,
             static_cast<uint64_t>(0));
    CHECK(streamer.diagnostics().generation.empty());
    CHECK(streamer.diagnostics().eviction.empty());
    CHECK(streamer.diagnostics().eviction.lastError.empty());

    for (uint32_t stable = 0;
         stable < StreamingDiagnosticSnapshot::QuiescenceUpdateWindow;
         ++stable) {
        streamer.update(outsideUnload.toWorldCenter());
        streamer.processCompletions();
    }
    CHECK_EQ(persistenceAttempts, static_cast<size_t>(2));
    CHECK_EQ(streamer.diagnostics().state,
             StreamingLifecycleState::Quiescent);
}

TEST_CASE(ChunkStreamer_DepartureRetiresVersionReplacementWait) {
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
    const ChunkCoord outsideUnload{4, 4, 0};
    streamer.update(outsideUnload.toWorldCenter());
    streamer.processCompletions();
    CHECK(!manager.hasChunk(coord));
    CHECK_EQ(persistenceAttempts, static_cast<size_t>(1));
    CHECK_EQ(streamer.diagnostics().eviction.pending, static_cast<size_t>(0));
    CHECK(streamer.diagnostics().eviction.lastError.empty());

    for (uint32_t update = 0;
         update < StreamingDiagnosticSnapshot::QuiescenceUpdateWindow;
         ++update) {
        streamer.update(outsideUnload.toWorldCenter());
        streamer.processCompletions();
        CHECK_EQ(streamer.diagnostics().eviction.pending,
                 static_cast<size_t>(0));
        CHECK_EQ(persistenceAttempts, static_cast<size_t>(1));
        CHECK_EQ(
            streamer.workMetrics().lastUpdateDeferredEvictionCoordinatesInspected,
            static_cast<uint64_t>(0));
    }
    CHECK_EQ(streamer.diagnostics().state,
             StreamingLifecycleState::Quiescent);

    streamer.update(coord.toWorldCenter());
    CHECK_EQ(streamer.diagnostics().generation.inFlight, static_cast<size_t>(1));
    CHECK_EQ(streamer.diagnostics().eviction.pending, static_cast<size_t>(0));

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
        streamer.getDebugStates(states, coord, 0);
        CHECK_EQ(states.size(), static_cast<size_t>(1));
        CHECK_EQ(states.front().coord, coord);
        CHECK_EQ(states.front().state,
                 ChunkStreamer::DebugState::TerminalFailure);
        CHECK_EQ(states.front().failure,
                 ChunkStreamer::DebugFailure::Generation);

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
        streamer.getDebugStates(states, nextCoord, 0);
        CHECK_EQ(states.size(), static_cast<size_t>(1));
        CHECK_EQ(states.front().coord, nextCoord);
        CHECK_EQ(states.front().state,
                 ChunkStreamer::DebugState::TerminalFailure);
        CHECK_EQ(states.front().failure,
                 ChunkStreamer::DebugFailure::Generation);
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

TEST_CASE(ChunkStreamer_NearMissingMeshWinsAfterDependencyReadiness) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    BlockID solid = registerTestBlock(
        registry, "rigel:mesh_dispatch_priority_solid");

    const ChunkCoord nearCoord{0, 0, 0};
    const ChunkCoord missingDependency{1, 0, 0};
    const std::array<ChunkCoord, 3> fartherCoords{
        ChunkCoord{-1, 0, 0},
        ChunkCoord{0, -1, 0},
        ChunkCoord{0, 0, -1}
    };
    for (int z = -2; z <= 2; ++z) {
        for (int y = -2; y <= 2; ++y) {
            for (int x = -2; x <= 2; ++x) {
                if (x * x + y * y + z * z > 4) {
                    continue;
                }
                const ChunkCoord coord{x, y, z};
                if (coord == missingDependency) {
                    continue;
                }
                Chunk& chunk = manager.getOrCreateChunk(coord);
                chunk.setWorldGenVersion(generator->config().world.version);
                chunk.setLoadedFromDisk(true);
                chunk.clearDirty();
            }
        }
    }
    manager.getChunk(nearCoord)->setBlock(
        0, 0, 0, BlockState{solid}, registry);
    manager.getChunk(nearCoord)->clearDirty();
    for (const ChunkCoord& coord : fartherCoords) {
        manager.getChunk(coord)->setBlock(
            0, 0, 0, BlockState{solid}, registry);
        manager.getChunk(coord)->clearDirty();
    }

    auto gate = std::make_shared<WorkerGate>();
    std::atomic<size_t> buildsEntered{0};
    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 2;
    stream.unloadDistanceChunks = 2;
    stream.genQueueLimit = 0;
    stream.meshQueueLimit = 8;
    stream.updateBudgetPerFrame = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 2;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    streamer.setChunkLoader([missingDependency](ChunkLoadRequest request) {
        return request.coord == missingDependency
            ? ChunkLoadRequestResult::Queued
            : ChunkLoadRequestResult::Missing;
    });
    Rigel::Voxel::detail::ChunkStreamerTestAccess::setMeshBuildStartCallback(
        streamer,
        [gate, &buildsEntered]() {
            if (buildsEntered.fetch_add(1, std::memory_order_relaxed) == 0) {
                gate->enterAndWait();
            }
        });
    WorkerGateRelease releaseOnExit(gate);

    streamer.update(nearCoord.toWorldCenter());
    const bool firstBuildEntered = gate->waitUntilEntered();
    if (!firstBuildEntered) {
        gate->release();
    }
    CHECK(firstBuildEntered);
    CHECK_EQ(streamer.diagnostics().meshWorkerCount, static_cast<size_t>(1));
    CHECK_EQ(streamer.diagnostics().meshSubmissionLimit,
             static_cast<size_t>(1));
    CHECK_EQ(streamer.diagnostics().mesh.inFlight, static_cast<size_t>(1));
    CHECK_EQ(streamer.diagnostics().mesh.pending, static_cast<size_t>(3));
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(1));
    const auto initialDispatch =
        Rigel::Voxel::detail::ChunkStreamerTestAccess::
            inFlightMeshDispatchOrder(streamer);
    CHECK_EQ(initialDispatch.size(), static_cast<size_t>(1));
    CHECK_NE(initialDispatch.front(), nearCoord);

    Chunk& dependency = manager.getOrCreateChunk(missingDependency);
    dependency.setWorldGenVersion(generator->config().world.version);
    dependency.setLoadedFromDisk(true);
    dependency.clearDirty();
    streamer.update(nearCoord.toWorldCenter());
    streamer.update(nearCoord.toWorldCenter());
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(1));

    gate->release();
    CHECK(waitForMeshCompletions(streamer, 1));
    CHECK_EQ(streamer.workMetrics().meshJobsAccepted, static_cast<uint64_t>(1));
    CHECK_EQ(streamer.workMetrics().meshJobsRejectedStale,
             static_cast<uint64_t>(0));
    streamer.update(nearCoord.toWorldCenter());

    const auto dispatchOrder =
        Rigel::Voxel::detail::ChunkStreamerTestAccess::
            inFlightMeshDispatchOrder(streamer);
    CHECK_EQ(dispatchOrder.size(), static_cast<size_t>(1));
    CHECK_EQ(dispatchOrder.front(), nearCoord);
    CHECK_EQ(
        Rigel::Voxel::detail::ChunkStreamerTestAccess::inFlightMeshMissing(
            streamer),
        static_cast<size_t>(1));
    CHECK_EQ(streamer.diagnostics().mesh.pending, static_cast<size_t>(2));
    CHECK(waitForMeshCompletions(streamer, 2));
    CHECK_EQ(streamer.workMetrics().meshJobsAccepted, static_cast<uint64_t>(2));
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
    CHECK_EQ(streamer.workMetrics().lastUpdateSchedulerCoordinatesInspected,
             static_cast<uint64_t>(3));
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
             static_cast<uint64_t>(2));

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

TEST_CASE(ChunkStreamer_CameraMovementReprioritizesPendingMeshes) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    const BlockID solid = registerTestBlock(
        registry, "rigel:moved_mesh_priority_solid");
    const ChunkCoord initialCenter{0, 0, 0};
    const ChunkCoord blockerCoord{0, 0, 0};
    const ChunkCoord olderFarCoord{1, 0, 0};
    const ChunkCoord movedNearCoord{2, 0, 0};

    for (int z = -2; z <= 2; ++z) {
        for (int y = -2; y <= 2; ++y) {
            for (int x = -2; x <= 4; ++x) {
                const ChunkCoord coord{x, y, z};
                const bool initiallyDesired =
                    x * x + y * y + z * z <= 4;
                const int movedX = x - movedNearCoord.x;
                const bool desiredAfterMovement =
                    movedX * movedX + y * y + z * z <= 4;
                if (!initiallyDesired && !desiredAfterMovement) {
                    continue;
                }
                Chunk& chunk = manager.getOrCreateChunk(coord);
                chunk.setWorldGenVersion(generator->config().world.version);
                chunk.setLoadedFromDisk(true);
                chunk.clearPersistDirty();
                chunk.clearDirty();
            }
        }
    }

    Chunk& blocker = *manager.getChunk(blockerCoord);
    blocker.setBlock(0, 0, 0, BlockState{solid}, registry);
    blocker.clearPersistDirty();
    blocker.clearDirty();
    meshStore.set(blockerCoord, {});
    blocker.invalidateMesh();

    auto gate = std::make_shared<WorkerGate>();
    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 2;
    stream.unloadDistanceChunks = 4;
    stream.genQueueLimit = 0;
    stream.meshQueueLimit = 1;
    stream.updateBudgetPerFrame = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 2;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    streamer.markSpawnDiscoveryComplete();
    streamer.prioritizeMesh(blockerCoord);
    WorkerGateRelease releaseOnExit(gate);
    Rigel::Voxel::detail::ChunkStreamerTestAccess::setMeshBuildStartCallback(
        streamer, [gate]() { gate->enterAndWait(); });

    streamer.update(initialCenter.toWorldCenter());
    CHECK(gate->waitUntilEntered());
    const auto blockerRequest =
        Rigel::Voxel::detail::ChunkStreamerTestAccess::inFlightMeshRequestId(
            streamer, blockerCoord);
    CHECK(blockerRequest.has_value());

    Chunk& olderFar = *manager.getChunk(olderFarCoord);
    olderFar.setBlock(0, 0, 0, BlockState{solid}, registry);
    olderFar.clearPersistDirty();
    meshStore.set(olderFarCoord, {});
    Chunk& movedNear = *manager.getChunk(movedNearCoord);
    movedNear.setBlock(0, 0, 0, BlockState{solid}, registry);
    movedNear.clearPersistDirty();
    meshStore.set(movedNearCoord, {});
    CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::queuePendingDirtyMesh(
        streamer, olderFarCoord));
    CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::queuePendingDirtyMesh(
        streamer, movedNearCoord));
    streamer.update(initialCenter.toWorldCenter());

    const auto olderSequence =
        Rigel::Voxel::detail::ChunkStreamerTestAccess::pendingMeshSequence(
            streamer, olderFarCoord);
    const auto newerSequence =
        Rigel::Voxel::detail::ChunkStreamerTestAccess::pendingMeshSequence(
            streamer, movedNearCoord);
    const auto initialFarPriority =
        Rigel::Voxel::detail::ChunkStreamerTestAccess::pendingMeshPriority(
            streamer, olderFarCoord);
    const auto initialNearPriority =
        Rigel::Voxel::detail::ChunkStreamerTestAccess::pendingMeshPriority(
            streamer, movedNearCoord);
    CHECK(olderSequence.has_value());
    CHECK(newerSequence.has_value());
    CHECK(*olderSequence < *newerSequence);
    CHECK(initialFarPriority.has_value());
    CHECK(initialNearPriority.has_value());
    CHECK(*initialFarPriority < *initialNearPriority);
    Rigel::Voxel::detail::ChunkStreamerTestAccess::pushStalePendingMeshHead(
        streamer, movedNearCoord);
    CHECK_EQ(
        Rigel::Voxel::detail::ChunkStreamerTestAccess::
            pendingMeshQueueRecordCount(streamer),
        static_cast<size_t>(3));

    const uint64_t schedulerInspectionsBeforeMovement =
        streamer.workMetrics().schedulerCoordinatesInspected;
    streamer.update(movedNearCoord.toWorldCenter());
    CHECK_EQ(streamer.workMetrics().lastUpdateSchedulerCoordinatesInspected,
             static_cast<uint64_t>(36));
    CHECK_EQ(
        streamer.workMetrics().schedulerCoordinatesInspected -
            schedulerInspectionsBeforeMovement,
        static_cast<uint64_t>(36));

    const auto movedFarPriority =
        Rigel::Voxel::detail::ChunkStreamerTestAccess::pendingMeshPriority(
            streamer, olderFarCoord);
    const auto movedNearPriority =
        Rigel::Voxel::detail::ChunkStreamerTestAccess::pendingMeshPriority(
            streamer, movedNearCoord);
    CHECK(movedFarPriority.has_value());
    CHECK(movedNearPriority.has_value());
    CHECK(*movedNearPriority < *movedFarPriority);
    CHECK_EQ(
        Rigel::Voxel::detail::ChunkStreamerTestAccess::pendingMeshSequence(
            streamer, olderFarCoord),
        olderSequence);
    CHECK_EQ(
        Rigel::Voxel::detail::ChunkStreamerTestAccess::pendingMeshSequence(
            streamer, movedNearCoord),
        newerSequence);
    CHECK_EQ(
        Rigel::Voxel::detail::ChunkStreamerTestAccess::
            pendingMeshQueueRecordCount(streamer),
        static_cast<size_t>(2));
    CHECK_EQ(
        Rigel::Voxel::detail::ChunkStreamerTestAccess::inFlightMeshRequestId(
            streamer, blockerCoord),
        blockerRequest);
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(1));
    CHECK_EQ(streamer.diagnostics().mesh.pending, static_cast<size_t>(2));
    CHECK_EQ(streamer.diagnostics().mesh.inFlight, static_cast<size_t>(1));

    Rigel::Voxel::detail::ChunkStreamerTestAccess::pushStalePendingMeshHead(
        streamer, movedNearCoord);
    streamer.update(movedNearCoord.toWorldCenter());
    CHECK_EQ(streamer.workMetrics().lastUpdateDesiredBuildCoordinatesInspected,
             static_cast<uint64_t>(0));
    CHECK_EQ(streamer.workMetrics().lastUpdateResidentEvictionCoordinatesInspected,
             static_cast<uint64_t>(0));
    CHECK_EQ(streamer.workMetrics().lastUpdateSchedulerCoordinatesInspected,
             static_cast<uint64_t>(0));
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(1));

    gate->release();
    CHECK(waitForMeshCompletions(streamer, 1));
    streamer.update(movedNearCoord.toWorldCenter());
    auto dispatchOrder =
        Rigel::Voxel::detail::ChunkStreamerTestAccess::
            inFlightMeshDispatchOrder(streamer);
    CHECK_EQ(dispatchOrder.size(), static_cast<size_t>(1));
    CHECK_EQ(dispatchOrder.front(), movedNearCoord);
    CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::hasReadyPendingMesh(
        streamer, olderFarCoord));
    CHECK(!Rigel::Voxel::detail::ChunkStreamerTestAccess::hasReadyPendingMesh(
        streamer, movedNearCoord));
    CHECK(waitForMeshCompletions(streamer, 2));

    streamer.update(movedNearCoord.toWorldCenter());
    dispatchOrder = Rigel::Voxel::detail::ChunkStreamerTestAccess::
        inFlightMeshDispatchOrder(streamer);
    CHECK_EQ(dispatchOrder.size(), static_cast<size_t>(1));
    CHECK_EQ(dispatchOrder.front(), olderFarCoord);
    CHECK(waitForMeshCompletions(streamer, 3));

    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(3));
    CHECK_EQ(streamer.workMetrics().meshJobsCompleted,
             static_cast<uint64_t>(3));
    CHECK_EQ(streamer.workMetrics().meshJobsAccepted, static_cast<uint64_t>(3));
    CHECK_EQ(streamer.workMetrics().meshJobsRejectedStale,
             static_cast<uint64_t>(0));
    CHECK_EQ(streamer.workMetrics().meshJobsFailed, static_cast<uint64_t>(0));
    CHECK(!olderFar.isDirty());
    CHECK(!movedNear.isDirty());
    CHECK(streamer.diagnostics().mesh.empty());
    CHECK_EQ(
        Rigel::Voxel::detail::ChunkStreamerTestAccess::
            pendingMeshQueueRecordCount(streamer),
        static_cast<size_t>(0));

    for (uint32_t stable = 0;
         stable < StreamingDiagnosticSnapshot::QuiescenceUpdateWindow;
         ++stable) {
        streamer.update(movedNearCoord.toWorldCenter());
        streamer.processCompletions();
    }
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(3));
    CHECK_EQ(streamer.workMetrics().lastUpdateSchedulerCoordinatesInspected,
             static_cast<uint64_t>(0));
    CHECK_EQ(streamer.diagnostics().state,
             StreamingLifecycleState::Quiescent);
}

TEST_CASE(ChunkStreamer_MeshSubmissionDoesNotExceedWorkerCount) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    const BlockID solid =
        registerTestBlock(registry, "rigel:mesh_worker_bound_solid");

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
    }

    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 1;
    stream.unloadDistanceChunks = 1;
    stream.meshQueueLimit = 8;
    stream.workerThreads = 8;
    streamer.setConfig(stream);

    streamer.update(glm::vec3(0.0f));
    CHECK_EQ(streamer.diagnostics().meshWorkerCount, static_cast<size_t>(4));
    CHECK_EQ(streamer.diagnostics().meshSubmissionLimit,
             static_cast<size_t>(4));
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(4));
    CHECK_EQ(streamer.diagnostics().mesh.inFlight, static_cast<size_t>(4));

    streamer.update(glm::vec3(0.0f));
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(4));
    CHECK_EQ(streamer.diagnostics().mesh.inFlight, static_cast<size_t>(4));
}

TEST_CASE(ChunkStreamer_InlineMeshSubmissionUsesOnePhysicalSlot) {
    for (const int workerThreads : {0, 1}) {
        for (const int applyBudget : {0, 1, 64}) {
            ChunkManager manager;
            BlockRegistry registry;
            WorldMeshStore meshStore;
            auto generator = makeGenerator(registry);
            const BlockID solid = registerTestBlock(
                registry,
                "rigel:inline_mesh_slot_" +
                    std::to_string(workerThreads) + "_" +
                    std::to_string(applyBudget));
            const std::array<ChunkCoord, 3> expectedDispatch{
                ChunkCoord{0, 0, 0},
                ChunkCoord{1, 0, 0},
                ChunkCoord{2, 0, 0}
            };

            for (int z = -2; z <= 2; ++z) {
                for (int y = -2; y <= 2; ++y) {
                    for (int x = -2; x <= 2; ++x) {
                        if (x * x + y * y + z * z > 4) {
                            continue;
                        }
                        Chunk& chunk = manager.getOrCreateChunk({x, y, z});
                        chunk.setWorldGenVersion(
                            generator->config().world.version);
                        chunk.setLoadedFromDisk(true);
                        chunk.clearPersistDirty();
                        chunk.clearDirty();
                    }
                }
            }
            for (const ChunkCoord& coord : expectedDispatch) {
                Chunk& chunk = *manager.getChunk(coord);
                chunk.setBlock(0, 0, 0, BlockState{solid}, registry);
                chunk.clearPersistDirty();
                chunk.clearDirty();
            }

            size_t physicalBuilds = 0;
            ChunkStreamer streamer(
                manager, meshStore, registry, nullptr, generator);
            StreamingConfig stream;
            stream.viewDistanceChunks = 2;
            stream.unloadDistanceChunks = 2;
            stream.meshQueueLimit = 8;
            stream.updateBudgetPerFrame = 0;
            stream.applyBudgetPerFrame = applyBudget;
            stream.workerThreads = workerThreads;
            stream.maxResidentChunks = 0;
            streamer.setConfig(stream);
            streamer.markSpawnDiscoveryComplete();
            Rigel::Voxel::detail::ChunkStreamerTestAccess::
                setMeshBuildStartCallback(streamer, [&]() {
                    ++physicalBuilds;
                });

            CHECK_EQ(
                Rigel::Voxel::detail::ChunkStreamerTestAccess::
                    meshDispatchLimit(streamer),
                static_cast<size_t>(1));
            CHECK_EQ(streamer.diagnostics().meshWorkerCount,
                     static_cast<size_t>(0));
            CHECK_EQ(streamer.diagnostics().meshSubmissionLimit,
                     static_cast<size_t>(1));

            for (size_t dispatch = 0;
                 dispatch < expectedDispatch.size();
                 ++dispatch) {
                streamer.update(expectedDispatch.front().toWorldCenter());

                CHECK_EQ(streamer.workMetrics().meshJobsStarted,
                         static_cast<uint64_t>(dispatch + 1));
                CHECK_EQ(streamer.workMetrics().meshJobsCompleted,
                         static_cast<uint64_t>(dispatch));
                CHECK_EQ(streamer.workMetrics().meshJobsAccepted,
                         static_cast<uint64_t>(dispatch));
                CHECK_EQ(physicalBuilds, dispatch + 1);
                CHECK_EQ(streamer.diagnostics().mesh.inFlight,
                         static_cast<size_t>(1));
                CHECK_EQ(streamer.diagnostics().mesh.pending,
                         expectedDispatch.size() - dispatch - 1);
                CHECK_EQ(
                    Rigel::Voxel::detail::ChunkStreamerTestAccess::
                        meshCompletionCount(streamer),
                    static_cast<size_t>(1));
                const auto inFlight = Rigel::Voxel::detail::
                    ChunkStreamerTestAccess::inFlightMeshDispatchOrder(
                        streamer);
                CHECK_EQ(inFlight.size(), static_cast<size_t>(1));
                CHECK_EQ(inFlight.front(), expectedDispatch[dispatch]);

                streamer.processCompletions();
                CHECK_EQ(streamer.workMetrics().meshJobsCompleted,
                         static_cast<uint64_t>(dispatch + 1));
                CHECK_EQ(streamer.workMetrics().meshJobsAccepted,
                         static_cast<uint64_t>(dispatch + 1));
                CHECK_EQ(streamer.workMetrics().meshJobsRejectedStale,
                         static_cast<uint64_t>(0));
                CHECK_EQ(streamer.workMetrics().meshJobsFailed,
                         static_cast<uint64_t>(0));
                CHECK_EQ(streamer.diagnostics().mesh.inFlight,
                         static_cast<size_t>(0));
                CHECK_EQ(streamer.diagnostics().mesh.pending,
                         expectedDispatch.size() - dispatch - 1);
            }

            CHECK_EQ(physicalBuilds, expectedDispatch.size());
            CHECK(streamer.diagnostics().mesh.empty());
            for (uint32_t stable = 0;
                 stable < StreamingDiagnosticSnapshot::QuiescenceUpdateWindow;
                 ++stable) {
                streamer.update(expectedDispatch.front().toWorldCenter());
                streamer.processCompletions();
            }
            CHECK_EQ(streamer.workMetrics().meshJobsStarted,
                     static_cast<uint64_t>(expectedDispatch.size()));
            CHECK_EQ(
                streamer.workMetrics().lastUpdateSchedulerCoordinatesInspected,
                static_cast<uint64_t>(0));
            CHECK_EQ(streamer.diagnostics().state,
                     StreamingLifecycleState::Quiescent);
        }
    }
}

TEST_CASE(ChunkStreamer_PendingMeshesSettleAfterInlineWorkerTransition) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    const BlockID solid =
        registerTestBlock(registry, "rigel:inline_pending_mesh_solid");

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
    }

    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 1;
    stream.unloadDistanceChunks = 1;
    stream.meshQueueLimit = 1;
    stream.workerThreads = 2;
    streamer.setConfig(stream);

    streamer.update(glm::vec3(0.0f));
    CHECK(waitForMeshCompletions(streamer, 1));
    CHECK_EQ(streamer.diagnostics().mesh.inFlight, static_cast<size_t>(0));
    CHECK(streamer.diagnostics().mesh.pending > 0);

    stream.workerThreads = 0;
    streamer.setConfig(stream);
    for (size_t update = 0; update < desired.size() * 2; ++update) {
        streamer.update(glm::vec3(0.0f));
        streamer.processCompletions();
        if (streamer.diagnostics().mesh.empty()) {
            break;
        }
    }

    CHECK_EQ(streamer.workMetrics().meshJobsStarted,
             static_cast<uint64_t>(desired.size()));
    CHECK_EQ(streamer.workMetrics().meshJobsAccepted,
             static_cast<uint64_t>(desired.size()));
    CHECK(streamer.diagnostics().mesh.empty());
    const uint64_t settledJobs = streamer.workMetrics().meshJobsStarted;
    streamer.update(glm::vec3(0.0f));
    streamer.processCompletions();
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, settledJobs);
    CHECK_EQ(streamer.workMetrics().lastUpdateSchedulerCoordinatesInspected,
             static_cast<uint64_t>(0));
}

TEST_CASE(ChunkStreamer_DependencyLossMovesReadyPendingOwnership) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    const BlockID solid =
        registerTestBlock(registry, "rigel:pending_dependency_solid");
    const ChunkCoord blockerCoord{0, 0, 0};
    const ChunkCoord pendingCoord{1, 0, 0};

    const std::array<ChunkCoord, 7> desired{
        blockerCoord,
        pendingCoord,
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
        if (coord == blockerCoord || coord == pendingCoord) {
            chunk.setBlock(0, 0, 0, BlockState{solid}, registry);
        }
        chunk.clearPersistDirty();
        chunk.clearDirty();
    }
    meshStore.set(blockerCoord, {});
    meshStore.set(pendingCoord, {});
    manager.getChunk(blockerCoord)->invalidateMesh();
    manager.getChunk(pendingCoord)->invalidateMesh();

    auto gate = std::make_shared<WorkerGate>();
    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 1;
    stream.unloadDistanceChunks = 1;
    stream.meshQueueLimit = 1;
    stream.workerThreads = 2;
    streamer.setConfig(stream);
    streamer.prioritizeMesh(blockerCoord);
    Rigel::Voxel::detail::ChunkStreamerTestAccess::setMeshBuildStartCallback(
        streamer, [gate]() { gate->enterAndWait(); });
    WorkerGateRelease releaseOnExit(gate);

    streamer.update(glm::vec3(0.0f));
    CHECK(gate->waitUntilEntered());
    for (size_t update = 0;
         update < desired.size() &&
         !Rigel::Voxel::detail::ChunkStreamerTestAccess::hasReadyPendingMesh(
             streamer, pendingCoord);
         ++update) {
        streamer.update(glm::vec3(0.0f));
    }
    CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::hasReadyPendingMesh(
        streamer, pendingCoord));
    CHECK(!Rigel::Voxel::detail::ChunkStreamerTestAccess::
        hasDependencyPendingMesh(streamer, pendingCoord));

    CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::evictChunk(
        streamer, blockerCoord));
    streamer.update(glm::vec3(0.0f));

    CHECK(!Rigel::Voxel::detail::ChunkStreamerTestAccess::hasReadyPendingMesh(
        streamer, pendingCoord));
    CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::
        hasDependencyPendingMesh(streamer, pendingCoord));
    CHECK_EQ(streamer.diagnostics().mesh.pending, static_cast<size_t>(1));
    std::vector<ChunkStreamer::DebugChunkState> states;
    streamer.getDebugStates(states, pendingCoord, 0);
    CHECK_EQ(states.size(), static_cast<size_t>(1));
    CHECK_EQ(states.front().state,
             ChunkStreamer::DebugState::WaitingForNeighbors);
    CHECK_EQ(states.front().pipelineOwner,
             ChunkStreamer::DebugPipelineOwner::WaitingForNeighbors);
    CHECK_EQ(states.front().voxelOccupancy,
             ChunkStreamer::DebugVoxelOccupancy::Nonempty);
}

TEST_CASE(ChunkStreamer_DebugSnapshotDistinguishesMeshOwnership) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    const BlockID solid =
        registerTestBlock(registry, "rigel:debug_snapshot_mesh_solid");
    const ChunkCoord blocker{0, 0, 0};
    const ChunkCoord schedulerWait{1, 0, 0};
    const ChunkCoord remeshWait{-1, 0, 0};
    const std::array<ChunkCoord, 7> desired{
        blocker,
        schedulerWait,
        remeshWait,
        ChunkCoord{0, 1, 0},
        ChunkCoord{0, -1, 0},
        ChunkCoord{0, 0, 1},
        ChunkCoord{0, 0, -1}
    };
    for (const ChunkCoord coord : desired) {
        Chunk& chunk = manager.getOrCreateChunk(coord);
        chunk.setWorldGenVersion(generator->config().world.version);
        chunk.setLoadedFromDisk(true);
        chunk.clearPersistDirty();
        chunk.clearDirty();
    }
    manager.getChunk(blocker)->setBlock(
        0, 0, 0, BlockState{solid}, registry);
    manager.getChunk(schedulerWait)->setBlock(
        0, 0, 0, BlockState{solid}, registry);
    manager.getChunk(remeshWait)->setBlock(
        0, 0, 0, BlockState{solid}, registry);

    ChunkMesh installed;
    installed.vertices.resize(3);
    installed.indices.resize(3);
    installed.layers[static_cast<size_t>(RenderLayer::Opaque)].indexCount = 3;
    meshStore.set(remeshWait, std::move(installed));
    manager.getChunk(remeshWait)->clearDirty();
    manager.getChunk(remeshWait)->markDirty();

    auto gate = std::make_shared<WorkerGate>();
    auto tracer = std::make_shared<ChunkVisibilityTracer>(
        ChunkVisibilityTracer::Config{blocker, 2});
    ChunkStreamer streamer(
        manager, meshStore, registry, nullptr, generator);
    WorkerGateRelease releaseOnExit(gate);
    StreamingConfig stream;
    stream.viewDistanceChunks = 1;
    stream.unloadDistanceChunks = 1;
    stream.meshQueueLimit = 1;
    stream.workerThreads = 2;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    streamer.setVisibilityTracer(tracer);
    streamer.markSpawnDiscoveryComplete();
    Rigel::Voxel::detail::ChunkStreamerTestAccess::setMeshBuildStartCallback(
        streamer, [gate]() { gate->enterAndWait(); });

    streamer.update(blocker.toWorldCenter());
    CHECK(gate->waitUntilEntered());

    std::vector<ChunkStreamer::DebugChunkState> states;
    streamer.getDebugStates(states, blocker, 1);
    auto stateFor = [&](ChunkCoord coord)
        -> const ChunkStreamer::DebugChunkState* {
        const auto stateIt = std::find_if(
            states.begin(), states.end(), [coord](const auto& state) {
                return state.coord == coord;
            });
        return stateIt == states.end() ? nullptr : &*stateIt;
    };

    const auto* building = stateFor(blocker);
    CHECK(building != nullptr);
    if (building) {
        CHECK_EQ(building->state,
                 ChunkStreamer::DebugState::MeshSubmittedOrBuilding);
        CHECK_EQ(building->pipelineOwner,
                 ChunkStreamer::DebugPipelineOwner::MeshWork);
        CHECK_EQ(building->installedGeometry,
                 ChunkStreamer::DebugInstalledGeometry::None);
        CHECK_EQ(building->historicalTraceOutcome,
                 ChunkVisibilityOutcome::Pending);
        CHECK_EQ(building->drawEvidence,
                 ChunkStreamer::DebugDrawEvidence::NotApplicable);
    }

    const auto* eligible = stateFor(schedulerWait);
    CHECK(eligible != nullptr);
    if (eligible) {
        CHECK_EQ(eligible->state,
                 ChunkStreamer::DebugState::MeshSchedulerWait);
        CHECK_EQ(eligible->pipelineOwner,
                 ChunkStreamer::DebugPipelineOwner::MeshScheduler);
        CHECK_EQ(eligible->remeshIntent,
                 ChunkStreamer::DebugRemeshIntent::None);
    }

    const auto* remesh = stateFor(remeshWait);
    CHECK(remesh != nullptr);
    if (remesh) {
        CHECK_EQ(remesh->state,
                 ChunkStreamer::DebugState::DirtyRemeshPending);
        CHECK_EQ(remesh->pipelineOwner,
                 ChunkStreamer::DebugPipelineOwner::MeshScheduler);
        CHECK_EQ(remesh->installedGeometry,
                 ChunkStreamer::DebugInstalledGeometry::Nonempty);
        CHECK_EQ(remesh->remeshIntent,
                 ChunkStreamer::DebugRemeshIntent::Pending);
        CHECK_EQ(remesh->drawEvidence,
                 ChunkStreamer::DebugDrawEvidence::NotDrawn);
    }

    CHECK_EQ(streamer.diagnostics().mesh.inFlight, static_cast<size_t>(1));
    CHECK_EQ(streamer.diagnostics().mesh.pending, static_cast<size_t>(2));
    gate->release();
    CHECK(waitForMeshCompletions(streamer, 1));
}

TEST_CASE(ChunkStreamer_DebugSnapshotClampsOversizedProgrammaticRadius) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);

    const ChunkCoord center{0, 0, 0};
    const ChunkCoord edge{StreamingConfig::MaxViewDistanceChunks, 0, 0};
    const ChunkCoord outside{
        StreamingConfig::MaxViewDistanceChunks + 1, 0, 0};
    Rigel::Voxel::detail::ChunkStreamerTestAccess::injectLoadGenOwner(
        streamer, center);
    Rigel::Voxel::detail::ChunkStreamerTestAccess::injectLoadGenOwner(
        streamer, edge);
    Rigel::Voxel::detail::ChunkStreamerTestAccess::injectLoadGenOwner(
        streamer, outside);

    const ChunkStreamer::WorkMetrics metricsBefore = streamer.workMetrics();
    std::vector<ChunkStreamer::DebugChunkState> states;
    streamer.getDebugStates(
        states, center, std::numeric_limits<int>::max());

    CHECK_EQ(states.size(), static_cast<size_t>(2));
    CHECK(std::any_of(states.begin(), states.end(), [&](const auto& state) {
        return state.coord == center;
    }));
    CHECK(std::any_of(states.begin(), states.end(), [&](const auto& state) {
        return state.coord == edge;
    }));
    CHECK(std::none_of(states.begin(), states.end(), [&](const auto& state) {
        return state.coord == outside;
    }));
    CHECK(std::all_of(states.begin(), states.end(), [&](const auto& state) {
        const int64_t dx =
            static_cast<int64_t>(state.coord.x) - center.x;
        const int64_t dy =
            static_cast<int64_t>(state.coord.y) - center.y;
        const int64_t dz =
            static_cast<int64_t>(state.coord.z) - center.z;
        return std::abs(dx) <= StreamingConfig::MaxViewDistanceChunks &&
            std::abs(dy) <= StreamingConfig::MaxViewDistanceChunks &&
            std::abs(dz) <= StreamingConfig::MaxViewDistanceChunks;
    }));
    CHECK_EQ(streamer.workMetrics().schedulerCoordinatesInspected,
             metricsBefore.schedulerCoordinatesInspected);
    CHECK_EQ(streamer.workMetrics().meshJobsStarted,
             metricsBefore.meshJobsStarted);

    streamer.getDebugStates(
        states, center, std::numeric_limits<int>::min());
    CHECK_EQ(states.size(), static_cast<size_t>(1));
    CHECK_EQ(states.front().coord, center);
}

TEST_CASE(ChunkStreamer_DebugSnapshotKeepsSettledReconciliationComplete) {
    for (const bool nonempty : {false, true}) {
        ChunkManager manager;
        BlockRegistry registry;
        WorldMeshStore meshStore;
        auto generator = makeGenerator(registry);
        const ChunkCoord coord{0, 0, 0};
        Chunk& chunk = manager.getOrCreateChunk(coord);
        chunk.setWorldGenVersion(generator->config().world.version);
        chunk.setLoadedFromDisk(true);
        if (nonempty) {
            const BlockID solid = registerTestBlock(
                registry, "rigel:settled_reconciliation_solid");
            chunk.setBlock(0, 0, 0, BlockState{solid}, registry);
            ChunkMesh installed;
            installed.vertices.resize(3);
            installed.indices = {0, 1, 2};
            installed.layers[
                static_cast<size_t>(RenderLayer::Opaque)].indexCount = 3;
            meshStore.set(coord, std::move(installed));
        }
        chunk.clearPersistDirty();
        chunk.clearDirty();

        ChunkStreamer streamer(
            manager, meshStore, registry, nullptr, generator);
        StreamingConfig stream;
        stream.viewDistanceChunks = 0;
        stream.unloadDistanceChunks = 0;
        stream.workerThreads = 0;
        stream.maxResidentChunks = 0;
        streamer.setConfig(stream);
        streamer.update(coord.toWorldCenter());
        streamer.processCompletions();

        Rigel::Voxel::detail::ChunkStreamerTestAccess::injectLoadGenOwner(
            streamer, coord);
        std::vector<ChunkStreamer::DebugChunkState> states;
        streamer.getDebugStates(states, coord, 0);
        CHECK_EQ(states.size(), static_cast<size_t>(1));
        CHECK_EQ(states.front().pipelineOwner,
                 ChunkStreamer::DebugPipelineOwner::Complete);
        CHECK_EQ(
            states.front().state,
            nonempty
                ? ChunkStreamer::DebugState::AcceptedNonemptyGeometry
                : ChunkStreamer::DebugState::VoxelEmpty);
        CHECK_NE(states.front().state,
                 ChunkStreamer::DebugState::WaitingForNeighbors);
    }
}

TEST_CASE(ChunkStreamer_VoxelEmptyRemovesInstalledMeshWithoutDependencies) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    const BlockID solid =
        registerTestBlock(registry, "rigel:voxel_empty_retirement_solid");
    const ChunkCoord blockerCoord{0, 0, 0};
    const ChunkCoord emptyCoord{10, 0, 0};

    Chunk& blocker = manager.getOrCreateChunk(blockerCoord);
    blocker.setBlock(0, 0, 0, BlockState{solid}, registry);
    blocker.setWorldGenVersion(generator->config().world.version);
    blocker.setLoadedFromDisk(true);
    blocker.clearDirty();
    for (int i = 0; i < DirectionCount; ++i) {
        int dx = 0;
        int dy = 0;
        int dz = 0;
        directionOffset(static_cast<Direction>(i), dx, dy, dz);
        Chunk& neighbor = manager.getOrCreateChunk(
            blockerCoord.offset(dx, dy, dz));
        neighbor.setWorldGenVersion(generator->config().world.version);
        neighbor.setLoadedFromDisk(true);
        neighbor.clearDirty();
    }

    Chunk& emptied = manager.getOrCreateChunk(emptyCoord);
    emptied.setWorldGenVersion(generator->config().world.version);
    emptied.setLoadedFromDisk(true);
    emptied.setBlock(0, 0, 0, BlockState{solid}, registry);
    emptied.clearDirty();
    meshStore.set(emptyCoord, {});
    emptied.setBlock(0, 0, 0, BlockState{}, registry);
    emptied.clearPersistDirty();

    auto gate = std::make_shared<WorkerGate>();
    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 1;
    stream.meshQueueLimit = 1;
    stream.workerThreads = 2;
    streamer.setConfig(stream);
    streamer.markSpawnDiscoveryComplete();
    Rigel::Voxel::detail::ChunkStreamerTestAccess::setMeshBuildStartCallback(
        streamer, [gate]() { gate->enterAndWait(); });
    WorkerGateRelease releaseOnExit(gate);

    streamer.update(blockerCoord.toWorldCenter());
    CHECK(gate->waitUntilEntered());
    CHECK(!meshStore.contains(emptyCoord));
    CHECK(!emptied.isDirty());
    CHECK(!Rigel::Voxel::detail::ChunkStreamerTestAccess::hasReadyPendingMesh(
        streamer, emptyCoord));
    CHECK(!Rigel::Voxel::detail::ChunkStreamerTestAccess::
        hasDependencyPendingMesh(streamer, emptyCoord));
    CHECK_EQ(streamer.diagnostics().mesh.pending, static_cast<size_t>(0));

    gate->release();
    CHECK(waitForMeshCompletions(streamer, 1));
    for (size_t update = 0;
         update < 8 &&
         streamer.diagnostics().state != StreamingLifecycleState::Quiescent;
         ++update) {
        streamer.update(blockerCoord.toWorldCenter());
        streamer.processCompletions();
    }
    CHECK(streamer.diagnostics().mesh.empty());
    CHECK_EQ(streamer.diagnostics().state, StreamingLifecycleState::Quiescent);
    CHECK_EQ(streamer.workMetrics().lastUpdateSchedulerCoordinatesInspected,
             static_cast<uint64_t>(0));
}

TEST_CASE(ChunkStreamer_PendingMeshHeapStorageFollowsCanonicalOwnership) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    const BlockID solid =
        registerTestBlock(registry, "rigel:pending_heap_storage_solid");
    const ChunkCoord coord{8, 0, 0};

    Chunk& chunk = manager.getOrCreateChunk(coord);
    chunk.setBlock(0, 0, 0, BlockState{solid}, registry);
    chunk.setWorldGenVersion(generator->config().world.version);
    chunk.setLoadedFromDisk(true);
    chunk.clearDirty();
    chunk.invalidateMesh();
    meshStore.set(coord, {});

    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 0;
    stream.meshQueueLimit = 1;
    stream.workerThreads = 0;
    streamer.setConfig(stream);

    for (size_t cycle = 0; cycle < 64; ++cycle) {
        Rigel::Voxel::detail::ChunkStreamerTestAccess::queuePendingDirtyMesh(
            streamer, coord);
        CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::
            hasReadyPendingMesh(streamer, coord));
        CHECK(!Rigel::Voxel::detail::ChunkStreamerTestAccess::
            hasDependencyPendingMesh(streamer, coord));
        Rigel::Voxel::detail::ChunkStreamerTestAccess::waitForMeshDependencies(
            streamer, coord);
        CHECK(!Rigel::Voxel::detail::ChunkStreamerTestAccess::
            hasReadyPendingMesh(streamer, coord));
        CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::
            hasDependencyPendingMesh(streamer, coord));
        CHECK_EQ(
            Rigel::Voxel::detail::ChunkStreamerTestAccess::
                pendingMeshQueueRecordCount(streamer),
            static_cast<size_t>(0));
        Rigel::Voxel::detail::ChunkStreamerTestAccess::retirePendingMesh(
            streamer, coord);
    }

    Rigel::Voxel::detail::ChunkStreamerTestAccess::queuePendingDirtyMesh(
        streamer, coord);
    for (size_t incarnation = 0; incarnation < 64; ++incarnation) {
        if (incarnation % 2 == 0) {
            Rigel::Voxel::detail::ChunkStreamerTestAccess::
                queuePendingMissingMesh(streamer, coord);
        } else {
            Rigel::Voxel::detail::ChunkStreamerTestAccess::
                queuePendingDirtyMesh(streamer, coord);
        }
    }
    CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::
        pendingMeshQueueRecordCount(streamer) <= static_cast<size_t>(10));
    Rigel::Voxel::detail::ChunkStreamerTestAccess::retirePendingMesh(
        streamer, coord);
    CHECK_EQ(
        Rigel::Voxel::detail::ChunkStreamerTestAccess::
            pendingMeshQueueRecordCount(streamer),
        static_cast<size_t>(0));
    CHECK_EQ(streamer.diagnostics().mesh.pending, static_cast<size_t>(0));
}

TEST_CASE(ChunkStreamer_MeshInspectionMetricsCountEveryCandidateVisit) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    const BlockID solid =
        registerTestBlock(registry, "rigel:mesh_inspection_solid");
    const ChunkCoord missingCoord{0, 0, 0};
    const ChunkCoord dirtyCoord{10, 0, 0};

    Chunk& missing = manager.getOrCreateChunk(missingCoord);
    missing.setWorldGenVersion(generator->config().world.version);
    missing.setLoadedFromDisk(true);
    missing.clearDirty();
    for (int i = 0; i < DirectionCount; ++i) {
        int dx = 0;
        int dy = 0;
        int dz = 0;
        directionOffset(static_cast<Direction>(i), dx, dy, dz);
        Chunk& neighbor = manager.getOrCreateChunk(
            missingCoord.offset(dx, dy, dz));
        neighbor.setWorldGenVersion(generator->config().world.version);
        neighbor.setLoadedFromDisk(true);
        neighbor.clearDirty();
    }

    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 10;
    stream.meshQueueLimit = 1;
    stream.workerThreads = 0;
    streamer.setConfig(stream);
    streamer.update(missingCoord.toWorldCenter());
    streamer.processCompletions();

    missing.setBlock(0, 0, 0, BlockState{solid}, registry);
    CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::
        queuePendingMissingMesh(streamer, missingCoord));

    Chunk& dirty = manager.getOrCreateChunk(dirtyCoord);
    dirty.setBlock(0, 0, 0, BlockState{solid}, registry);
    dirty.setWorldGenVersion(generator->config().world.version);
    dirty.setLoadedFromDisk(true);
    dirty.clearDirty();
    meshStore.set(dirtyCoord, {});
    dirty.invalidateMesh();
    CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::
        queuePendingDirtyMesh(streamer, dirtyCoord));
    streamer.prioritizeMesh(dirtyCoord);

    CHECK_EQ(
        Rigel::Voxel::detail::ChunkStreamerTestAccess::
            reprioritizePendingMeshes(streamer),
        static_cast<uint64_t>(3));
    Rigel::Voxel::detail::ChunkStreamerTestAccess::pushStalePendingMeshHead(
        streamer, missingCoord);
    CHECK_EQ(
        Rigel::Voxel::detail::ChunkStreamerTestAccess::
            dispatchPendingMeshes(streamer),
        static_cast<uint64_t>(4));
    CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::hasReadyPendingMesh(
        streamer, missingCoord));
    CHECK(!Rigel::Voxel::detail::ChunkStreamerTestAccess::hasReadyPendingMesh(
        streamer, dirtyCoord));
    streamer.processCompletions();
}

TEST_CASE(ChunkStreamer_UpdateMetricsIncludePendingMeshCompactionVisits) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    const BlockID solid =
        registerTestBlock(registry, "rigel:mesh_compaction_metric_solid");
    const ChunkCoord center{0, 0, 0};
    const std::array<ChunkCoord, 3> pendingCoords{{
        {10, 0, 0},
        {20, 0, 0},
        {30, 0, 0},
    }};

    Chunk& centerChunk = manager.getOrCreateChunk(center);
    centerChunk.setWorldGenVersion(generator->config().world.version);
    centerChunk.setLoadedFromDisk(true);
    centerChunk.clearDirty();

    for (const ChunkCoord& coord : pendingCoords) {
        Chunk& chunk = manager.getOrCreateChunk(coord);
        chunk.setBlock(0, 0, 0, BlockState{solid}, registry);
        chunk.setWorldGenVersion(generator->config().world.version);
        chunk.setLoadedFromDisk(true);
    }

    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 64;
    stream.meshQueueLimit = 1;
    stream.workerThreads = 0;
    streamer.setConfig(stream);
    streamer.update(center.toWorldCenter());

    for (const ChunkCoord& coord : pendingCoords) {
        meshStore.set(coord, {});
        CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::
            queuePendingDirtyMesh(streamer, coord));
    }

    Rigel::Voxel::detail::ChunkStreamerTestAccess::pushStalePendingMeshTail(
        streamer, pendingCoords.front(), 12);
    CHECK_EQ(
        Rigel::Voxel::detail::ChunkStreamerTestAccess::readyPendingMeshCount(
            streamer),
        pendingCoords.size());
    CHECK_EQ(
        Rigel::Voxel::detail::ChunkStreamerTestAccess::
            pendingMeshQueueRecordCount(streamer),
        static_cast<size_t>(15));

    const uint64_t inspectedBefore =
        streamer.workMetrics().schedulerCoordinatesInspected;
    streamer.update(center.toWorldCenter());

    constexpr uint64_t dispatchVisits = 2;
    constexpr uint64_t compactedOwnerVisits = pendingCoords.size() - 1;
    constexpr uint64_t expectedVisits =
        dispatchVisits + compactedOwnerVisits;
    CHECK_EQ(
        streamer.workMetrics().lastUpdateSchedulerCoordinatesInspected,
        expectedVisits);
    CHECK_EQ(
        streamer.workMetrics().schedulerCoordinatesInspected - inspectedBefore,
        expectedVisits);
    CHECK_EQ(
        Rigel::Voxel::detail::ChunkStreamerTestAccess::readyPendingMeshCount(
            streamer),
        pendingCoords.size() - 1);
    CHECK_EQ(streamer.diagnostics().mesh.pending, pendingCoords.size() - 1);
}

TEST_CASE(ChunkStreamer_ConfigShrinkRetiresPendingMeshImmediately) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    const BlockID solid =
        registerTestBlock(registry, "rigel:config_shrink_pending_solid");
    const ChunkCoord blockerCoord{0, 0, 0};
    const ChunkCoord pendingCoord{1, 0, 0};

    for (int z = -2; z <= 2; ++z) {
        for (int y = -2; y <= 2; ++y) {
            for (int x = -2; x <= 2; ++x) {
                if (x * x + y * y + z * z > 4) {
                    continue;
                }
                Chunk& chunk = manager.getOrCreateChunk({x, y, z});
                chunk.setWorldGenVersion(generator->config().world.version);
                chunk.setLoadedFromDisk(true);
                chunk.clearDirty();
            }
        }
    }
    manager.getChunk(blockerCoord)->setBlock(
        0, 0, 0, BlockState{solid}, registry);
    manager.getChunk(blockerCoord)->clearDirty();
    meshStore.set(blockerCoord, {});
    manager.getChunk(blockerCoord)->invalidateMesh();
    manager.getChunk(pendingCoord)->setBlock(
        0, 0, 0, BlockState{solid}, registry);
    manager.getChunk(pendingCoord)->clearDirty();

    auto gate = std::make_shared<WorkerGate>();
    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 1;
    stream.unloadDistanceChunks = 2;
    stream.meshQueueLimit = 1;
    stream.workerThreads = 2;
    streamer.setConfig(stream);
    streamer.prioritizeMesh(blockerCoord);
    Rigel::Voxel::detail::ChunkStreamerTestAccess::setMeshBuildStartCallback(
        streamer, [gate]() { gate->enterAndWait(); });
    WorkerGateRelease releaseOnExit(gate);

    streamer.update(blockerCoord.toWorldCenter());
    CHECK(gate->waitUntilEntered());
    const auto initialDispatch =
        Rigel::Voxel::detail::ChunkStreamerTestAccess::
            inFlightMeshDispatchOrder(streamer);
    CHECK_EQ(initialDispatch.size(), static_cast<size_t>(1));
    if (!initialDispatch.empty()) {
        CHECK_EQ(initialDispatch.front(), blockerCoord);
    }
    CHECK(!Rigel::Voxel::detail::ChunkStreamerTestAccess::
        hasDependencyPendingMesh(streamer, pendingCoord));
    CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::hasReadyPendingMesh(
        streamer, pendingCoord));
    CHECK_EQ(streamer.diagnostics().mesh.pending, static_cast<size_t>(1));

    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 0;
    streamer.setConfig(stream);
    CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::desiredContains(
        streamer, pendingCoord));
    CHECK(!Rigel::Voxel::detail::ChunkStreamerTestAccess::hasReadyPendingMesh(
        streamer, pendingCoord));
    CHECK(!Rigel::Voxel::detail::ChunkStreamerTestAccess::
        hasDependencyPendingMesh(streamer, pendingCoord));
    CHECK_EQ(streamer.diagnostics().mesh.pending, static_cast<size_t>(0));
    CHECK_EQ(
        Rigel::Voxel::detail::ChunkStreamerTestAccess::
            pendingMeshQueueRecordCount(streamer),
        static_cast<size_t>(0));
    CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::
        hasConfigRetiredWork(streamer, pendingCoord));

    streamer.update(blockerCoord.toWorldCenter());
    CHECK(!Rigel::Voxel::detail::ChunkStreamerTestAccess::desiredContains(
        streamer, pendingCoord));
    CHECK(!Rigel::Voxel::detail::ChunkStreamerTestAccess::
        hasConfigRetiredWork(streamer, pendingCoord));
}

TEST_CASE(ChunkStreamer_ConfigRetiredDirtyMeshRebuildsAfterDemandReturns) {
    for (const int workerThreads : {0, 1}) {
        for (const bool completionBeforeRestore : {false, true}) {
            ChunkManager manager;
            BlockRegistry registry;
            WorldMeshStore meshStore;
            auto generator = makeGenerator(registry);
            const BlockID solid = registerTestBlock(
                registry,
                "rigel:config_retired_dirty_" +
                    std::to_string(workerThreads) + "_" +
                    std::to_string(completionBeforeRestore));
            const ChunkCoord cameraCoord{0, 4, 0};
            const ChunkCoord remeshCoord{1, 4, 0};

            addLoadedNeighborShell(
                manager,
                remeshCoord,
                std::nullopt,
                generator->config().world.version);
            for (const ChunkCoord& coord : {
                     cameraCoord,
                     cameraCoord.offset(-1, 0, 0),
                     cameraCoord.offset(0, 1, 0),
                     cameraCoord.offset(0, -1, 0),
                     cameraCoord.offset(0, 0, 1),
                     cameraCoord.offset(0, 0, -1)}) {
                Chunk& resident = manager.getOrCreateChunk(coord);
                resident.setWorldGenVersion(
                    generator->config().world.version);
                resident.setLoadedFromDisk(true);
                resident.clearPersistDirty();
                resident.clearDirty();
            }
            Chunk& chunk = manager.getOrCreateChunk(remeshCoord);
            chunk.setBlock(0, 0, 0, BlockState{solid}, registry);
            chunk.setWorldGenVersion(generator->config().world.version);
            chunk.setLoadedFromDisk(true);
            chunk.clearPersistDirty();
            chunk.clearDirty();
            ChunkMesh installedGeometry;
            installedGeometry.vertices.resize(3);
            installedGeometry.indices = {0, 1, 2};
            meshStore.set(remeshCoord, std::move(installedGeometry));
            chunk.invalidateMesh();
            const uint64_t installedStoreVersion = meshStore.version();

            ChunkStreamer streamer(
                manager, meshStore, registry, nullptr, generator);
            StreamingConfig stream;
            stream.viewDistanceChunks = 1;
            stream.unloadDistanceChunks = 1;
            stream.meshQueueLimit = 1;
            stream.workerThreads = workerThreads;
            stream.maxResidentChunks = 0;
            streamer.setConfig(stream);
            streamer.markSpawnDiscoveryComplete();

            streamer.update(cameraCoord.toWorldCenter());
            const auto retiredRequestId =
                Rigel::Voxel::detail::ChunkStreamerTestAccess::
                    inFlightMeshRequestId(streamer, remeshCoord);
            CHECK(retiredRequestId.has_value());
            CHECK_EQ(streamer.workMetrics().meshJobsStarted,
                     static_cast<uint64_t>(1));
            chunk.clearDirty();
            const uint32_t currentRevision = chunk.meshRevision();
            CHECK(!chunk.isDirty());

            stream.viewDistanceChunks = 0;
            stream.unloadDistanceChunks = 0;
            streamer.setConfig(stream);
            CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::
                hasConfigRetiredWork(streamer, remeshCoord));
            CHECK_EQ(streamer.diagnostics().mesh.pending,
                     static_cast<size_t>(0));
            CHECK_EQ(streamer.diagnostics().mesh.inFlight,
                     static_cast<size_t>(1));
            CHECK_EQ(meshStore.version(), installedStoreVersion);

            if (completionBeforeRestore) {
                CHECK(waitForMeshCompletions(streamer, 1));
                CHECK_EQ(streamer.workMetrics().meshJobsRejectedStale,
                         static_cast<uint64_t>(1));
                CHECK_EQ(meshStore.version(), installedStoreVersion);
            }

            stream.viewDistanceChunks = 1;
            stream.unloadDistanceChunks = 1;
            streamer.setConfig(stream);
            CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::
                hasConfigRetiredWork(streamer, remeshCoord));
            CHECK_EQ(streamer.diagnostics().mesh.pending,
                     static_cast<size_t>(1));
            CHECK_NE(streamer.diagnostics().state,
                     StreamingLifecycleState::Quiescent);

            streamer.update(cameraCoord.toWorldCenter());
            CHECK(!Rigel::Voxel::detail::ChunkStreamerTestAccess::
                hasConfigRetiredWork(streamer, remeshCoord));
            CHECK_EQ(streamer.diagnostics().mesh.pending,
                     completionBeforeRestore
                         ? static_cast<size_t>(0)
                         : static_cast<size_t>(1));
            CHECK_EQ(streamer.diagnostics().mesh.inFlight,
                     static_cast<size_t>(1));

            if (!completionBeforeRestore) {
                CHECK(waitForMeshCompletions(streamer, 1));
                CHECK_EQ(streamer.workMetrics().meshJobsRejectedStale,
                         static_cast<uint64_t>(1));
            }

            streamer.update(cameraCoord.toWorldCenter());
            const auto replacementRequestId =
                Rigel::Voxel::detail::ChunkStreamerTestAccess::
                    inFlightMeshRequestId(streamer, remeshCoord);
            CHECK(replacementRequestId.has_value());
            CHECK_NE(replacementRequestId, retiredRequestId);
            CHECK_EQ(
                Rigel::Voxel::detail::ChunkStreamerTestAccess::
                    inFlightMeshObservedRevision(streamer, remeshCoord),
                std::optional<uint32_t>{currentRevision});
            CHECK_EQ(streamer.workMetrics().meshJobsStarted,
                     static_cast<uint64_t>(2));
            CHECK(waitForMeshCompletions(streamer, 2));

            CHECK_EQ(streamer.workMetrics().meshJobsCompleted,
                     static_cast<uint64_t>(2));
            CHECK_EQ(streamer.workMetrics().meshJobsAccepted,
                     static_cast<uint64_t>(1));
            CHECK_EQ(streamer.workMetrics().meshJobsRejectedStale,
                     static_cast<uint64_t>(1));
            CHECK_EQ(chunk.meshRevision(), currentRevision);
            CHECK(!chunk.isDirty());
            CHECK(meshStore.contains(remeshCoord));
            CHECK_EQ(meshStore.version(), installedStoreVersion + 1);
            CHECK(streamer.diagnostics().mesh.empty());

            for (uint32_t stable = 0;
                 stable <= StreamingDiagnosticSnapshot::QuiescenceUpdateWindow;
                 ++stable) {
                streamer.update(cameraCoord.toWorldCenter());
                streamer.processCompletions();
            }
            CHECK_EQ(streamer.workMetrics().meshJobsStarted,
                     static_cast<uint64_t>(2));
            CHECK_EQ(
                streamer.workMetrics().lastUpdateSchedulerCoordinatesInspected,
                static_cast<uint64_t>(0));
            CHECK_EQ(streamer.diagnostics().state,
                     StreamingLifecycleState::Quiescent);
        }
    }
}

TEST_CASE(ChunkStreamer_ConfigRetiredDebugSnapshotsTransferOnce) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    const BlockID solid = registerTestBlock(
        registry, "rigel:config_retired_debug_snapshots");
    const ChunkCoord blocker{0, 0, 0};
    const ChunkCoord missingMesh{2, 0, 0};
    const ChunkCoord dirtyMesh{-2, 0, 0};
    const ChunkCoord loadGen{0, 2, 0};

    constexpr int viewDistance = 2;
    for (int z = -viewDistance; z <= viewDistance; ++z) {
        for (int y = -viewDistance; y <= viewDistance; ++y) {
            for (int x = -viewDistance; x <= viewDistance; ++x) {
                if (x * x + y * y + z * z >
                        viewDistance * viewDistance ||
                    ChunkCoord{x, y, z} == loadGen) {
                    continue;
                }
                Chunk& chunk = manager.getOrCreateChunk({x, y, z});
                chunk.setWorldGenVersion(generator->config().world.version);
                chunk.setLoadedFromDisk(true);
                chunk.clearPersistDirty();
                chunk.clearDirty();
            }
        }
    }
    addLoadedNeighborShell(
        manager,
        missingMesh,
        std::nullopt,
        generator->config().world.version);
    addLoadedNeighborShell(
        manager,
        dirtyMesh,
        std::nullopt,
        generator->config().world.version);

    auto installDirtyMesh = [&](ChunkCoord coord) {
        Chunk& chunk = *manager.getChunk(coord);
        chunk.setBlock(0, 0, 0, BlockState{solid}, registry);
        chunk.clearPersistDirty();
        ChunkMesh installed;
        installed.vertices.resize(3);
        installed.indices = {0, 1, 2};
        installed.layers[static_cast<size_t>(RenderLayer::Opaque)].indexCount =
            3;
        meshStore.set(coord, std::move(installed));
        chunk.clearDirty();
        chunk.invalidateMesh();
    };
    installDirtyMesh(blocker);
    installDirtyMesh(dirtyMesh);
    Chunk& missingChunk = *manager.getChunk(missingMesh);
    missingChunk.setBlock(0, 0, 0, BlockState{solid}, registry);
    missingChunk.clearPersistDirty();

    auto gate = std::make_shared<WorkerGate>();
    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
    // Release the gated worker before streamer teardown attempts to join it.
    WorkerGateRelease releaseOnExit(gate);
    StreamingConfig stream;
    stream.viewDistanceChunks = viewDistance;
    stream.unloadDistanceChunks = 3;
    stream.meshQueueLimit = 1;
    stream.workerThreads = 2;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    streamer.markSpawnDiscoveryComplete();
    streamer.prioritizeMesh(blocker);
    Rigel::Voxel::detail::ChunkStreamerTestAccess::setMeshBuildStartCallback(
        streamer, [gate]() { gate->enterAndWait(); });

    size_t loadAttempts = 0;
    streamer.setChunkLoader([&](ChunkLoadRequest request) {
        if (request.coord != loadGen) {
            return ChunkLoadRequestResult::Missing;
        }
        ++loadAttempts;
        return ChunkLoadRequestResult::Queued;
    });

    streamer.update(blocker.toWorldCenter());
    CHECK(gate->waitUntilEntered());
    CHECK_EQ(loadAttempts, static_cast<size_t>(1));
    CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::hasPendingLoad(
        streamer, loadGen));
    CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::hasReadyPendingMesh(
        streamer, missingMesh));
    CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::hasReadyPendingMesh(
        streamer, dirtyMesh));

    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 0;
    streamer.setConfig(stream);
    CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::
        isConfigRetiredLoadGen(streamer, loadGen));
    CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::
        isConfigRetiredMissingMesh(streamer, missingMesh));
    CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::
        isConfigRetiredDirtyMesh(streamer, dirtyMesh));
    CHECK(!Rigel::Voxel::detail::ChunkStreamerTestAccess::hasPendingLoad(
        streamer, loadGen));
    CHECK(!Rigel::Voxel::detail::ChunkStreamerTestAccess::hasReadyPendingMesh(
        streamer, missingMesh));
    CHECK(!Rigel::Voxel::detail::ChunkStreamerTestAccess::hasReadyPendingMesh(
        streamer, dirtyMesh));

    enum class SnapshotPhase {
        Retired,
        Canonical,
        Settled
    };
    auto snapshot = [&](SnapshotPhase phase) {
        std::vector<ChunkStreamer::DebugChunkState> states;
        streamer.getDebugStates(states, blocker, viewDistance);
        auto find = [&](ChunkCoord coord)
            -> const ChunkStreamer::DebugChunkState* {
            auto it = std::find_if(
                states.begin(), states.end(), [coord](const auto& state) {
                    return state.coord == coord;
                });
            return it == states.end() ? nullptr : &*it;
        };
        const auto* load = find(loadGen);
        const auto* missing = find(missingMesh);
        const auto* dirty = find(dirtyMesh);
        CHECK(load != nullptr);
        CHECK(missing != nullptr);
        CHECK(dirty != nullptr);
        if (phase == SnapshotPhase::Settled) {
            if (load) {
                CHECK_EQ(load->state, ChunkStreamer::DebugState::VoxelEmpty);
                CHECK_EQ(load->pipelineOwner,
                         ChunkStreamer::DebugPipelineOwner::Complete);
            }
            for (const auto* meshed : {missing, dirty}) {
                if (meshed) {
                    CHECK_EQ(
                        meshed->state,
                        ChunkStreamer::DebugState::AcceptedNonemptyGeometry);
                    CHECK_EQ(meshed->pipelineOwner,
                             ChunkStreamer::DebugPipelineOwner::Complete);
                    CHECK_EQ(meshed->remeshIntent,
                             ChunkStreamer::DebugRemeshIntent::None);
                }
            }
        } else {
            if (load) {
                CHECK_EQ(load->state,
                         ChunkStreamer::DebugState::WaitingForData);
                CHECK_EQ(load->pipelineOwner,
                         ChunkStreamer::DebugPipelineOwner::WaitingForData);
            }
            if (missing) {
                CHECK_EQ(missing->state,
                         ChunkStreamer::DebugState::MeshSchedulerWait);
                CHECK_EQ(missing->pipelineOwner,
                         ChunkStreamer::DebugPipelineOwner::MeshScheduler);
                CHECK_EQ(missing->remeshIntent,
                         ChunkStreamer::DebugRemeshIntent::None);
            }
            if (dirty) {
                CHECK_EQ(dirty->state,
                         ChunkStreamer::DebugState::DirtyRemeshPending);
                CHECK_EQ(
                    dirty->pipelineOwner,
                    phase == SnapshotPhase::Retired
                        ? ChunkStreamer::DebugPipelineOwner::DirtyRemesh
                        : ChunkStreamer::DebugPipelineOwner::MeshScheduler);
                CHECK_EQ(dirty->remeshIntent,
                         ChunkStreamer::DebugRemeshIntent::Pending);
            }
        }
    };
    snapshot(SnapshotPhase::Retired);

    stream.viewDistanceChunks = viewDistance;
    stream.unloadDistanceChunks = 3;
    streamer.setConfig(stream);
    snapshot(SnapshotPhase::Retired);
    CHECK_EQ(streamer.diagnostics().generation.pending,
             static_cast<size_t>(1));
    CHECK_EQ(streamer.diagnostics().mesh.pending,
             static_cast<size_t>(2));

    streamer.update(blocker.toWorldCenter());
    CHECK_EQ(loadAttempts, static_cast<size_t>(2));
    CHECK_EQ(
        Rigel::Voxel::detail::ChunkStreamerTestAccess::configRetiredWorkCount(
            streamer),
        static_cast<size_t>(0));
    CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::hasPendingLoad(
        streamer, loadGen));
    CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::hasReadyPendingMesh(
        streamer, missingMesh));
    CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::hasReadyPendingMesh(
        streamer, dirtyMesh));
    snapshot(SnapshotPhase::Canonical);

    Chunk& loadedEmpty = manager.getOrCreateChunk(loadGen);
    loadedEmpty.setWorldGenVersion(generator->config().world.version);
    loadedEmpty.setLoadedFromDisk(true);
    loadedEmpty.clearPersistDirty();
    loadedEmpty.clearDirty();
    gate->release();

    bool quiescent = false;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        streamer.update(blocker.toWorldCenter());
        streamer.processCompletions();
        if (streamer.diagnostics().state ==
            StreamingLifecycleState::Quiescent) {
            quiescent = true;
            break;
        }
        std::this_thread::yield();
    }
    CHECK(quiescent);
    CHECK_EQ(
        Rigel::Voxel::detail::ChunkStreamerTestAccess::configRetiredWorkCount(
            streamer),
        static_cast<size_t>(0));
    CHECK(streamer.diagnostics().generation.empty());
    CHECK(streamer.diagnostics().chunkLoad.empty());
    CHECK(streamer.diagnostics().mesh.empty());
    CHECK_EQ(streamer.workMetrics().lastUpdateSchedulerCoordinatesInspected,
             static_cast<uint64_t>(0));
    snapshot(SnapshotPhase::Settled);
}

TEST_CASE(ChunkStreamer_ConfigRetiredRetentionMeshHandsOffOnDeferredEviction) {
    for (const int workerThreads : {0, 1}) {
        ChunkManager manager;
        BlockRegistry registry;
        WorldMeshStore meshStore;
        auto generator = makeGenerator(registry);
        const BlockID solid = registerTestBlock(
            registry,
            "rigel:config_retired_retention_" +
                std::to_string(workerThreads));
        const ChunkCoord cameraCoord{0, 0, 0};
        const ChunkCoord remeshCoord{3, 0, 0};

        Chunk& camera = manager.getOrCreateChunk(cameraCoord);
        camera.setWorldGenVersion(generator->config().world.version);
        camera.setLoadedFromDisk(true);
        camera.clearPersistDirty();
        camera.clearDirty();
        addLoadedNeighborShell(
            manager,
            remeshCoord,
            std::nullopt,
            generator->config().world.version);
        manager.forEachChunk([&](ChunkCoord coord, Chunk& chunk) {
            if (coord != cameraCoord) {
                chunk.markPersistDirty();
            }
        });

        Chunk& chunk = manager.getOrCreateChunk(remeshCoord);
        chunk.setBlock(0, 0, 0, BlockState{solid}, registry);
        chunk.setWorldGenVersion(generator->config().world.version);
        chunk.setLoadedFromDisk(false);
        chunk.clearDirty();
        ChunkMesh installedGeometry;
        installedGeometry.vertices.resize(3);
        installedGeometry.indices = {0, 1, 2};
        meshStore.set(remeshCoord, std::move(installedGeometry));
        const uint64_t installedStoreVersion = meshStore.version();

        size_t remeshPersistenceAttempts = 0;
        ChunkStreamer streamer(
            manager, meshStore, registry, nullptr, generator);
        StreamingConfig stream;
        stream.viewDistanceChunks = 0;
        stream.unloadDistanceChunks = 4;
        stream.meshQueueLimit = 1;
        stream.workerThreads = workerThreads;
        stream.maxResidentChunks = 0;
        streamer.setConfig(stream);
        streamer.setChunkEvictionCallback([&](ChunkCoord coord) {
            if (coord == remeshCoord) {
                ++remeshPersistenceAttempts;
            }
            return false;
        });
        streamer.markSpawnDiscoveryComplete();

        for (size_t cycle = 0; cycle < 2; ++cycle) {
            const uint64_t oldCompletionTarget = cycle * 2 + 1;
            const uint64_t replacementCompletionTarget = cycle * 2 + 2;
            const bool completionBeforeRestore = cycle != 0;

            chunk.invalidateMesh();
            const uint32_t currentRevision = chunk.meshRevision();
            streamer.update(cameraCoord.toWorldCenter());
            CHECK_EQ(streamer.workMetrics().meshJobsStarted,
                     oldCompletionTarget);
            CHECK_EQ(streamer.diagnostics().mesh.inFlight,
                     static_cast<size_t>(1));
            CHECK(!chunk.isDirty());

            stream.unloadDistanceChunks = 0;
            streamer.setConfig(stream);
            CHECK_EQ(
                Rigel::Voxel::detail::ChunkStreamerTestAccess::
                    configRetiredWorkCount(streamer),
                static_cast<size_t>(1));
            CHECK_EQ(streamer.diagnostics().mesh.pending,
                     static_cast<size_t>(0));
            CHECK_EQ(streamer.diagnostics().mesh.inFlight,
                     static_cast<size_t>(1));

            streamer.update(cameraCoord.toWorldCenter());
            CHECK_EQ(
                Rigel::Voxel::detail::ChunkStreamerTestAccess::
                    configRetiredWorkCount(streamer),
                static_cast<size_t>(0));
            CHECK(manager.hasChunk(remeshCoord));
            CHECK(chunk.isPersistDirty());
            CHECK(chunk.isDirty());
            CHECK_EQ(remeshPersistenceAttempts, cycle + 1);
            CHECK(!streamer.diagnostics().eviction.empty());
            CHECK_NE(streamer.diagnostics().state,
                     StreamingLifecycleState::Quiescent);

            if (completionBeforeRestore) {
                CHECK(waitForMeshCompletions(
                    streamer, oldCompletionTarget));
                CHECK_EQ(streamer.workMetrics().meshJobsRejectedStale,
                         cycle + 1);
                CHECK_EQ(meshStore.version(),
                         installedStoreVersion + cycle);
            }

            stream.unloadDistanceChunks = 4;
            streamer.setConfig(stream);
            streamer.update(cameraCoord.toWorldCenter());
            CHECK_EQ(
                Rigel::Voxel::detail::ChunkStreamerTestAccess::
                    configRetiredWorkCount(streamer),
                static_cast<size_t>(0));
            CHECK(streamer.diagnostics().eviction.empty());
            CHECK_NE(streamer.diagnostics().state,
                     StreamingLifecycleState::Quiescent);

            if (!completionBeforeRestore) {
                CHECK_EQ(streamer.workMetrics().meshJobsStarted,
                         oldCompletionTarget);
                CHECK_EQ(streamer.diagnostics().mesh.pending,
                         static_cast<size_t>(1));
                CHECK(waitForMeshCompletions(
                    streamer, oldCompletionTarget));
                CHECK_EQ(streamer.workMetrics().meshJobsRejectedStale,
                         cycle + 1);
                CHECK_EQ(meshStore.version(),
                         installedStoreVersion + cycle);
                streamer.update(cameraCoord.toWorldCenter());
            }

            CHECK_EQ(streamer.workMetrics().meshJobsStarted,
                     replacementCompletionTarget);
            CHECK_EQ(
                Rigel::Voxel::detail::ChunkStreamerTestAccess::
                    inFlightMeshObservedRevision(streamer, remeshCoord),
                std::optional<uint32_t>{currentRevision});
            CHECK(waitForMeshCompletions(
                streamer, replacementCompletionTarget));
            CHECK_EQ(streamer.workMetrics().meshJobsCompleted,
                     replacementCompletionTarget);
            CHECK_EQ(streamer.workMetrics().meshJobsAccepted, cycle + 1);
            CHECK_EQ(streamer.workMetrics().meshJobsRejectedStale,
                     cycle + 1);
            CHECK_EQ(chunk.meshRevision(), currentRevision);
            CHECK(!chunk.isDirty());
            CHECK(meshStore.contains(remeshCoord));
            CHECK_EQ(meshStore.version(),
                     installedStoreVersion + cycle + 1);
            CHECK(streamer.diagnostics().mesh.empty());
        }

        const uint64_t started = streamer.workMetrics().meshJobsStarted;
        for (uint32_t stable = 0;
             stable <= StreamingDiagnosticSnapshot::QuiescenceUpdateWindow;
             ++stable) {
            streamer.update(cameraCoord.toWorldCenter());
            streamer.processCompletions();
            CHECK_EQ(
                Rigel::Voxel::detail::ChunkStreamerTestAccess::
                    configRetiredWorkCount(streamer),
                static_cast<size_t>(0));
        }
        CHECK_EQ(streamer.workMetrics().meshJobsStarted, started);
        CHECK_EQ(
            streamer.workMetrics().lastUpdateSchedulerCoordinatesInspected,
            static_cast<uint64_t>(0));
        CHECK(streamer.diagnostics().generation.empty());
        CHECK(streamer.diagnostics().chunkLoad.empty());
        CHECK(streamer.diagnostics().mesh.empty());
        CHECK(streamer.diagnostics().eviction.empty());
        CHECK_EQ(streamer.diagnostics().state,
                 StreamingLifecycleState::Quiescent);
    }
}

TEST_CASE(ChunkStreamer_ConfigRetiredMeshTransfersToCanonicalWakeOwner) {
    enum class WakeKind {
        GeneratorReplacement,
        DirtyNotification
    };

    for (const WakeKind wake :
         {WakeKind::GeneratorReplacement, WakeKind::DirtyNotification}) {
        ChunkManager manager;
        BlockRegistry registry;
        WorldMeshStore meshStore;
        auto generator = makeGenerator(registry);
        auto replacementGenerator = std::make_shared<WorldGenerator>(
            registry, generator->config());
        const BlockID solid = registerTestBlock(
            registry,
            wake == WakeKind::GeneratorReplacement
                ? "rigel:config_retired_generator_transfer"
                : "rigel:config_retired_dirty_transfer");
        const ChunkCoord cameraCoord{0, 0, 0};
        const ChunkCoord remeshCoord{1, 0, 0};

        const std::array<ChunkCoord, 7> desired{
            cameraCoord,
            remeshCoord,
            ChunkCoord{-1, 0, 0},
            ChunkCoord{0, 1, 0},
            ChunkCoord{0, -1, 0},
            ChunkCoord{0, 0, 1},
            ChunkCoord{0, 0, -1}
        };
        for (const ChunkCoord& coord : desired) {
            Chunk& resident = manager.getOrCreateChunk(coord);
            resident.setWorldGenVersion(generator->config().world.version);
            resident.setLoadedFromDisk(true);
            resident.clearPersistDirty();
            resident.clearDirty();
        }
        Chunk& chunk = *manager.getChunk(remeshCoord);
        chunk.setBlock(0, 0, 0, BlockState{solid}, registry);
        chunk.clearPersistDirty();
        chunk.clearDirty();
        meshStore.set(remeshCoord, {});
        chunk.invalidateMesh();

        auto gate = std::make_shared<WorkerGate>();
        std::atomic<size_t> physicalBuilds{0};
        ChunkStreamer streamer(
            manager, meshStore, registry, nullptr, generator);
        WorkerGateRelease releaseOnExit(gate);
        StreamingConfig stream;
        stream.viewDistanceChunks = 1;
        stream.unloadDistanceChunks = 1;
        stream.meshQueueLimit = 1;
        stream.applyBudgetPerFrame = 0;
        stream.workerThreads = 2;
        stream.maxResidentChunks = 0;
        streamer.setConfig(stream);
        streamer.markSpawnDiscoveryComplete();
        streamer.prioritizeMesh(remeshCoord);
        Rigel::Voxel::detail::ChunkStreamerTestAccess::
            setMeshBuildStartCallback(
                streamer,
                [gate, &physicalBuilds]() {
                    if (physicalBuilds.fetch_add(
                            1, std::memory_order_relaxed) == 0) {
                        gate->enterAndWait();
                    }
                });

        streamer.update(cameraCoord.toWorldCenter());
        CHECK(gate->waitUntilEntered());
        CHECK_EQ(streamer.workMetrics().meshJobsStarted,
                 static_cast<uint64_t>(1));

        stream.viewDistanceChunks = 0;
        stream.unloadDistanceChunks = 0;
        streamer.setConfig(stream);
        CHECK_EQ(
            Rigel::Voxel::detail::ChunkStreamerTestAccess::
                configRetiredWorkCount(streamer),
            static_cast<size_t>(1));
        CHECK_EQ(streamer.diagnostics().mesh.pending,
                 static_cast<size_t>(0));
        CHECK_EQ(streamer.diagnostics().mesh.inFlight,
                 static_cast<size_t>(1));

        stream.viewDistanceChunks = 1;
        stream.unloadDistanceChunks = 1;
        streamer.setConfig(stream);
        CHECK_EQ(streamer.diagnostics().mesh.pending,
                 static_cast<size_t>(1));
        if (wake == WakeKind::GeneratorReplacement) {
            streamer.setGenerator(replacementGenerator);
        } else {
            chunk.invalidateMesh();
            streamer.prioritizeMesh(remeshCoord);
        }

        CHECK_EQ(
            Rigel::Voxel::detail::ChunkStreamerTestAccess::
                configRetiredWorkCount(streamer),
            static_cast<size_t>(0));
        CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::
            hasReplacementPendingMesh(streamer, remeshCoord));
        CHECK_EQ(
            Rigel::Voxel::detail::ChunkStreamerTestAccess::
                replacementPendingMeshCount(streamer),
            static_cast<size_t>(1));
        CHECK(!Rigel::Voxel::detail::ChunkStreamerTestAccess::
            hasReadyPendingMesh(streamer, remeshCoord));
        CHECK(!Rigel::Voxel::detail::ChunkStreamerTestAccess::
            hasDependencyPendingMesh(streamer, remeshCoord));
        CHECK_EQ(streamer.diagnostics().mesh.pending,
                 static_cast<size_t>(1));
        CHECK_EQ(streamer.diagnostics().mesh.inFlight,
                 static_cast<size_t>(1));

        gate->release();
        CHECK(waitForMeshCompletions(streamer, 1));
        CHECK_EQ(streamer.workMetrics().meshJobsAccepted,
                 static_cast<uint64_t>(0));
        CHECK_EQ(streamer.workMetrics().meshJobsRejectedStale,
                 static_cast<uint64_t>(1));
        CHECK_EQ(
            Rigel::Voxel::detail::ChunkStreamerTestAccess::
                replacementPendingMeshCount(streamer),
            static_cast<size_t>(0));
        CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::
            hasReadyPendingMesh(streamer, remeshCoord));
        CHECK_EQ(streamer.diagnostics().mesh.pending,
                 static_cast<size_t>(1));
        CHECK_EQ(streamer.diagnostics().mesh.inFlight,
                 static_cast<size_t>(0));

        streamer.update(cameraCoord.toWorldCenter());
        CHECK_EQ(streamer.workMetrics().meshJobsStarted,
                 static_cast<uint64_t>(2));
        CHECK(waitForMeshCompletions(streamer, 2));
        CHECK_EQ(physicalBuilds.load(std::memory_order_relaxed),
                 static_cast<size_t>(2));
        CHECK_EQ(streamer.workMetrics().meshJobsAccepted,
                 static_cast<uint64_t>(1));
        CHECK_EQ(streamer.workMetrics().meshJobsRejectedStale,
                 static_cast<uint64_t>(1));
        CHECK(streamer.diagnostics().mesh.empty());

        for (uint32_t stable = 0;
             stable <= StreamingDiagnosticSnapshot::QuiescenceUpdateWindow;
             ++stable) {
            streamer.update(cameraCoord.toWorldCenter());
            streamer.processCompletions();
        }
        CHECK_EQ(
            streamer.workMetrics().lastUpdateSchedulerCoordinatesInspected,
            static_cast<uint64_t>(0));
        CHECK_EQ(streamer.diagnostics().state,
                 StreamingLifecycleState::Quiescent);
    }
}

TEST_CASE(ChunkStreamer_ConfigRetiredDirtyWakeTransfersToDependencyOwner) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    const BlockID solid = registerTestBlock(
        registry, "rigel:config_retired_dependency_transfer");
    const ChunkCoord missingDependency{0, 0, 0};
    const ChunkCoord remeshCoord{1, 0, 0};

    addLoadedNeighborShell(
        manager,
        remeshCoord,
        missingDependency,
        generator->config().world.version);
    for (const ChunkCoord& coord : {
             remeshCoord,
             ChunkCoord{-1, 0, 0},
             ChunkCoord{0, 1, 0},
             ChunkCoord{0, -1, 0},
             ChunkCoord{0, 0, 1},
             ChunkCoord{0, 0, -1}}) {
        Chunk& resident = manager.getOrCreateChunk(coord);
        resident.setWorldGenVersion(generator->config().world.version);
        resident.setLoadedFromDisk(true);
        resident.clearPersistDirty();
        resident.clearDirty();
    }
    Chunk& chunk = *manager.getChunk(remeshCoord);
    chunk.setBlock(0, 0, 0, BlockState{solid}, registry);
    chunk.clearPersistDirty();
    chunk.clearDirty();
    meshStore.set(remeshCoord, {});
    chunk.invalidateMesh();

    bool dependencyPending = true;
    size_t physicalBuilds = 0;
    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 1;
    stream.unloadDistanceChunks = 2;
    stream.meshQueueLimit = 1;
    stream.genQueueLimit = 1;
    stream.updateBudgetPerFrame = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 0;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    streamer.setChunkLoader(
        [&](ChunkLoadRequest request) {
            return request.coord == missingDependency
                ? ChunkLoadRequestResult::Queued
                : ChunkLoadRequestResult::Missing;
        });
    streamer.setChunkPendingCallback(
        [&](ChunkCoord coord) {
            return coord == missingDependency && dependencyPending;
        });
    Rigel::Voxel::detail::ChunkStreamerTestAccess::setMeshBuildStartCallback(
        streamer, [&physicalBuilds]() { ++physicalBuilds; });
    streamer.markSpawnDiscoveryComplete();

    streamer.update(missingDependency.toWorldCenter());
    CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::
        hasDependencyPendingMesh(streamer, remeshCoord));
    CHECK(!Rigel::Voxel::detail::ChunkStreamerTestAccess::
        hasConfigRetiredWork(streamer, remeshCoord));
    CHECK_EQ(streamer.diagnostics().mesh.pending, static_cast<size_t>(1));
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(0));

    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 0;
    streamer.setConfig(stream);
    CHECK(!Rigel::Voxel::detail::ChunkStreamerTestAccess::
        hasDependencyPendingMesh(streamer, remeshCoord));
    CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::
        hasConfigRetiredWork(streamer, remeshCoord));
    CHECK_EQ(
        Rigel::Voxel::detail::ChunkStreamerTestAccess::
            configRetiredWorkCount(streamer),
        static_cast<size_t>(1));
    CHECK_EQ(streamer.diagnostics().mesh.pending, static_cast<size_t>(0));

    stream.viewDistanceChunks = 1;
    stream.unloadDistanceChunks = 2;
    streamer.setConfig(stream);
    CHECK_EQ(streamer.diagnostics().mesh.pending, static_cast<size_t>(1));

    chunk.invalidateMesh();
    streamer.prioritizeMesh(remeshCoord);

    CHECK(!Rigel::Voxel::detail::ChunkStreamerTestAccess::
        hasConfigRetiredWork(streamer, remeshCoord));
    CHECK_EQ(
        Rigel::Voxel::detail::ChunkStreamerTestAccess::
            configRetiredWorkCount(streamer),
        static_cast<size_t>(0));
    CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::
        hasDependencyPendingMesh(streamer, remeshCoord));
    CHECK(!Rigel::Voxel::detail::ChunkStreamerTestAccess::
        hasReadyPendingMesh(streamer, remeshCoord));
    CHECK_EQ(streamer.diagnostics().mesh.pending, static_cast<size_t>(1));
    CHECK_EQ(streamer.diagnostics().mesh.inFlight, static_cast<size_t>(0));
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(0));

    dependencyPending = false;
    Chunk& dependency = manager.getOrCreateChunk(missingDependency);
    dependency.setWorldGenVersion(generator->config().world.version);
    dependency.setLoadedFromDisk(true);
    dependency.clearPersistDirty();
    dependency.clearDirty();

    streamer.update(missingDependency.toWorldCenter());
    CHECK(!Rigel::Voxel::detail::ChunkStreamerTestAccess::
        hasDependencyPendingMesh(streamer, remeshCoord));
    CHECK(!Rigel::Voxel::detail::ChunkStreamerTestAccess::
        hasReadyPendingMesh(streamer, remeshCoord));
    CHECK_EQ(physicalBuilds, static_cast<size_t>(1));
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(1));
    CHECK_EQ(
        Rigel::Voxel::detail::ChunkStreamerTestAccess::meshCompletionCount(
            streamer),
        static_cast<size_t>(1));
    CHECK_EQ(streamer.diagnostics().mesh.pending, static_cast<size_t>(0));
    CHECK_EQ(streamer.diagnostics().mesh.inFlight, static_cast<size_t>(1));

    streamer.processCompletions();
    CHECK_EQ(streamer.workMetrics().meshJobsCompleted, static_cast<uint64_t>(1));
    CHECK_EQ(streamer.workMetrics().meshJobsAccepted, static_cast<uint64_t>(1));
    CHECK_EQ(streamer.workMetrics().meshJobsRejectedStale,
             static_cast<uint64_t>(0));
    CHECK_EQ(physicalBuilds, static_cast<size_t>(1));
    CHECK(!chunk.isDirty());
    CHECK(meshStore.contains(remeshCoord));
    CHECK(streamer.diagnostics().mesh.empty());

    for (uint32_t stable = 0;
         stable <= StreamingDiagnosticSnapshot::QuiescenceUpdateWindow;
         ++stable) {
        streamer.update(missingDependency.toWorldCenter());
        streamer.processCompletions();
    }
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(1));
    CHECK_EQ(physicalBuilds, static_cast<size_t>(1));
    CHECK_EQ(streamer.workMetrics().lastUpdateSchedulerCoordinatesInspected,
             static_cast<uint64_t>(0));
    CHECK_EQ(streamer.diagnostics().state,
             StreamingLifecycleState::Quiescent);
}

TEST_CASE(ChunkStreamer_ConfigRetiredLoadTransfersToReplacementLoader) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    const ChunkCoord cameraCoord{0, 0, 0};
    const ChunkCoord loadCoord{1, 0, 0};

    const std::array<ChunkCoord, 6> residentCoords{
        cameraCoord,
        ChunkCoord{-1, 0, 0},
        ChunkCoord{0, 1, 0},
        ChunkCoord{0, -1, 0},
        ChunkCoord{0, 0, 1},
        ChunkCoord{0, 0, -1}
    };
    for (const ChunkCoord& coord : residentCoords) {
        Chunk& chunk = manager.getOrCreateChunk(coord);
        chunk.setWorldGenVersion(generator->config().world.version);
        chunk.setLoadedFromDisk(true);
        chunk.clearPersistDirty();
        chunk.clearDirty();
    }

    size_t originalLoads = 0;
    size_t replacementLoads = 0;
    size_t cancellations = 0;
    ChunkStreamer streamer(
        manager, meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 1;
    stream.unloadDistanceChunks = 1;
    stream.updateBudgetPerFrame = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 0;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    streamer.setChunkLoader([&](ChunkLoadRequest request) {
        if (request.coord == loadCoord) {
            ++originalLoads;
            return ChunkLoadRequestResult::Queued;
        }
        return ChunkLoadRequestResult::Missing;
    });
    streamer.setChunkLoadCancel([&](ChunkCoord coord) {
        if (coord == loadCoord) {
            ++cancellations;
        }
    });
    streamer.markSpawnDiscoveryComplete();

    streamer.update(cameraCoord.toWorldCenter());
    CHECK_EQ(originalLoads, static_cast<size_t>(1));
    CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::hasPendingLoad(
        streamer, loadCoord));

    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 0;
    streamer.setConfig(stream);
    CHECK_EQ(cancellations, static_cast<size_t>(1));
    CHECK_EQ(
        Rigel::Voxel::detail::ChunkStreamerTestAccess::
            configRetiredWorkCount(streamer),
        static_cast<size_t>(1));
    CHECK(!Rigel::Voxel::detail::ChunkStreamerTestAccess::hasPendingLoad(
        streamer, loadCoord));

    stream.viewDistanceChunks = 1;
    stream.unloadDistanceChunks = 1;
    streamer.setConfig(stream);
    CHECK_EQ(streamer.diagnostics().generation.pending,
             static_cast<size_t>(1));
    streamer.setChunkLoader([&](ChunkLoadRequest request) {
        if (request.coord == loadCoord) {
            ++replacementLoads;
        }
        return ChunkLoadRequestResult::Missing;
    });

    CHECK_EQ(
        Rigel::Voxel::detail::ChunkStreamerTestAccess::
            configRetiredWorkCount(streamer),
        static_cast<size_t>(0));
    CHECK_EQ(streamer.diagnostics().generation.pending,
             static_cast<size_t>(1));
    CHECK_EQ(streamer.diagnostics().generation.inFlight,
             static_cast<size_t>(0));

    streamer.update(cameraCoord.toWorldCenter());
    CHECK_EQ(replacementLoads, static_cast<size_t>(1));
    CHECK_EQ(streamer.workMetrics().generationJobsStarted,
             static_cast<uint64_t>(1));
    CHECK_EQ(streamer.diagnostics().generation.pending,
             static_cast<size_t>(0));
    CHECK_EQ(streamer.diagnostics().generation.inFlight,
             static_cast<size_t>(1));
    streamer.processCompletions();
    CHECK_EQ(streamer.diagnostics().generation.inFlight,
             static_cast<size_t>(0));

    for (int update = 0; update < 8; ++update) {
        streamer.update(cameraCoord.toWorldCenter());
        streamer.processCompletions();
    }
    CHECK_EQ(originalLoads, static_cast<size_t>(1));
    CHECK_EQ(replacementLoads, static_cast<size_t>(1));
    CHECK_EQ(streamer.workMetrics().generationJobsStarted,
             static_cast<uint64_t>(1));
    CHECK_EQ(
        streamer.workMetrics().lastUpdateSchedulerCoordinatesInspected,
        static_cast<uint64_t>(0));
    CHECK(streamer.diagnostics().generation.empty());
    CHECK(streamer.diagnostics().chunkLoad.empty());
}

TEST_CASE(ChunkStreamer_PendingDiagnosticsClassifyResidentMutationOnce) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    const BlockID solid = registerTestBlock(
        registry, "rigel:pending_diagnostic_classification");
    const ChunkCoord coord{0, 0, 0};

    Chunk& initial = manager.getOrCreateChunk(coord);
    initial.setWorldGenVersion(generator->config().world.version);
    initial.setLoadedFromDisk(true);
    initial.clearPersistDirty();
    initial.clearDirty();

    ChunkStreamer streamer(
        manager, meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 0;
    stream.updateBudgetPerFrame = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 0;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    streamer.markSpawnDiscoveryComplete();
    streamer.update(coord.toWorldCenter());
    streamer.processCompletions();
    Rigel::Voxel::detail::ChunkStreamerTestAccess::queueLoadGen(
        streamer, coord);

    CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::evictChunk(
        streamer, coord));
    Rigel::Voxel::detail::ChunkStreamerTestAccess::refreshDiagnostics(
        streamer);
    CHECK_EQ(streamer.diagnostics().generation.pending,
             static_cast<size_t>(1));
    CHECK_EQ(streamer.diagnostics().mesh.pending,
             static_cast<size_t>(0));

    Chunk& replacement = manager.getOrCreateChunk(coord);
    replacement.setWorldGenVersion(generator->config().world.version + 1);
    replacement.setLoadedFromDisk(true);
    replacement.clearPersistDirty();
    replacement.clearDirty();
    Rigel::Voxel::detail::ChunkStreamerTestAccess::refreshDiagnostics(
        streamer);
    CHECK_EQ(streamer.diagnostics().generation.pending,
             static_cast<size_t>(1));
    CHECK_EQ(streamer.diagnostics().mesh.pending,
             static_cast<size_t>(0));

    replacement.setWorldGenVersion(generator->config().world.version);
    Rigel::Voxel::detail::ChunkStreamerTestAccess::refreshDiagnostics(
        streamer);
    CHECK_EQ(streamer.diagnostics().generation.pending,
             static_cast<size_t>(0));
    CHECK_EQ(streamer.diagnostics().mesh.pending,
             static_cast<size_t>(0));

    replacement.setBlock(0, 0, 0, BlockState{solid}, registry);
    replacement.clearPersistDirty();
    replacement.clearDirty();
    Rigel::Voxel::detail::ChunkStreamerTestAccess::refreshDiagnostics(
        streamer);
    CHECK_EQ(streamer.diagnostics().generation.pending,
             static_cast<size_t>(0));
    CHECK_EQ(streamer.diagnostics().mesh.pending,
             static_cast<size_t>(1));

    meshStore.set(coord, {});
    Rigel::Voxel::detail::ChunkStreamerTestAccess::refreshDiagnostics(
        streamer);
    CHECK_EQ(streamer.diagnostics().generation.pending,
             static_cast<size_t>(0));
    CHECK_EQ(streamer.diagnostics().mesh.pending,
             static_cast<size_t>(0));

    replacement.invalidateMesh();
    Rigel::Voxel::detail::ChunkStreamerTestAccess::refreshDiagnostics(
        streamer);
    CHECK_EQ(streamer.diagnostics().generation.pending,
             static_cast<size_t>(0));
    CHECK_EQ(streamer.diagnostics().mesh.pending,
             static_cast<size_t>(1));

    Rigel::Voxel::detail::ChunkStreamerTestAccess::
        rememberConfigRetiredLoadGen(streamer, coord);
    Rigel::Voxel::detail::ChunkStreamerTestAccess::refreshDiagnostics(
        streamer);
    CHECK_EQ(
        Rigel::Voxel::detail::ChunkStreamerTestAccess::
            configRetiredWorkCount(streamer),
        static_cast<size_t>(1));
    CHECK_EQ(streamer.diagnostics().mesh.pending,
             static_cast<size_t>(1));

    replacement.setBlock(0, 0, 0, BlockState{}, registry);
    Rigel::Voxel::detail::ChunkStreamerTestAccess::refreshDiagnostics(
        streamer);
    CHECK_EQ(streamer.diagnostics().generation.pending,
             static_cast<size_t>(0));
    CHECK_EQ(streamer.diagnostics().mesh.pending,
             static_cast<size_t>(1));

    meshStore.remove(coord);
    Rigel::Voxel::detail::ChunkStreamerTestAccess::refreshDiagnostics(
        streamer);
    CHECK_EQ(streamer.diagnostics().generation.pending,
             static_cast<size_t>(0));
    CHECK_EQ(streamer.diagnostics().mesh.pending,
             static_cast<size_t>(0));
    CHECK_EQ(streamer.diagnostics().generation.terminalErrors,
             static_cast<size_t>(0));
    CHECK_EQ(streamer.diagnostics().mesh.terminalErrors,
             static_cast<size_t>(0));
}

TEST_CASE(ChunkStreamer_ConfigRetiredVoxelEmptyMeshCleansUpAfterRestore) {
    for (const int workerThreads : {0, 1}) {
        ChunkManager manager;
        BlockRegistry registry;
        WorldMeshStore meshStore;
        auto generator = makeGenerator(registry);
        const BlockID solid = registerTestBlock(
            registry,
            "rigel:config_retired_empty_cleanup_" +
                std::to_string(workerThreads));
        const ChunkCoord cameraCoord{0, 0, 0};
        const ChunkCoord cleanupCoord{3, 0, 0};

        Chunk& camera = manager.getOrCreateChunk(cameraCoord);
        camera.setWorldGenVersion(generator->config().world.version);
        camera.setLoadedFromDisk(true);
        camera.clearPersistDirty();
        camera.clearDirty();
        addLoadedNeighborShell(
            manager,
            cleanupCoord,
            std::nullopt,
            generator->config().world.version);

        Chunk& chunk = manager.getOrCreateChunk(cleanupCoord);
        chunk.setBlock(0, 0, 0, BlockState{solid}, registry);
        chunk.setWorldGenVersion(generator->config().world.version);
        chunk.setLoadedFromDisk(true);
        chunk.clearPersistDirty();
        chunk.clearDirty();
        ChunkMesh installedGeometry;
        installedGeometry.vertices.resize(3);
        installedGeometry.indices = {0, 1, 2};
        meshStore.set(cleanupCoord, std::move(installedGeometry));
        const uint64_t installedStoreVersion = meshStore.version();

        size_t physicalBuilds = 0;
        size_t cleanupPersistenceAttempts = 0;
        ChunkStreamer streamer(
            manager, meshStore, registry, nullptr, generator);
        StreamingConfig stream;
        stream.viewDistanceChunks = 0;
        stream.unloadDistanceChunks = 4;
        stream.meshQueueLimit = 1;
        stream.applyBudgetPerFrame = 0;
        stream.workerThreads = workerThreads;
        stream.maxResidentChunks = 0;
        streamer.setConfig(stream);
        streamer.setChunkEvictionCallback([&](ChunkCoord coord) {
            if (coord == cleanupCoord) {
                ++cleanupPersistenceAttempts;
            }
            return false;
        });
        streamer.markSpawnDiscoveryComplete();
        Rigel::Voxel::detail::ChunkStreamerTestAccess::
            setMeshBuildStartCallback(streamer, [&]() {
                ++physicalBuilds;
            });

        chunk.invalidateMesh();
        streamer.update(cameraCoord.toWorldCenter());
        CHECK_EQ(physicalBuilds, static_cast<size_t>(1));
        CHECK_EQ(streamer.workMetrics().meshJobsStarted,
                 static_cast<uint64_t>(1));
        CHECK_EQ(streamer.diagnostics().mesh.inFlight,
                 static_cast<size_t>(1));
        CHECK_EQ(
            Rigel::Voxel::detail::ChunkStreamerTestAccess::
                meshCompletionCount(streamer),
            static_cast<size_t>(1));
        CHECK(!chunk.isDirty());

        chunk.setBlock(0, 0, 0, BlockState{}, registry);
        CHECK(chunk.isEmpty());
        CHECK(chunk.isDirty());
        CHECK(meshStore.contains(cleanupCoord));

        stream.unloadDistanceChunks = 0;
        streamer.setConfig(stream);
        CHECK_EQ(
            Rigel::Voxel::detail::ChunkStreamerTestAccess::
                configRetiredWorkCount(streamer),
            static_cast<size_t>(1));
        CHECK_EQ(streamer.diagnostics().mesh.pending,
                 static_cast<size_t>(1));
        CHECK_EQ(streamer.diagnostics().mesh.inFlight,
                 static_cast<size_t>(1));
        CHECK_EQ(streamer.diagnostics().eviction.pending,
                 static_cast<size_t>(0));

        streamer.processCompletions();
        CHECK_EQ(streamer.workMetrics().meshJobsCompleted,
                 static_cast<uint64_t>(1));
        CHECK_EQ(streamer.workMetrics().meshJobsAccepted,
                 static_cast<uint64_t>(0));
        CHECK_EQ(streamer.workMetrics().meshJobsRejectedStale,
                 static_cast<uint64_t>(1));
        CHECK_EQ(
            Rigel::Voxel::detail::ChunkStreamerTestAccess::
                configRetiredWorkCount(streamer),
            static_cast<size_t>(1));
        CHECK_EQ(streamer.diagnostics().mesh.pending,
                 static_cast<size_t>(1));
        CHECK_EQ(streamer.diagnostics().mesh.inFlight,
                 static_cast<size_t>(0));
        CHECK_EQ(streamer.diagnostics().eviction.pending,
                 static_cast<size_t>(0));
        CHECK(!streamer.diagnostics().mesh.empty());
        CHECK_NE(streamer.diagnostics().state,
                 StreamingLifecycleState::Quiescent);
        CHECK_EQ(physicalBuilds, static_cast<size_t>(1));
        CHECK_EQ(meshStore.version(), installedStoreVersion);
        CHECK(meshStore.contains(cleanupCoord));

        streamer.update(cameraCoord.toWorldCenter());
        CHECK_EQ(
            Rigel::Voxel::detail::ChunkStreamerTestAccess::
                configRetiredWorkCount(streamer),
            static_cast<size_t>(0));
        CHECK_EQ(cleanupPersistenceAttempts, static_cast<size_t>(1));
        CHECK_EQ(streamer.diagnostics().mesh.pending,
                 static_cast<size_t>(0));
        CHECK_EQ(streamer.diagnostics().eviction.pending,
                 static_cast<size_t>(1));
        CHECK(meshStore.contains(cleanupCoord));
        CHECK_EQ(meshStore.version(), installedStoreVersion);
        CHECK(meshStore.contains(cleanupCoord));

        stream.unloadDistanceChunks = 4;
        streamer.setConfig(stream);
        CHECK_EQ(physicalBuilds, static_cast<size_t>(1));
        CHECK_EQ(streamer.workMetrics().meshJobsStarted,
                 static_cast<uint64_t>(1));
        CHECK_EQ(
            Rigel::Voxel::detail::ChunkStreamerTestAccess::
                configRetiredWorkCount(streamer),
            static_cast<size_t>(0));
        CHECK(streamer.diagnostics().mesh.empty());
        CHECK(streamer.diagnostics().eviction.empty());
        CHECK(!meshStore.contains(cleanupCoord));
        CHECK_EQ(meshStore.version(), installedStoreVersion + 1);
        CHECK(!chunk.isDirty());

        for (uint32_t stable = 0;
             stable <= StreamingDiagnosticSnapshot::QuiescenceUpdateWindow;
             ++stable) {
            streamer.update(cameraCoord.toWorldCenter());
            streamer.processCompletions();
        }
        CHECK_EQ(physicalBuilds, static_cast<size_t>(1));
        CHECK_EQ(streamer.workMetrics().meshJobsStarted,
                 static_cast<uint64_t>(1));
        CHECK_EQ(
            streamer.workMetrics().lastUpdateSchedulerCoordinatesInspected,
            static_cast<uint64_t>(0));
        CHECK_EQ(streamer.diagnostics().state,
                 StreamingLifecycleState::Quiescent);
    }
}

TEST_CASE(ChunkStreamer_ConfigRetiredWorkBoundedForInlineMeshExecution) {
    for (const int workerThreads : {0, 1}) {
        ChunkManager manager;
        BlockRegistry registry;
        WorldMeshStore meshStore;
        auto generator = makeGenerator(registry);
        const BlockID solid = registerTestBlock(
            registry,
            "rigel:config_retired_inline_bound_" +
                std::to_string(workerThreads));
        const std::array<ChunkCoord, 4> cameraCoords{
            ChunkCoord{0, 0, 0},
            ChunkCoord{8, 0, 0},
            ChunkCoord{16, 0, 0},
            ChunkCoord{24, 0, 0}
        };

        size_t physicalBuilds = 0;
        ChunkStreamer streamer(
            manager, meshStore, registry, nullptr, generator);
        StreamingConfig stream;
        stream.viewDistanceChunks = 1;
        stream.unloadDistanceChunks = 1;
        stream.meshQueueLimit = 0;
        stream.updateBudgetPerFrame = 0;
        stream.applyBudgetPerFrame = 0;
        stream.workerThreads = workerThreads;
        stream.maxResidentChunks = 0;
        streamer.setConfig(stream);
        streamer.markSpawnDiscoveryComplete();
        Rigel::Voxel::detail::ChunkStreamerTestAccess::
            setMeshBuildStartCallback(streamer, [&]() {
                ++physicalBuilds;
            });

        auto preloadView = [&](ChunkCoord center) {
            for (int dz = -1; dz <= 1; ++dz) {
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        if (dx * dx + dy * dy + dz * dz > 1) {
                            continue;
                        }
                        Chunk& chunk = manager.getOrCreateChunk(
                            center.offset(dx, dy, dz));
                        chunk.setBlock(
                            1, 1, 1, BlockState{solid}, registry);
                        chunk.setWorldGenVersion(
                            generator->config().world.version);
                        chunk.setLoadedFromDisk(true);
                        chunk.clearPersistDirty();
                        chunk.clearDirty();
                    }
                }
            }
        };

        for (size_t view = 0; view < cameraCoords.size(); ++view) {
            preloadView(cameraCoords[view]);
            streamer.update(cameraCoords[view].toWorldCenter());

            CHECK_EQ(
                Rigel::Voxel::detail::ChunkStreamerTestAccess::
                    meshDispatchLimit(streamer),
                static_cast<size_t>(1));
            CHECK_EQ(streamer.diagnostics().meshSubmissionLimit,
                     static_cast<size_t>(1));
            CHECK_EQ(streamer.workMetrics().meshJobsStarted,
                     static_cast<uint64_t>(1));
            CHECK_EQ(physicalBuilds, static_cast<size_t>(1));
            CHECK_EQ(streamer.diagnostics().mesh.inFlight,
                     static_cast<size_t>(1));
            CHECK_EQ(
                Rigel::Voxel::detail::ChunkStreamerTestAccess::
                    meshCompletionCount(streamer),
                static_cast<size_t>(1));
            CHECK_EQ(
                Rigel::Voxel::detail::ChunkStreamerTestAccess::
                    configRetiredWorkCount(streamer),
                static_cast<size_t>(0));
            CHECK_EQ(streamer.diagnostics().mesh.pending,
                     view == 0 ? static_cast<size_t>(6)
                               : static_cast<size_t>(7));
        }

        const uint64_t schedulerBeforeShrink =
            streamer.workMetrics().schedulerCoordinatesInspected;
        stream.viewDistanceChunks = 0;
        stream.unloadDistanceChunks = 0;
        streamer.setConfig(stream);

        CHECK_EQ(
            streamer.workMetrics().schedulerCoordinatesInspected -
                schedulerBeforeShrink,
            static_cast<uint64_t>(8));
        CHECK_EQ(
            Rigel::Voxel::detail::ChunkStreamerTestAccess::
                configRetiredWorkCount(streamer),
            static_cast<size_t>(7));
        CHECK_EQ(streamer.diagnostics().mesh.pending,
                 static_cast<size_t>(1));
        CHECK_EQ(streamer.diagnostics().mesh.inFlight,
                 static_cast<size_t>(1));
        CHECK_NE(streamer.diagnostics().state,
                 StreamingLifecycleState::Quiescent);

        streamer.update(cameraCoords.back().toWorldCenter());
        CHECK_EQ(
            Rigel::Voxel::detail::ChunkStreamerTestAccess::
                configRetiredWorkCount(streamer),
            static_cast<size_t>(0));
        CHECK_EQ(
            streamer.workMetrics().lastUpdateSchedulerCoordinatesInspected,
            static_cast<uint64_t>(10));
        CHECK_EQ(streamer.workMetrics().meshJobsStarted,
                 static_cast<uint64_t>(1));
        CHECK_EQ(streamer.diagnostics().mesh.pending,
                 static_cast<size_t>(1));
        CHECK_EQ(streamer.diagnostics().mesh.inFlight,
                 static_cast<size_t>(1));

        streamer.processCompletions();
        CHECK_EQ(streamer.workMetrics().meshJobsCompleted,
                 static_cast<uint64_t>(1));
        CHECK_EQ(streamer.workMetrics().meshJobsAccepted,
                 static_cast<uint64_t>(0));
        CHECK_EQ(streamer.workMetrics().meshJobsRejectedStale,
                 static_cast<uint64_t>(1));
        CHECK_EQ(streamer.diagnostics().mesh.pending,
                 static_cast<size_t>(1));
        CHECK_EQ(streamer.diagnostics().mesh.inFlight,
                 static_cast<size_t>(0));

        streamer.update(cameraCoords.back().toWorldCenter());
        CHECK_EQ(streamer.workMetrics().meshJobsStarted,
                 static_cast<uint64_t>(2));
        CHECK_EQ(physicalBuilds, static_cast<size_t>(2));
        CHECK_EQ(streamer.diagnostics().mesh.pending,
                 static_cast<size_t>(0));
        CHECK_EQ(streamer.diagnostics().mesh.inFlight,
                 static_cast<size_t>(1));
        streamer.processCompletions();

        CHECK_EQ(streamer.workMetrics().meshJobsCompleted,
                 static_cast<uint64_t>(2));
        CHECK_EQ(streamer.workMetrics().meshJobsAccepted,
                 static_cast<uint64_t>(1));
        CHECK_EQ(streamer.workMetrics().meshJobsRejectedStale,
                 static_cast<uint64_t>(1));
        CHECK(streamer.diagnostics().mesh.empty());

        Chunk* current = manager.getChunk(cameraCoords.back());
        CHECK(current != nullptr);
        if (!current) {
            return;
        }
        for (int edit = 0; edit < 3; ++edit) {
            current->setBlock(
                2 + edit, 1, 1, BlockState{solid}, registry);
            streamer.update(cameraCoords.back().toWorldCenter());
            CHECK_EQ(streamer.workMetrics().meshJobsStarted,
                     static_cast<uint64_t>(3 + edit));
            CHECK_EQ(
                Rigel::Voxel::detail::ChunkStreamerTestAccess::
                    meshCompletionCount(streamer),
                static_cast<size_t>(1));
            CHECK_EQ(streamer.diagnostics().mesh.inFlight,
                     static_cast<size_t>(1));
            streamer.processCompletions();
            CHECK_EQ(streamer.workMetrics().meshJobsCompleted,
                     static_cast<uint64_t>(3 + edit));
            CHECK_EQ(streamer.workMetrics().meshJobsAccepted,
                     static_cast<uint64_t>(2 + edit));
            CHECK_EQ(streamer.workMetrics().meshJobsRejectedStale,
                     static_cast<uint64_t>(1));
            CHECK(streamer.diagnostics().mesh.empty());
        }

        for (uint32_t stable = 0;
             stable <= StreamingDiagnosticSnapshot::QuiescenceUpdateWindow;
             ++stable) {
            streamer.update(cameraCoords.back().toWorldCenter());
            streamer.processCompletions();
            CHECK_EQ(
                Rigel::Voxel::detail::ChunkStreamerTestAccess::
                    configRetiredWorkCount(streamer),
                static_cast<size_t>(0));
        }
        CHECK_EQ(streamer.workMetrics().meshJobsStarted,
                 static_cast<uint64_t>(5));
        CHECK_EQ(physicalBuilds, static_cast<size_t>(5));
        CHECK_EQ(
            streamer.workMetrics().lastUpdateSchedulerCoordinatesInspected,
            static_cast<uint64_t>(0));
        CHECK_EQ(streamer.diagnostics().state,
                 StreamingLifecycleState::Quiescent);

        CHECK_EQ(
            Rigel::Voxel::detail::ChunkStreamerTestAccess::
                meshDispatchLimit(streamer),
            static_cast<size_t>(1));
        CHECK_EQ(streamer.diagnostics().meshSubmissionLimit,
                 static_cast<size_t>(1));
    }
}

TEST_CASE(ChunkStreamer_ConfigRetiredMissingMeshesRecoverInCameraOrder) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    const BlockID solid = registerTestBlock(
        registry, "rigel:config_retired_missing_order");
    const ChunkCoord cameraCoord{0, 0, 0};
    const ChunkCoord nearCoord{1, 0, 0};
    const ChunkCoord farCoord{2, 0, 0};

    for (int z = -3; z <= 3; ++z) {
        for (int y = -3; y <= 3; ++y) {
            for (int x = -3; x <= 3; ++x) {
                Chunk& chunk = manager.getOrCreateChunk({x, y, z});
                chunk.setWorldGenVersion(generator->config().world.version);
                chunk.setLoadedFromDisk(true);
                chunk.clearPersistDirty();
                chunk.clearDirty();
            }
        }
    }
    Chunk& blocker = *manager.getChunk(cameraCoord);
    blocker.setBlock(0, 0, 0, BlockState{solid}, registry);
    blocker.clearPersistDirty();
    blocker.clearDirty();
    meshStore.set(cameraCoord, {});
    blocker.invalidateMesh();
    for (const ChunkCoord& coord : {nearCoord, farCoord}) {
        Chunk& chunk = *manager.getChunk(coord);
        chunk.setBlock(0, 0, 0, BlockState{solid}, registry);
        chunk.clearPersistDirty();
        chunk.clearDirty();
    }

    auto gate = std::make_shared<WorkerGate>();
    std::atomic<size_t> physicalBuilds{0};
    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
    WorkerGateRelease releaseOnExit(gate);
    StreamingConfig stream;
    stream.viewDistanceChunks = 2;
    stream.unloadDistanceChunks = 2;
    stream.meshQueueLimit = 1;
    stream.updateBudgetPerFrame = 0;
    stream.workerThreads = 2;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    streamer.markSpawnDiscoveryComplete();
    streamer.prioritizeMesh(cameraCoord);
    Rigel::Voxel::detail::ChunkStreamerTestAccess::setMeshBuildStartCallback(
        streamer,
        [gate, &physicalBuilds]() {
            if (physicalBuilds.fetch_add(1, std::memory_order_relaxed) == 0) {
                gate->enterAndWait();
            }
        });

    streamer.update(cameraCoord.toWorldCenter());
    CHECK(gate->waitUntilEntered());
    CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::hasReadyPendingMesh(
        streamer, nearCoord));
    CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::hasReadyPendingMesh(
        streamer, farCoord));
    CHECK_EQ(streamer.diagnostics().mesh.pending, static_cast<size_t>(2));

    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 0;
    streamer.setConfig(stream);
    CHECK_EQ(
        Rigel::Voxel::detail::ChunkStreamerTestAccess::configRetiredWorkCount(
            streamer),
        static_cast<size_t>(2));
    CHECK_EQ(streamer.diagnostics().mesh.pending, static_cast<size_t>(0));
    CHECK_EQ(streamer.diagnostics().mesh.inFlight, static_cast<size_t>(1));

    gate->release();
    CHECK(waitForMeshCompletions(streamer, 1));
    CHECK_EQ(streamer.workMetrics().meshJobsAccepted,
             static_cast<uint64_t>(1));
    CHECK_EQ(streamer.workMetrics().meshJobsRejectedStale,
             static_cast<uint64_t>(0));

    stream.viewDistanceChunks = 2;
    stream.unloadDistanceChunks = 2;
    stream.updateBudgetPerFrame = 1;
    streamer.setConfig(stream);
    CHECK_EQ(streamer.diagnostics().mesh.pending, static_cast<size_t>(2));
    CHECK_EQ(streamer.diagnostics().mesh.inFlight, static_cast<size_t>(0));

    streamer.update(cameraCoord.toWorldCenter());
    const auto firstRecovery =
        Rigel::Voxel::detail::ChunkStreamerTestAccess::
            inFlightMeshDispatchOrder(streamer);
    CHECK_EQ(firstRecovery.size(), static_cast<size_t>(1));
    if (!firstRecovery.empty()) {
        CHECK_EQ(firstRecovery.front(), nearCoord);
    }
    CHECK_EQ(
        Rigel::Voxel::detail::ChunkStreamerTestAccess::configRetiredWorkCount(
            streamer),
        static_cast<size_t>(0));
    CHECK_EQ(streamer.diagnostics().mesh.pending, static_cast<size_t>(1));
    CHECK_EQ(streamer.diagnostics().mesh.inFlight, static_cast<size_t>(1));
    CHECK_EQ(streamer.workMetrics().lastUpdateSchedulerCoordinatesInspected,
             static_cast<uint64_t>(5));
    CHECK(waitForMeshCompletions(streamer, 2));

    streamer.update(cameraCoord.toWorldCenter());
    const auto secondRecovery =
        Rigel::Voxel::detail::ChunkStreamerTestAccess::
            inFlightMeshDispatchOrder(streamer);
    CHECK_EQ(secondRecovery.size(), static_cast<size_t>(1));
    if (!secondRecovery.empty()) {
        CHECK_EQ(secondRecovery.front(), farCoord);
    }
    CHECK_EQ(streamer.workMetrics().lastUpdateSchedulerCoordinatesInspected,
             static_cast<uint64_t>(3));
    CHECK(waitForMeshCompletions(streamer, 3));

    CHECK_EQ(physicalBuilds.load(std::memory_order_relaxed),
             static_cast<size_t>(3));
    CHECK_EQ(streamer.workMetrics().meshJobsStarted,
             static_cast<uint64_t>(3));
    CHECK_EQ(streamer.workMetrics().meshJobsCompleted,
             static_cast<uint64_t>(3));
    CHECK_EQ(streamer.workMetrics().meshJobsAccepted,
             static_cast<uint64_t>(3));
    CHECK_EQ(streamer.workMetrics().meshJobsRejectedStale,
             static_cast<uint64_t>(0));
    CHECK(meshStore.contains(nearCoord));
    CHECK(meshStore.contains(farCoord));
    CHECK(streamer.diagnostics().mesh.empty());

    for (uint32_t stable = 0;
         stable < StreamingDiagnosticSnapshot::QuiescenceUpdateWindow;
         ++stable) {
        streamer.update(cameraCoord.toWorldCenter());
        streamer.processCompletions();
    }
    CHECK_EQ(streamer.workMetrics().lastUpdateSchedulerCoordinatesInspected,
             static_cast<uint64_t>(0));
    CHECK_EQ(streamer.diagnostics().state,
             StreamingLifecycleState::Quiescent);
}

TEST_CASE(ChunkStreamer_ReadyPendingMeshSurvivesNeighborWake) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    const BlockID solid =
        registerTestBlock(registry, "rigel:pending_wake_owner_solid");
    const ChunkCoord blockerCoord{0, 0, 0};
    const ChunkCoord preservedCoord{1, 0, 0};
    const ChunkCoord ordinaryCoord{0, 1, 0};
    const std::array<ChunkCoord, 7> desired{
        blockerCoord,
        preservedCoord,
        ChunkCoord{-1, 0, 0},
        ordinaryCoord,
        ChunkCoord{0, -1, 0},
        ChunkCoord{0, 0, 1},
        ChunkCoord{0, 0, -1}
    };
    for (const ChunkCoord& coord : desired) {
        Chunk& chunk = manager.getOrCreateChunk(coord);
        chunk.setBlock(0, 0, 0, BlockState{solid}, registry);
        chunk.setWorldGenVersion(generator->config().world.version);
        chunk.setLoadedFromDisk(true);
        chunk.clearPersistDirty();
        chunk.clearDirty();
        meshStore.set(coord, {});
    }
    manager.getChunk(blockerCoord)->invalidateMesh();
    manager.getChunk(preservedCoord)->invalidateMesh();
    manager.getChunk(ordinaryCoord)->invalidateMesh();

    auto gate = std::make_shared<WorkerGate>();
    std::atomic<size_t> buildsEntered{0};
    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
    WorkerGateRelease releaseOnExit(gate);
    StreamingConfig stream;
    stream.viewDistanceChunks = 1;
    stream.unloadDistanceChunks = 1;
    stream.meshQueueLimit = 1;
    stream.workerThreads = 2;
    streamer.setConfig(stream);
    streamer.prioritizeMesh(blockerCoord);
    Rigel::Voxel::detail::ChunkStreamerTestAccess::setMeshBuildStartCallback(
        streamer,
        [gate, &buildsEntered]() {
            if (buildsEntered.fetch_add(1, std::memory_order_relaxed) == 0) {
                gate->enterAndWait();
            }
        });

    streamer.update(blockerCoord.toWorldCenter());
    CHECK(gate->waitUntilEntered());
    CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::hasReadyPendingMesh(
        streamer, preservedCoord));
    CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::hasReadyPendingMesh(
        streamer, ordinaryCoord));

    streamer.prioritizeMesh(preservedCoord);
    const auto requestSequence =
        Rigel::Voxel::detail::ChunkStreamerTestAccess::pendingMeshSequence(
            streamer, preservedCoord);
    CHECK(requestSequence.has_value());
    CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::
        pendingMeshIsPrioritized(streamer, preservedCoord));

    Rigel::Voxel::detail::ChunkStreamerTestAccess::queueLoadedNeighbors(
        streamer, blockerCoord);

    CHECK_EQ(
        Rigel::Voxel::detail::ChunkStreamerTestAccess::pendingMeshSequence(
            streamer, preservedCoord),
        requestSequence);
    CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::
        pendingMeshIsPrioritized(streamer, preservedCoord));

    gate->release();
    CHECK(waitForMeshCompletions(streamer, 1));
    streamer.update(blockerCoord.toWorldCenter());
    const auto dispatchOrder =
        Rigel::Voxel::detail::ChunkStreamerTestAccess::
            inFlightMeshDispatchOrder(streamer);
    CHECK_EQ(dispatchOrder.size(), static_cast<size_t>(1));
    if (!dispatchOrder.empty()) {
        CHECK_EQ(dispatchOrder.front(), preservedCoord);
    }
    CHECK(waitForMeshCompletions(streamer, 2));
}

TEST_CASE(ChunkStreamer_UnloadShrinkRetiresFringeMeshImmediately) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    const BlockID solid =
        registerTestBlock(registry, "rigel:unload_shrink_fringe_solid");
    const ChunkCoord cameraCoord{0, 0, 0};
    const ChunkCoord fringeCoord{4, 0, 0};

    for (const ChunkCoord& coord : {cameraCoord, fringeCoord}) {
        Chunk& chunk = manager.getOrCreateChunk(coord);
        chunk.setBlock(0, 0, 0, BlockState{solid}, registry);
        chunk.setWorldGenVersion(generator->config().world.version);
        chunk.setLoadedFromDisk(true);
        chunk.clearPersistDirty();
        chunk.clearDirty();
        meshStore.set(coord, {});
        chunk.invalidateMesh();
    }

    auto gate = std::make_shared<WorkerGate>();
    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
    WorkerGateRelease releaseOnExit(gate);
    StreamingConfig stream;
    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 8;
    stream.meshQueueLimit = 1;
    stream.workerThreads = 2;
    streamer.setConfig(stream);
    streamer.prioritizeMesh(cameraCoord);
    Rigel::Voxel::detail::ChunkStreamerTestAccess::setMeshBuildStartCallback(
        streamer, [gate]() { gate->enterAndWait(); });

    streamer.update(cameraCoord.toWorldCenter());
    CHECK(gate->waitUntilEntered());
    CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::hasReadyPendingMesh(
        streamer, fringeCoord));
    CHECK_EQ(streamer.diagnostics().mesh.pending, static_cast<size_t>(1));

    stream.unloadDistanceChunks = 0;
    streamer.setConfig(stream);
    CHECK(!Rigel::Voxel::detail::ChunkStreamerTestAccess::hasReadyPendingMesh(
        streamer, fringeCoord));
    CHECK(!Rigel::Voxel::detail::ChunkStreamerTestAccess::
        hasDependencyPendingMesh(streamer, fringeCoord));
    CHECK_EQ(streamer.diagnostics().mesh.pending, static_cast<size_t>(0));
    gate->release();
}

TEST_CASE(ChunkStreamer_ConfigShrinkCancelsDepartedLoadBeforeNextUpdate) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    const ChunkCoord departedCoord{1, 0, 0};
    std::unordered_set<ChunkCoord, ChunkCoordHash> cancelled;

    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 1;
    stream.unloadDistanceChunks = 1;
    stream.updateBudgetPerFrame = 0;
    stream.workerThreads = 0;
    streamer.setConfig(stream);
    streamer.setChunkLoader([](ChunkLoadRequest) {
        return ChunkLoadRequestResult::Queued;
    });
    streamer.setChunkLoadCancel([&](ChunkCoord coord) {
        cancelled.insert(coord);
    });

    streamer.update(glm::vec3(0.0f));
    CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::hasPendingLoad(
        streamer, departedCoord));
    CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::desiredContains(
        streamer, departedCoord));

    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 0;
    streamer.setConfig(stream);
    CHECK(!Rigel::Voxel::detail::ChunkStreamerTestAccess::hasPendingLoad(
        streamer, departedCoord));
    CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::desiredContains(
        streamer, departedCoord));
    CHECK(cancelled.find(departedCoord) != cancelled.end());

    streamer.update(glm::vec3(0.0f));
    CHECK(!Rigel::Voxel::detail::ChunkStreamerTestAccess::hasPendingLoad(
        streamer, departedCoord));
    CHECK(!Rigel::Voxel::detail::ChunkStreamerTestAccess::desiredContains(
        streamer, departedCoord));
    CHECK(cancelled.find(departedCoord) != cancelled.end());
}

TEST_CASE(ChunkStreamer_ConfigShrinkRejectsCompletionsBeforeStreamingUpdate) {
    enum class CompletionKind {
        Load,
        Generation,
        Mesh
    };

    auto runCompletion = [&](int workerThreads,
                             CompletionKind completionKind,
                             bool completionFails) {
            ChunkManager manager;
            BlockRegistry registry;
            WorldMeshStore meshStore;
            auto generator = makeGenerator(registry);
            const BlockID solid = registerTestBlock(
                registry,
                "rigel:config_completion_admission_" +
                    std::to_string(workerThreads) + "_" +
                    std::to_string(static_cast<int>(completionKind)) + "_" +
                    std::to_string(completionFails));
            const ChunkCoord cameraCoord{0, 4, 0};
            const ChunkCoord retiringCoord{1, 4, 0};
            const std::array<ChunkCoord, 7> desired{
                cameraCoord,
                retiringCoord,
                ChunkCoord{-1, 4, 0},
                ChunkCoord{0, 5, 0},
                ChunkCoord{0, 3, 0},
                ChunkCoord{0, 4, 1},
                ChunkCoord{0, 4, -1}
            };
            for (const ChunkCoord& coord : desired) {
                if (coord == retiringCoord &&
                    completionKind != CompletionKind::Mesh) {
                    continue;
                }
                Chunk& chunk = manager.getOrCreateChunk(coord);
                chunk.setWorldGenVersion(generator->config().world.version);
                chunk.setLoadedFromDisk(true);
                chunk.clearPersistDirty();
                chunk.clearDirty();
            }
            if (completionKind == CompletionKind::Mesh) {
                Chunk& retiring = *manager.getChunk(retiringCoord);
                retiring.setBlock(
                    0, 0, 0, BlockState{solid}, registry);
                retiring.clearPersistDirty();
                retiring.clearDirty();
                addLoadedNeighborShell(
                    manager,
                    retiringCoord,
                    std::nullopt,
                    generator->config().world.version);
            }

            auto gate = std::make_shared<WorkerGate>();
            ChunkStreamer streamer(
                manager, meshStore, registry, nullptr, generator);
            WorkerGateRelease releaseOnExit(gate);
            StreamingConfig stream;
            stream.viewDistanceChunks = 1;
            stream.unloadDistanceChunks = 2;
            stream.genQueueLimit = 1;
            stream.meshQueueLimit = 1;
            stream.updateBudgetPerFrame = 0;
            stream.applyBudgetPerFrame = 0;
            stream.workerThreads = workerThreads;
            stream.maxResidentChunks = 0;
            streamer.setConfig(stream);
            streamer.markSpawnDiscoveryComplete();

            ChunkLoadRequestId loadRequestId = 0;
            bool loadCancelled = false;
            bool loadCompletionPending = false;
            size_t physicalLoadInFlight = 0;
            uint64_t physicalLoadsStarted = 0;
            if (completionKind == CompletionKind::Load) {
                streamer.setChunkLoader([&](ChunkLoadRequest request) {
                    CHECK_EQ(request.coord, retiringCoord);
                    loadRequestId = request.requestId;
                    loadCancelled = false;
                    loadCompletionPending = true;
                    physicalLoadInFlight = 1;
                    ++physicalLoadsStarted;
                    return ChunkLoadRequestResult::Queued;
                });
                streamer.setChunkLoadCancel([&](ChunkCoord coord) {
                    CHECK_EQ(coord, retiringCoord);
                    loadCancelled = true;
                });
                streamer.setChunkLoadDrain([&](size_t) {
                    if (!loadCompletionPending) {
                        return std::vector<ChunkLoadCompletion>{};
                    }
                    loadCompletionPending = false;
                    physicalLoadInFlight = 0;
                    if (!loadCancelled && !completionFails) {
                        Chunk& loaded =
                            manager.getOrCreateChunk(retiringCoord);
                        loaded.setBlock(
                            0, 0, 0, BlockState{solid}, registry);
                        loaded.setWorldGenVersion(
                            generator->config().world.version);
                        loaded.setLoadedFromDisk(true);
                        loaded.clearPersistDirty();
                        loaded.clearDirty();
                    }
                    return std::vector<ChunkLoadCompletion>{
                        {retiringCoord,
                         loadRequestId,
                         completionFails
                             ? ChunkLoadOutcome::Failed
                             : ChunkLoadOutcome::Loaded,
                         completionFails
                             ? "injected departed load failure"
                             : std::string{}}
                    };
                });
                streamer.setChunkLoadDiagnosticsCallback([&]() {
                    return ChunkLoadDiagnosticSnapshot{
                        .work = StreamingWorkCount{
                            .pending = loadCancelled
                                ? static_cast<size_t>(0)
                                : physicalLoadInFlight,
                            .inFlight = physicalLoadInFlight,
                            .started = physicalLoadsStarted
                        }
                    };
                });
            } else if (completionKind == CompletionKind::Generation &&
                       (workerThreads == 1 || completionFails)) {
                Rigel::Voxel::detail::ChunkStreamerTestAccess::
                    setGenerationStartCallback(
                        streamer,
                        [gate, workerThreads, completionFails]() {
                            if (workerThreads == 1) {
                                gate->enterAndWait();
                            }
                            if (completionFails) {
                                throw std::runtime_error(
                                    "injected departed generation failure");
                            }
                        });
            } else if (completionKind == CompletionKind::Mesh &&
                       completionFails) {
                Rigel::Voxel::detail::ChunkStreamerTestAccess::
                    setMeshBuildStartCallback(streamer, []() {
                        throw std::runtime_error(
                            "injected departed mesh failure");
                    });
            }

            streamer.update(cameraCoord.toWorldCenter());
            if (completionKind == CompletionKind::Generation &&
                workerThreads == 1) {
                CHECK(gate->waitUntilEntered());
            }
            if (completionKind == CompletionKind::Load) {
                CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::
                    hasPendingLoad(streamer, retiringCoord));
                CHECK_EQ(streamer.workMetrics().chunkLoadRequestsStarted,
                         static_cast<uint64_t>(1));
                CHECK_EQ(streamer.diagnostics().chunkLoad.pending,
                         static_cast<size_t>(1));
                CHECK_EQ(streamer.diagnostics().chunkLoad.inFlight,
                         static_cast<size_t>(1));
            } else if (completionKind == CompletionKind::Generation) {
                CHECK_EQ(streamer.workMetrics().generationJobsStarted,
                         static_cast<uint64_t>(1));
                CHECK_EQ(streamer.diagnostics().generation.inFlight,
                         static_cast<size_t>(1));
            } else {
                CHECK_EQ(streamer.workMetrics().meshJobsStarted,
                         static_cast<uint64_t>(1));
                CHECK_EQ(streamer.diagnostics().mesh.inFlight,
                         static_cast<size_t>(1));
            }

            const uint64_t meshStoreVersion = meshStore.version();
            stream.viewDistanceChunks = 0;
            stream.unloadDistanceChunks = 0;
            streamer.setConfig(stream);
            CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::
                desiredContains(streamer, retiringCoord));
            if (completionKind == CompletionKind::Load) {
                CHECK(loadCancelled);
                CHECK(!Rigel::Voxel::detail::ChunkStreamerTestAccess::
                    hasPendingLoad(streamer, retiringCoord));
                CHECK_EQ(streamer.diagnostics().chunkLoad.pending,
                         static_cast<size_t>(0));
                CHECK_EQ(streamer.diagnostics().chunkLoad.inFlight,
                         static_cast<size_t>(1));
            }

            gate->release();
            if (completionKind == CompletionKind::Generation) {
                CHECK(waitForGenerationCompletion(streamer));
                streamer.processCompletions();
            } else if (completionKind == CompletionKind::Mesh) {
                CHECK(waitForMeshCompletions(streamer, 1));
            } else {
                streamer.processCompletions();
            }

            CHECK_EQ(
                manager.hasChunk(retiringCoord),
                completionKind == CompletionKind::Mesh);
            CHECK_EQ(meshStore.version(), meshStoreVersion);
            CHECK(!meshStore.contains(retiringCoord));
            CHECK_EQ(streamer.workMetrics().generationJobsFailed,
                     static_cast<uint64_t>(0));
            CHECK_EQ(streamer.workMetrics().meshJobsFailed,
                     static_cast<uint64_t>(0));
            CHECK_EQ(streamer.diagnostics().generation.terminalErrors,
                     static_cast<size_t>(0));
            CHECK_EQ(streamer.diagnostics().chunkLoad.terminalErrors,
                     static_cast<size_t>(0));
            CHECK_EQ(streamer.diagnostics().mesh.terminalErrors,
                     static_cast<size_t>(0));
            if (completionKind == CompletionKind::Load) {
                CHECK_EQ(physicalLoadsStarted, static_cast<uint64_t>(1));
                CHECK_EQ(physicalLoadInFlight, static_cast<size_t>(0));
                CHECK_EQ(streamer.diagnostics().chunkLoad.started,
                         static_cast<uint64_t>(1));
                CHECK_EQ(streamer.diagnostics().chunkLoad.inFlight,
                         static_cast<size_t>(0));
            }
            if (completionKind == CompletionKind::Generation) {
                CHECK_EQ(streamer.diagnostics().generation.inFlight,
                         static_cast<size_t>(0));
            }
            if (completionKind == CompletionKind::Mesh) {
                CHECK_EQ(streamer.workMetrics().meshJobsCompleted,
                         static_cast<uint64_t>(1));
                CHECK_EQ(streamer.workMetrics().meshJobsAccepted,
                         static_cast<uint64_t>(0));
                CHECK_EQ(streamer.workMetrics().meshJobsRejectedStale,
                         static_cast<uint64_t>(1));
                CHECK_EQ(streamer.diagnostics().mesh.inFlight,
                         static_cast<size_t>(0));
            }

            if (!completionFails) {
                stream.viewDistanceChunks = 1;
                stream.unloadDistanceChunks = 2;
                streamer.setConfig(stream);
                CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::
                    desiredContains(streamer, retiringCoord));
                streamer.update(cameraCoord.toWorldCenter());
                CHECK_EQ(streamer.diagnostics().state,
                         StreamingLifecycleState::Streaming);

                if (completionKind == CompletionKind::Load) {
                    CHECK_EQ(physicalLoadsStarted, static_cast<uint64_t>(2));
                    CHECK_EQ(streamer.workMetrics().chunkLoadRequestsStarted,
                             static_cast<uint64_t>(2));
                    CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::
                        hasPendingLoad(streamer, retiringCoord));
                    CHECK(!manager.hasChunk(retiringCoord));
                    CHECK_EQ(streamer.diagnostics().chunkLoad.pending,
                             static_cast<size_t>(1));
                    CHECK_EQ(streamer.diagnostics().chunkLoad.inFlight,
                             static_cast<size_t>(1));
                    streamer.processCompletions();
                    CHECK(!meshStore.contains(retiringCoord));
                    streamer.update(cameraCoord.toWorldCenter());
                    CHECK(!meshStore.contains(retiringCoord));
                    CHECK_EQ(streamer.diagnostics().mesh.inFlight,
                             static_cast<size_t>(1));
                    streamer.processCompletions();
                    CHECK(manager.hasChunk(retiringCoord));
                    CHECK(!manager.getChunk(retiringCoord)->isDirty());
                    CHECK(meshStore.contains(retiringCoord));
                    CHECK_EQ(streamer.workMetrics().meshJobsStarted,
                             static_cast<uint64_t>(1));
                    CHECK_EQ(streamer.workMetrics().meshJobsCompleted,
                             static_cast<uint64_t>(1));
                    CHECK_EQ(streamer.workMetrics().meshJobsAccepted,
                             static_cast<uint64_t>(1));
                    CHECK_EQ(streamer.workMetrics().meshJobsRejectedStale,
                             static_cast<uint64_t>(0));
                } else if (completionKind == CompletionKind::Generation) {
                    CHECK_EQ(streamer.workMetrics().generationJobsStarted,
                             static_cast<uint64_t>(2));
                    CHECK(!manager.hasChunk(retiringCoord));
                    CHECK_EQ(streamer.diagnostics().generation.inFlight,
                             static_cast<size_t>(1));
                    CHECK(waitForGenerationCompletion(streamer));
                    streamer.processCompletions();
                    CHECK(manager.hasChunk(retiringCoord));
                    CHECK(!manager.getChunk(retiringCoord)->isDirty());
                } else {
                    CHECK_EQ(streamer.workMetrics().meshJobsStarted,
                             static_cast<uint64_t>(2));
                    CHECK(!meshStore.contains(retiringCoord));
                    CHECK_EQ(streamer.diagnostics().mesh.inFlight,
                             static_cast<size_t>(1));
                    CHECK(waitForMeshCompletions(streamer, 2));
                    CHECK(meshStore.contains(retiringCoord));
                    CHECK(!manager.getChunk(retiringCoord)->isDirty());
                }

                CHECK_EQ(streamer.workMetrics().generationJobsFailed,
                         static_cast<uint64_t>(0));
                CHECK_EQ(streamer.workMetrics().meshJobsFailed,
                         static_cast<uint64_t>(0));
                CHECK_EQ(streamer.diagnostics().generation.terminalErrors,
                         static_cast<size_t>(0));
                CHECK_EQ(streamer.diagnostics().chunkLoad.terminalErrors,
                         static_cast<size_t>(0));
                CHECK_EQ(streamer.diagnostics().mesh.terminalErrors,
                         static_cast<size_t>(0));
                if (completionKind == CompletionKind::Mesh) {
                    CHECK_EQ(streamer.workMetrics().meshJobsCompleted,
                             static_cast<uint64_t>(2));
                    CHECK_EQ(streamer.workMetrics().meshJobsAccepted,
                             static_cast<uint64_t>(1));
                    CHECK_EQ(streamer.workMetrics().meshJobsRejectedStale,
                             static_cast<uint64_t>(1));
                }

                stream.viewDistanceChunks = 0;
                stream.unloadDistanceChunks = 0;
                streamer.setConfig(stream);
            }
            streamer.update(cameraCoord.toWorldCenter());
            streamer.processCompletions();
            CHECK(!Rigel::Voxel::detail::ChunkStreamerTestAccess::
                desiredContains(streamer, retiringCoord));
            CHECK(!manager.hasChunk(retiringCoord));
            for (uint32_t stable = 0;
                 stable < StreamingDiagnosticSnapshot::QuiescenceUpdateWindow;
                 ++stable) {
                streamer.update(cameraCoord.toWorldCenter());
                streamer.processCompletions();
            }
            CHECK(streamer.diagnostics().generation.empty());
            CHECK(streamer.diagnostics().chunkLoad.empty());
            CHECK(streamer.diagnostics().mesh.empty());
            CHECK(streamer.diagnostics().eviction.empty());
            CHECK_EQ(streamer.diagnostics().state,
                     StreamingLifecycleState::Quiescent);
            CHECK_EQ(
                meshStore.version(),
                meshStoreVersion +
                    ((!completionFails &&
                      completionKind != CompletionKind::Generation)
                         ? static_cast<uint64_t>(2)
                         : static_cast<uint64_t>(0)));
            if (completionKind == CompletionKind::Load) {
                const uint64_t expectedLoads = completionFails ? 1 : 2;
                CHECK_EQ(physicalLoadsStarted, expectedLoads);
                CHECK_EQ(streamer.workMetrics().chunkLoadRequestsStarted,
                         expectedLoads);
                CHECK_EQ(physicalLoadInFlight, static_cast<size_t>(0));
                CHECK_EQ(streamer.diagnostics().chunkLoad.started,
                         expectedLoads);
                CHECK_EQ(streamer.diagnostics().chunkLoad.inFlight,
                         static_cast<size_t>(0));
            } else if (completionKind == CompletionKind::Generation) {
                CHECK_EQ(
                    streamer.workMetrics().generationJobsStarted,
                    completionFails ? static_cast<uint64_t>(1)
                                    : static_cast<uint64_t>(2));
            } else {
                CHECK_EQ(
                    streamer.workMetrics().meshJobsStarted,
                    completionFails ? static_cast<uint64_t>(1)
                                    : static_cast<uint64_t>(2));
            }
    };

    for (const int workerThreads : {0, 1}) {
        for (const CompletionKind completionKind :
             {CompletionKind::Load,
              CompletionKind::Generation,
              CompletionKind::Mesh}) {
            runCompletion(workerThreads, completionKind, false);
        }
    }
    for (const CompletionKind completionKind :
         {CompletionKind::Load,
          CompletionKind::Generation,
          CompletionKind::Mesh}) {
        runCompletion(0, completionKind, true);
    }
}

TEST_CASE(ChunkStreamer_UnloadShrinkRejectsDirtyMeshCompletionBeforeUpdate) {
    for (const int workerThreads : {0, 1}) {
        ChunkManager manager;
        BlockRegistry registry;
        WorldMeshStore meshStore;
        auto generator = makeGenerator(registry);
        const BlockID solid = registerTestBlock(
            registry,
            "rigel:unload_completion_admission_" +
                std::to_string(workerThreads));
        const ChunkCoord cameraCoord{0, 4, 0};
        const ChunkCoord fringeCoord{1, 4, 0};

        Chunk& camera = manager.getOrCreateChunk(cameraCoord);
        camera.setWorldGenVersion(generator->config().world.version);
        camera.setLoadedFromDisk(true);
        camera.clearPersistDirty();
        camera.clearDirty();
        Chunk& fringe = manager.getOrCreateChunk(fringeCoord);
        fringe.setBlock(0, 0, 0, BlockState{solid}, registry);
        fringe.setWorldGenVersion(generator->config().world.version);
        fringe.setLoadedFromDisk(true);
        fringe.clearPersistDirty();
        fringe.clearDirty();
        meshStore.set(fringeCoord, {});
        fringe.invalidateMesh();

        ChunkStreamer streamer(
            manager, meshStore, registry, nullptr, generator);
        StreamingConfig stream;
        stream.viewDistanceChunks = 0;
        stream.unloadDistanceChunks = 2;
        stream.meshQueueLimit = 1;
        stream.workerThreads = workerThreads;
        stream.maxResidentChunks = 0;
        streamer.setConfig(stream);
        streamer.markSpawnDiscoveryComplete();

        streamer.update(cameraCoord.toWorldCenter());
        CHECK_EQ(streamer.workMetrics().meshJobsStarted,
                 static_cast<uint64_t>(1));
        CHECK_EQ(streamer.diagnostics().mesh.inFlight,
                 static_cast<size_t>(1));
        CHECK_EQ(
            Rigel::Voxel::detail::ChunkStreamerTestAccess::
                meshCompletionCount(streamer),
            static_cast<size_t>(1));
        const uint64_t meshStoreVersion = meshStore.version();
        const uint64_t installedRevision =
            installedMeshRevision(meshStore, fringeCoord);

        stream.unloadDistanceChunks = 0;
        streamer.setConfig(stream);
        streamer.processCompletions();

        CHECK_EQ(streamer.workMetrics().meshJobsCompleted,
                 static_cast<uint64_t>(1));
        CHECK_EQ(streamer.workMetrics().meshJobsAccepted,
                 static_cast<uint64_t>(0));
        CHECK_EQ(streamer.workMetrics().meshJobsRejectedStale,
                 static_cast<uint64_t>(1));
        CHECK_EQ(streamer.workMetrics().meshJobsFailed,
                 static_cast<uint64_t>(0));
        CHECK_EQ(streamer.diagnostics().mesh.inFlight,
                 static_cast<size_t>(0));
        CHECK_EQ(streamer.diagnostics().mesh.terminalErrors,
                 static_cast<size_t>(0));
        CHECK_EQ(meshStore.version(), meshStoreVersion);
        CHECK_EQ(installedMeshRevision(meshStore, fringeCoord),
                 installedRevision);
        CHECK(meshStore.contains(fringeCoord));

        streamer.update(cameraCoord.toWorldCenter());
        streamer.processCompletions();
        CHECK(!manager.hasChunk(fringeCoord));
        CHECK(!meshStore.contains(fringeCoord));
        for (uint32_t stable = 0;
             stable < StreamingDiagnosticSnapshot::QuiescenceUpdateWindow;
             ++stable) {
            streamer.update(cameraCoord.toWorldCenter());
            streamer.processCompletions();
        }
        CHECK(streamer.diagnostics().generation.empty());
        CHECK(streamer.diagnostics().chunkLoad.empty());
        CHECK(streamer.diagnostics().mesh.empty());
        CHECK(streamer.diagnostics().eviction.empty());
        CHECK_EQ(streamer.diagnostics().state,
                 StreamingLifecycleState::Quiescent);
    }
}

TEST_CASE(ChunkStreamer_ConfigShrinkRetiresVersionReplacementWait) {
    enum class GenerationPhase {
        Submitted,
        CapacityWaiting,
        Completed
    };

    for (const GenerationPhase phase :
         {GenerationPhase::Submitted,
          GenerationPhase::CapacityWaiting,
          GenerationPhase::Completed}) {
        ChunkManager manager;
        BlockRegistry registry;
        WorldMeshStore meshStore;
        auto originalGenerator = makeGenerator(registry);
        WorldGenConfig replacementConfig = originalGenerator->config();
        ++replacementConfig.world.version;
        auto replacementGenerator =
            std::make_shared<WorldGenerator>(registry, replacementConfig);

        const ChunkCoord cameraCoord{0, 4, 0};
        const ChunkCoord retiringCoord{1, 4, 0};
        const std::array<ChunkCoord, 7> desired{
            cameraCoord,
            retiringCoord,
            ChunkCoord{-1, 4, 0},
            ChunkCoord{0, 5, 0},
            ChunkCoord{0, 3, 0},
            ChunkCoord{0, 4, 1},
            ChunkCoord{0, 4, -1}
        };
        for (const ChunkCoord& coord : desired) {
            if (phase == GenerationPhase::CapacityWaiting &&
                coord == cameraCoord) {
                continue;
            }
            Chunk& chunk = manager.getOrCreateChunk(coord);
            chunk.setWorldGenVersion(
                coord == retiringCoord
                    ? originalGenerator->config().world.version
                    : replacementGenerator->config().world.version);
            chunk.setLoadedFromDisk(true);
            chunk.clearPersistDirty();
            chunk.clearDirty();
        }

        auto gate = std::make_shared<WorkerGate>();
        std::atomic<size_t> generationsEntered{0};
        ChunkStreamer streamer(
            manager, meshStore, registry, nullptr, replacementGenerator);
        WorkerGateRelease releaseOnExit(gate);
        StreamingConfig stream;
        stream.viewDistanceChunks = 1;
        stream.unloadDistanceChunks = 1;
        stream.genQueueLimit = 1;
        stream.meshQueueLimit = 0;
        stream.updateBudgetPerFrame = 0;
        stream.applyBudgetPerFrame = 0;
        stream.workerThreads = phase == GenerationPhase::Completed ? 0 : 1;
        stream.maxResidentChunks = 0;
        streamer.setConfig(stream);
        streamer.markSpawnDiscoveryComplete();
        if (phase != GenerationPhase::Completed) {
            Rigel::Voxel::detail::ChunkStreamerTestAccess::
                setGenerationStartCallback(
                    streamer,
                    [gate, &generationsEntered]() {
                        generationsEntered.fetch_add(
                            1, std::memory_order_relaxed);
                        gate->enterAndWait();
                    });
        }

        streamer.update(cameraCoord.toWorldCenter());
        if (phase != GenerationPhase::Completed) {
            CHECK(gate->waitUntilEntered());
        }
        CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::desiredContains(
            streamer, retiringCoord));
        CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::
            hasVersionReplacementWait(streamer, retiringCoord));
        CHECK_EQ(streamer.diagnostics().eviction.pending,
                 static_cast<size_t>(1));
        CHECK_EQ(streamer.workMetrics().generationJobsStarted,
                 static_cast<uint64_t>(1));
        CHECK_EQ(
            Rigel::Voxel::detail::ChunkStreamerTestAccess::
                hasGenerationCapacityWait(streamer, retiringCoord),
            phase == GenerationPhase::CapacityWaiting);
        CHECK_EQ(
            Rigel::Voxel::detail::ChunkStreamerTestAccess::
                hasSubmittedGeneration(streamer, retiringCoord),
            phase != GenerationPhase::CapacityWaiting);
        if (phase == GenerationPhase::Completed) {
            CHECK_EQ(
                Rigel::Voxel::detail::ChunkStreamerTestAccess::
                    generationCompletionCount(streamer),
                static_cast<size_t>(1));
        }

        stream.viewDistanceChunks = 0;
        stream.unloadDistanceChunks = 0;
        streamer.setConfig(stream);
        CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::desiredContains(
            streamer, retiringCoord));
        CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::
            hasVersionReplacementWait(streamer, retiringCoord));

        streamer.update(cameraCoord.toWorldCenter());
        CHECK(!Rigel::Voxel::detail::ChunkStreamerTestAccess::desiredContains(
            streamer, retiringCoord));
        CHECK(!Rigel::Voxel::detail::ChunkStreamerTestAccess::
            hasGenerationCapacityWait(streamer, retiringCoord));
        CHECK(!Rigel::Voxel::detail::ChunkStreamerTestAccess::
            hasSubmittedGeneration(streamer, retiringCoord));
        CHECK(!Rigel::Voxel::detail::ChunkStreamerTestAccess::
            hasVersionReplacementWait(streamer, retiringCoord));
        CHECK_EQ(streamer.diagnostics().eviction.pending,
                 static_cast<size_t>(0));

        gate->release();
        if (phase != GenerationPhase::Completed) {
            CHECK(waitForGenerationCompletion(streamer));
        }
        streamer.processCompletions();
        CHECK_EQ(streamer.workMetrics().generationJobsStarted,
                 static_cast<uint64_t>(1));
        CHECK_EQ(generationsEntered.load(std::memory_order_relaxed),
                 phase == GenerationPhase::Completed
                     ? static_cast<size_t>(0)
                     : static_cast<size_t>(1));
        CHECK(!manager.hasChunk(retiringCoord));
        CHECK(!Rigel::Voxel::detail::ChunkStreamerTestAccess::
            hasVersionReplacementWait(streamer, retiringCoord));
        CHECK(streamer.diagnostics().generation.empty());
        CHECK(streamer.diagnostics().eviction.empty());

        for (uint32_t stable = 0;
             stable < StreamingDiagnosticSnapshot::QuiescenceUpdateWindow;
             ++stable) {
            streamer.update(cameraCoord.toWorldCenter());
            streamer.processCompletions();
        }
        CHECK_EQ(streamer.workMetrics().generationJobsStarted,
                 static_cast<uint64_t>(1));
        CHECK_EQ(streamer.diagnostics().state,
                 StreamingLifecycleState::Quiescent);
    }
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

TEST_CASE(ChunkStreamer_PendingMeshPromotionKeepsOneDispatchableRequest) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    const BlockID solid =
        registerTestBlock(registry, "rigel:pending_incarnation_solid");
    const ChunkCoord blockerCoord{0, 0, 0};
    const ChunkCoord promotedCoord{1, 0, 0};

    const std::array<ChunkCoord, 7> desired{
        blockerCoord,
        promotedCoord,
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
        if (coord == blockerCoord || coord == promotedCoord) {
            chunk.setBlock(0, 0, 0, BlockState{solid}, registry);
            meshStore.set(coord, {});
        }
        chunk.clearDirty();
    }
    manager.getChunk(blockerCoord)->invalidateMesh();
    manager.getChunk(promotedCoord)->invalidateMesh();

    auto gate = std::make_shared<WorkerGate>();
    std::atomic<size_t> buildsEntered{0};
    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 1;
    stream.unloadDistanceChunks = 1;
    stream.meshQueueLimit = 1;
    stream.workerThreads = 2;
    streamer.setConfig(stream);
    streamer.prioritizeMesh(blockerCoord);
    Rigel::Voxel::detail::ChunkStreamerTestAccess::setMeshBuildStartCallback(
        streamer,
        [gate, &buildsEntered]() {
            if (buildsEntered.fetch_add(1, std::memory_order_relaxed) == 0) {
                gate->enterAndWait();
            }
        });
    WorkerGateRelease releaseOnExit(gate);

    streamer.update(glm::vec3(0.0f));
    CHECK(gate->waitUntilEntered());
    for (size_t update = 0;
         update < desired.size() &&
         !Rigel::Voxel::detail::ChunkStreamerTestAccess::hasReadyPendingMesh(
             streamer, promotedCoord);
         ++update) {
        streamer.update(glm::vec3(0.0f));
    }
    CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::hasReadyPendingMesh(
        streamer, promotedCoord));
    streamer.prioritizeMesh(promotedCoord);

    gate->release();
    CHECK(waitForMeshCompletions(streamer, 1));
    streamer.update(glm::vec3(0.0f));
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(2));
    CHECK(waitForMeshCompletions(streamer, 2));

    Chunk& promoted = *manager.getChunk(promotedCoord);
    promoted.setBlock(1, 0, 0, BlockState{solid}, registry);
    const uint64_t jobsBeforeRedispatch = streamer.workMetrics().meshJobsStarted;
    streamer.update(glm::vec3(0.0f));

    CHECK_EQ(streamer.workMetrics().meshJobsStarted, jobsBeforeRedispatch + 1);
    CHECK(streamer.workMetrics().lastUpdateSchedulerCoordinatesInspected >=
          static_cast<uint64_t>(2));
    CHECK(waitForMeshCompletions(streamer, jobsBeforeRedispatch + 1));
    CHECK_EQ(streamer.workMetrics().meshJobsAccepted,
             streamer.workMetrics().meshJobsStarted);
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
    std::vector<ChunkStreamer::DebugChunkState> states;
    streamer.getDebugStates(states, coord, 0);
    CHECK_EQ(states.size(), static_cast<size_t>(1));
    CHECK_EQ(states.front().state,
             ChunkStreamer::DebugState::WaitingForData);
    CHECK_EQ(states.front().pipelineOwner,
             ChunkStreamer::DebugPipelineOwner::WaitingForData);
    CHECK_EQ(states.front().voxelOccupancy,
             ChunkStreamer::DebugVoxelOccupancy::Unknown);
    CHECK_EQ(states.front().installedGeometry,
             ChunkStreamer::DebugInstalledGeometry::None);
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
    streamer.getDebugStates(states, coord, 0);
    CHECK_EQ(states.size(), static_cast<size_t>(1));
    CHECK_EQ(states.front().state,
             ChunkStreamer::DebugState::TerminalFailure);
    CHECK_EQ(states.front().pipelineOwner,
             ChunkStreamer::DebugPipelineOwner::TerminalFailure);
    CHECK_EQ(states.front().failure, ChunkStreamer::DebugFailure::Load);

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

TEST_CASE(ChunkStreamer_VisibilityTraceSeparatesSchedulerPoolAndWorkerTime) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    BlockID solid = registerTestBlock(registry, "rigel:trace_solid");
    const ChunkCoord coord{0, 0, 0};

    Chunk& chunk = manager.getOrCreateChunk(coord);
    chunk.setBlock(0, 0, 0, BlockState{solid}, registry);
    chunk.setWorldGenVersion(generator->config().world.version);
    chunk.setLoadedFromDisk(true);

    auto clock = std::make_shared<IncrementingTraceClock>();
    auto tracer = std::make_shared<ChunkVisibilityTracer>(
        ChunkVisibilityTracer::Config{coord, 4},
        [clock]() { return clock->now(); });
    auto gate = std::make_shared<WorkerGate>();

    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
    WorkerGateRelease releaseOnExit(gate);
    StreamingConfig stream;
    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 0;
    stream.meshQueueLimit = 1;
    stream.updateBudgetPerFrame = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 2;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    streamer.setVisibilityTracer(tracer);
    Rigel::Voxel::detail::ChunkStreamerTestAccess::setMeshBuildStartCallback(
        streamer,
        [gate]() { gate->enterAndWait(); });

    streamer.update(coord.toWorldCenter());
    bool workerEntered = gate->waitUntilEntered();
    if (!workerEntered) {
        gate->release();
    }
    CHECK(workerEntered);

    auto records = tracer->snapshot();
    CHECK_EQ(records.size(), static_cast<size_t>(1));
    CHECK(records.front().stage(ChunkVisibilityStage::Desired).has_value());
    CHECK(!records.front().stage(ChunkVisibilityStage::DataRequest).has_value());
    CHECK(!records.front().stage(ChunkVisibilityStage::DataReady).has_value());
    CHECK(!records.front().stage(ChunkVisibilityStage::NeighborReady).has_value());
    CHECK(records.front().stage(ChunkVisibilityStage::MeshEligible).has_value());
    CHECK(records.front().stage(ChunkVisibilityStage::SchedulerWait).has_value());
    CHECK(records.front().stage(ChunkVisibilityStage::PoolSubmit).has_value());
    CHECK(records.front().stage(ChunkVisibilityStage::WorkerStart).has_value());
    CHECK(!records.front().stage(ChunkVisibilityStage::WorkerFinish).has_value());
    CHECK_EQ(records.front().outcome, ChunkVisibilityOutcome::Pending);

    gate->release();
    CHECK(waitForMeshCompletions(streamer, 1));
    records = tracer->snapshot();
    CHECK_EQ(
        records.front().outcome,
        ChunkVisibilityOutcome::AcceptedNonemptyGeometry);
    CHECK(records.front().stage(ChunkVisibilityStage::WorkerFinish).has_value());
    CHECK(records.front().stage(ChunkVisibilityStage::ResultAccepted).has_value());
    CHECK(!records.front().stage(ChunkVisibilityStage::FirstDraw).has_value());

    CHECK(records.front().meshTask.has_value());
    CHECK_EQ(
        records.front().origin,
        ChunkVisibilityOrigin::ResidentLeftCensored);
    CHECK_EQ(
        records.front().meshTask->requestId,
        static_cast<uint64_t>(1));
    CHECK_EQ(
        records.front().meshTask->revision,
        chunk.meshRevision());

    const std::array orderedStages{
        ChunkVisibilityStage::Desired,
        ChunkVisibilityStage::MeshEligible,
        ChunkVisibilityStage::SchedulerWait,
        ChunkVisibilityStage::PoolSubmit,
        ChunkVisibilityStage::WorkerStart,
        ChunkVisibilityStage::WorkerFinish,
        ChunkVisibilityStage::ResultAccepted
    };
    for (size_t index = 1; index < orderedStages.size(); ++index) {
        const auto previous =
            records.front().stage(orderedStages[index - 1]);
        const auto current = records.front().stage(orderedStages[index]);
        CHECK(previous.has_value());
        CHECK(current.has_value());
        CHECK(*previous <= *current);
    }

    const auto durations = records.front().durations();
    CHECK(durations.schedulerWait.has_value());
    CHECK(durations.poolWait.has_value());
    CHECK(durations.workerExecution.has_value());
    CHECK(*durations.schedulerWait > ChunkVisibilityDuration::zero());
    CHECK(*durations.poolWait > ChunkVisibilityDuration::zero());
    CHECK(*durations.workerExecution > ChunkVisibilityDuration::zero());

    const size_t settledClockReads = clock->reads();
    const uint64_t settledMeshJobs = streamer.workMetrics().meshJobsStarted;
    streamer.update(coord.toWorldCenter());
    streamer.processCompletions();
    streamer.update(coord.toWorldCenter());
    streamer.processCompletions();
    CHECK_EQ(clock->reads(), settledClockReads);
    CHECK_EQ(tracer->snapshot().size(), static_cast<size_t>(1));
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, settledMeshJobs);
    CHECK_EQ(
        streamer.workMetrics().lastUpdateSchedulerCoordinatesInspected,
        static_cast<uint64_t>(0));
}

TEST_CASE(ChunkStreamer_VisibilityTraceRecordsSynchronousLoadReadiness) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    BlockID solid = registerTestBlock(
        registry, "rigel:trace_sync_load_solid");
    const ChunkCoord coord{0, 0, 0};
    auto tracer = std::make_shared<ChunkVisibilityTracer>(
        ChunkVisibilityTracer::Config{coord, 2});

    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 0;
    stream.workerThreads = 0;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    streamer.setVisibilityTracer(tracer);
    streamer.setChunkLoader([&](ChunkLoadRequest) {
        Chunk& chunk = manager.getOrCreateChunk(coord);
        chunk.setBlock(0, 0, 0, BlockState{solid}, registry);
        chunk.setWorldGenVersion(generator->config().world.version);
        chunk.setLoadedFromDisk(true);
        chunk.clearPersistDirty();
        return ChunkLoadRequestResult::Queued;
    });

    streamer.update(coord.toWorldCenter());
    streamer.processCompletions();

    const auto measurement = tracer->measurement();
    CHECK_EQ(measurement.records.size(), static_cast<size_t>(1));
    const auto& record = measurement.records.front();
    CHECK_EQ(record.origin, ChunkVisibilityOrigin::Persisted);
    const auto dataRequest =
        record.stage(ChunkVisibilityStage::DataRequest);
    const auto dataReady = record.stage(ChunkVisibilityStage::DataReady);
    CHECK(dataRequest.has_value());
    CHECK(dataReady.has_value());
    CHECK(*dataRequest <= *dataReady);
    CHECK(!record.stage(ChunkVisibilityStage::NeighborReady).has_value());
    CHECK(record.stage(ChunkVisibilityStage::MeshEligible).has_value());
    CHECK_EQ(
        record.outcome,
        ChunkVisibilityOutcome::AcceptedNonemptyGeometry);
    CHECK(record.meshTask.has_value());
}

TEST_CASE(ChunkStreamer_VisibilityTraceRecordsPolledLoadReadiness) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    BlockID solid = registerTestBlock(
        registry, "rigel:trace_polled_load_solid");
    const ChunkCoord coord{0, 0, 0};
    auto tracer = std::make_shared<ChunkVisibilityTracer>(
        ChunkVisibilityTracer::Config{coord, 2});
    bool pending = true;

    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 0;
    stream.workerThreads = 0;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    streamer.setVisibilityTracer(tracer);
    streamer.setChunkPendingCallback([&](ChunkCoord) { return pending; });

    streamer.update(coord.toWorldCenter());
    auto records = tracer->snapshot();
    CHECK_EQ(records.size(), static_cast<size_t>(1));
    CHECK(records.front().stage(ChunkVisibilityStage::DataRequest).has_value());
    CHECK(!records.front().stage(ChunkVisibilityStage::DataReady).has_value());

    Chunk& chunk = manager.getOrCreateChunk(coord);
    chunk.setBlock(0, 0, 0, BlockState{solid}, registry);
    chunk.setWorldGenVersion(generator->config().world.version);
    chunk.setLoadedFromDisk(true);
    chunk.clearPersistDirty();
    pending = false;
    streamer.update(coord.toWorldCenter());
    streamer.processCompletions();

    records = tracer->snapshot();
    CHECK_EQ(records.front().origin, ChunkVisibilityOrigin::Persisted);
    CHECK(records.front().stage(ChunkVisibilityStage::DataReady).has_value());
    CHECK(!records.front().stage(ChunkVisibilityStage::NeighborReady).has_value());
    CHECK_EQ(
        records.front().outcome,
        ChunkVisibilityOutcome::AcceptedNonemptyGeometry);
}

TEST_CASE(ChunkStreamer_VisibilityTraceRecordsCallbackRepollReadinessOnce) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    BlockID solid = registerTestBlock(
        registry, "rigel:trace_callback_repoll_solid");
    const ChunkCoord coord{0, 0, 0};
    addLoadedNeighborShell(
        manager,
        coord,
        std::nullopt,
        generator->config().world.version);
    auto clock = std::make_shared<IncrementingTraceClock>();
    auto tracer = std::make_shared<ChunkVisibilityTracer>(
        ChunkVisibilityTracer::Config{coord, 2},
        [clock]() { return clock->now(); });
    std::vector<ChunkLoadRequestId> requestIds;
    size_t cancellations = 0;
    size_t clockReadsAtCancellation = 0;

    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 1;
    stream.workerThreads = 0;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    streamer.setVisibilityTracer(tracer);
    streamer.setChunkLoader([&](ChunkLoadRequest request) {
        requestIds.push_back(request.requestId);
        if (requestIds.size() == 2) {
            Chunk& chunk = manager.getOrCreateChunk(coord);
            chunk.setBlock(0, 0, 0, BlockState{solid}, registry);
            chunk.setWorldGenVersion(generator->config().world.version);
            chunk.setLoadedFromDisk(true);
            chunk.clearPersistDirty();
        }
        return ChunkLoadRequestResult::Queued;
    });
    streamer.setChunkLoadCancel([&](ChunkCoord cancelled) {
        CHECK_EQ(cancelled, coord);
        ++cancellations;
        clockReadsAtCancellation = clock->reads();
    });

    streamer.update(coord.toWorldCenter());
    const auto pendingMeasurement = tracer->measurement();
    CHECK_EQ(requestIds.size(), static_cast<size_t>(1));
    CHECK_EQ(
        streamer.workMetrics().chunkLoadRequestsStarted,
        static_cast<uint64_t>(1));
    CHECK_EQ(streamer.diagnostics().chunkLoad.pending, static_cast<size_t>(1));
    CHECK_EQ(pendingMeasurement.records.size(), static_cast<size_t>(1));
    const auto lifecycleKey = pendingMeasurement.records.front().key;
    CHECK(!pendingMeasurement.records.front()
               .stage(ChunkVisibilityStage::DataReady)
               .has_value());

    streamer.update(coord.toWorldCenter());
    streamer.processCompletions();
    const auto settledMeasurement = tracer->measurement();

    CHECK_EQ(requestIds.size(), static_cast<size_t>(2));
    CHECK_EQ(requestIds.front(), requestIds.back());
    CHECK_EQ(cancellations, static_cast<size_t>(1));
    CHECK_EQ(clockReadsAtCancellation, static_cast<size_t>(2));
    CHECK_EQ(
        streamer.workMetrics().chunkLoadRequestsStarted,
        static_cast<uint64_t>(1));
    CHECK_EQ(streamer.diagnostics().chunkLoad.pending, static_cast<size_t>(0));
    CHECK_EQ(settledMeasurement.records.size(), static_cast<size_t>(1));
    const auto& record = settledMeasurement.records.front();
    CHECK_EQ(record.key, lifecycleKey);
    const auto dataReady = record.stage(ChunkVisibilityStage::DataReady);
    const auto meshEligible =
        record.stage(ChunkVisibilityStage::MeshEligible);
    const auto schedulerWait =
        record.stage(ChunkVisibilityStage::SchedulerWait);
    CHECK(dataReady.has_value());
    CHECK(meshEligible.has_value());
    CHECK(schedulerWait.has_value());
    CHECK(*dataReady < *meshEligible);
    CHECK_EQ(*meshEligible, *schedulerWait);
    CHECK_EQ(
        record.outcome,
        ChunkVisibilityOutcome::AcceptedNonemptyGeometry);
    const auto poolSubmit = record.stage(ChunkVisibilityStage::PoolSubmit);
    const auto workerStart = record.stage(ChunkVisibilityStage::WorkerStart);
    const auto workerFinish = record.stage(ChunkVisibilityStage::WorkerFinish);
    const auto resultAccepted =
        record.stage(ChunkVisibilityStage::ResultAccepted);
    CHECK(poolSubmit.has_value());
    CHECK(workerStart.has_value());
    CHECK(workerFinish.has_value());
    CHECK(resultAccepted.has_value());
    CHECK(*schedulerWait < *poolSubmit);
    CHECK(*poolSubmit < *workerStart);
    CHECK(*workerStart < *workerFinish);
    CHECK(*workerFinish < *resultAccepted);

    streamer.update(coord.toWorldCenter());
    streamer.processCompletions();
    const auto repeatedMeasurement = tracer->measurement();
    CHECK_EQ(repeatedMeasurement.sequence, settledMeasurement.sequence);
    CHECK_EQ(clock->reads(), static_cast<size_t>(8));
    CHECK_EQ(
        streamer.workMetrics().chunkLoadRequestsStarted,
        static_cast<uint64_t>(1));
}

TEST_CASE(ChunkStreamer_VisibilityTraceFaceNeighborClassificationWidensFirst) {
    constexpr int32_t minimum = std::numeric_limits<int32_t>::min();
    constexpr int32_t maximum = std::numeric_limits<int32_t>::max();
    using Access = Rigel::Voxel::detail::ChunkStreamerTestAccess;

    CHECK(Access::areFaceNeighbors(
        ChunkCoord{minimum, 0, 0}, ChunkCoord{minimum + 1, 0, 0}));
    CHECK(Access::areFaceNeighbors(
        ChunkCoord{maximum, 0, 0}, ChunkCoord{maximum - 1, 0, 0}));
    CHECK(!Access::areFaceNeighbors(
        ChunkCoord{minimum, 0, 0}, ChunkCoord{maximum, 0, 0}));
    CHECK(!Access::areFaceNeighbors(
        ChunkCoord{maximum, minimum, maximum},
        ChunkCoord{minimum, maximum, minimum}));
}

TEST_CASE(ChunkStreamer_VisibilityTraceClassifiesGeneratedData) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    const ChunkCoord coord{0, 0, 0};
    auto tracer = std::make_shared<ChunkVisibilityTracer>(
        ChunkVisibilityTracer::Config{coord, 2});

    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 0;
    stream.workerThreads = 0;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    streamer.setVisibilityTracer(tracer);

    streamer.update(coord.toWorldCenter());
    streamer.processCompletions();

    const auto records = tracer->snapshot();
    CHECK_EQ(records.size(), static_cast<size_t>(1));
    CHECK_EQ(records.front().origin, ChunkVisibilityOrigin::Generated);
    CHECK(records.front().stage(ChunkVisibilityStage::DataRequest).has_value());
    CHECK(records.front().stage(ChunkVisibilityStage::DataReady).has_value());
}

TEST_CASE(ChunkStreamer_VisibilityTraceBatchedLoadsPreserveOwnReadinessOrder) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    BlockID solid = registerTestBlock(
        registry, "rigel:trace_batched_load_solid");
    const ChunkCoord coord{0, 0, 0};
    const ChunkCoord finalNeighbor{1, 0, 0};
    addLoadedNeighborShell(
        manager,
        coord,
        finalNeighbor,
        generator->config().world.version);
    auto tracer = std::make_shared<ChunkVisibilityTracer>(
        ChunkVisibilityTracer::Config{coord, 2});
    std::unordered_map<ChunkCoord, ChunkLoadRequestId, ChunkCoordHash> requests;
    std::vector<ChunkLoadCompletion> completions;

    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 1;
    stream.unloadDistanceChunks = 1;
    stream.workerThreads = 0;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    streamer.setVisibilityTracer(tracer);
    streamer.setChunkLoader([&](ChunkLoadRequest request) {
        requests[request.coord] = request.requestId;
        return ChunkLoadRequestResult::Queued;
    });
    streamer.setChunkLoadDrain([&](size_t) {
        auto drained = std::move(completions);
        completions.clear();
        return drained;
    });
    streamer.update(coord.toWorldCenter());

    for (const ChunkCoord ready : {finalNeighbor, coord}) {
        Chunk& chunk = manager.getOrCreateChunk(ready);
        chunk.setBlock(0, 0, 0, BlockState{solid}, registry);
        chunk.setWorldGenVersion(generator->config().world.version);
        chunk.setLoadedFromDisk(true);
        chunk.clearPersistDirty();
        chunk.clearDirty();
        completions.push_back({
            ready,
            requests.at(ready),
            ChunkLoadOutcome::Loaded,
            {}});
    }
    streamer.processCompletions();

    const auto pendingOrder =
        Rigel::Voxel::detail::ChunkStreamerTestAccess::pendingLoadGenOrder(
            streamer);
    CHECK_EQ(pendingOrder.size(), static_cast<size_t>(2));
    CHECK_EQ(pendingOrder[0], finalNeighbor);
    CHECK_EQ(pendingOrder[1], coord);
    const auto record = tracer->snapshot().front();
    const auto dataReady = record.stage(ChunkVisibilityStage::DataReady);
    const auto neighborReady =
        record.stage(ChunkVisibilityStage::NeighborReady);
    CHECK(dataReady.has_value());
    CHECK(!neighborReady || *dataReady <= *neighborReady);
    CHECK_EQ(record.origin, ChunkVisibilityOrigin::Persisted);
}

TEST_CASE(ChunkStreamer_VisibilityTraceIncludesDirtyMeshCapacityWait) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    BlockID solid = registerTestBlock(
        registry, "rigel:trace_dirty_capacity_solid");
    const ChunkCoord blockerCoord{0, 0, 0};
    const ChunkCoord tracedCoord{1, 0, 0};
    const std::array<ChunkCoord, 7> desired{
        blockerCoord,
        tracedCoord,
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

    auto clock = std::make_shared<ManualTraceClock>();
    auto tracer = std::make_shared<ChunkVisibilityTracer>(
        ChunkVisibilityTracer::Config{tracedCoord, 2},
        [clock]() { return clock->now(); });
    auto gate = std::make_shared<WorkerGate>();
    std::atomic<size_t> buildsEntered{0};

    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
    WorkerGateRelease releaseOnExit(gate);
    StreamingConfig stream;
    stream.viewDistanceChunks = 1;
    stream.unloadDistanceChunks = 1;
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

    streamer.update(blockerCoord.toWorldCenter());
    streamer.processCompletions();
    streamer.setVisibilityTracer(tracer);

    Chunk& blocker = *manager.getChunk(blockerCoord);
    Chunk& traced = *manager.getChunk(tracedCoord);
    blocker.setBlock(1, 0, 0, BlockState{solid}, registry);
    traced.setBlock(1, 0, 0, BlockState{solid}, registry);
    streamer.update(blockerCoord.toWorldCenter());
    bool workerEntered = gate->waitUntilEntered();
    if (!workerEntered) {
        gate->release();
    }
    CHECK(workerEntered);
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(1));
    CHECK(traced.isDirty());

    clock->advance(std::chrono::milliseconds(25));
    gate->release();
    CHECK(waitForMeshCompletions(streamer, 1));
    streamer.update(blockerCoord.toWorldCenter());
    CHECK(waitForMeshCompletions(streamer, 2));

    const auto records = tracer->snapshot();
    CHECK_EQ(records.size(), static_cast<size_t>(1));
    const auto& record = records.front();
    CHECK_EQ(
        record.outcome,
        ChunkVisibilityOutcome::AcceptedNonemptyGeometry);
    CHECK_EQ(record.kind, ChunkVisibilityLifecycleKind::Remesh);
    CHECK(!record.stage(ChunkVisibilityStage::Desired).has_value());
    CHECK(!record.stage(ChunkVisibilityStage::DataRequest).has_value());
    CHECK(!record.stage(ChunkVisibilityStage::DataReady).has_value());
    const auto neighborReady =
        record.stage(ChunkVisibilityStage::NeighborReady);
    const auto meshEligible =
        record.stage(ChunkVisibilityStage::MeshEligible);
    const auto schedulerWait =
        record.stage(ChunkVisibilityStage::SchedulerWait);
    const auto poolSubmit =
        record.stage(ChunkVisibilityStage::PoolSubmit);
    CHECK(!neighborReady.has_value());
    CHECK(meshEligible.has_value());
    CHECK(schedulerWait.has_value());
    CHECK(poolSubmit.has_value());
    CHECK(*meshEligible <= *schedulerWait);
    CHECK(*schedulerWait <= *poolSubmit);
    const auto durations = record.durations();
    CHECK_EQ(
        record.firstObservedMissingDesiredCardinalNeighborCount,
        std::optional<uint8_t>{0});
    CHECK(durations.schedulerWait.has_value());
    CHECK_EQ(*durations.schedulerWait, std::chrono::milliseconds(25));
}

TEST_CASE(ChunkStreamer_VisibilityTraceStopsDependencyWaitAtFinalNeighborEvent) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    BlockID solid = registerTestBlock(
        registry, "rigel:trace_neighbor_event_solid");
    const ChunkCoord coord{0, 0, 0};
    const ChunkCoord partialNeighbor{0, 1, 0};
    const ChunkCoord finalNeighbor{1, 0, 0};

    Chunk& chunk = manager.getOrCreateChunk(coord);
    chunk.setBlock(0, 0, 0, BlockState{solid}, registry);
    chunk.setWorldGenVersion(generator->config().world.version);
    chunk.setLoadedFromDisk(true);
    const std::array loadedNeighbors{
        ChunkCoord{-1, 0, 0},
        ChunkCoord{0, -1, 0},
        ChunkCoord{0, 0, 1},
        ChunkCoord{0, 0, -1}
    };
    for (const ChunkCoord loadedNeighbor : loadedNeighbors) {
        Chunk& neighbor = manager.getOrCreateChunk(loadedNeighbor);
        neighbor.setWorldGenVersion(generator->config().world.version);
        neighbor.setLoadedFromDisk(true);
        neighbor.clearPersistDirty();
        neighbor.clearDirty();
    }

    auto clock = std::make_shared<ManualTraceClock>();
    auto tracer = std::make_shared<ChunkVisibilityTracer>(
        ChunkVisibilityTracer::Config{coord, 2},
        [clock]() { return clock->now(); });
    std::unordered_map<ChunkCoord, ChunkLoadRequestId, ChunkCoordHash> requests;
    std::vector<ChunkLoadCompletion> completions;

    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 1;
    stream.unloadDistanceChunks = 1;
    stream.workerThreads = 0;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    streamer.setVisibilityTracer(tracer);
    streamer.setChunkLoader([&](ChunkLoadRequest request) {
        requests[request.coord] = request.requestId;
        return ChunkLoadRequestResult::Queued;
    });
    streamer.setChunkLoadDrain([&](size_t) {
        auto drained = std::move(completions);
        completions.clear();
        return drained;
    });

    streamer.update(coord.toWorldCenter());
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(0));
    CHECK(requests.find(partialNeighbor) != requests.end());
    CHECK(requests.find(finalNeighbor) != requests.end());

    auto records = tracer->snapshot();
    CHECK_EQ(records.size(), static_cast<size_t>(1));
    CHECK_EQ(
        records.front().firstObservedMissingDesiredCardinalNeighborCount,
        std::optional<uint8_t>{2});

    clock->advance(std::chrono::milliseconds(5));
    Chunk& partial = manager.getOrCreateChunk(partialNeighbor);
    partial.setWorldGenVersion(generator->config().world.version);
    partial.setLoadedFromDisk(true);
    partial.clearDirty();
    completions.push_back({
        partialNeighbor,
        requests.at(partialNeighbor),
        ChunkLoadOutcome::Loaded,
        {}});
    streamer.processCompletions();

    records = tracer->snapshot();
    CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::
        hasDependencyPendingMesh(streamer, coord));
    CHECK_EQ(
        records.front().firstObservedMissingDesiredCardinalNeighborCount,
        std::optional<uint8_t>{2});
    CHECK(!records.front().stage(ChunkVisibilityStage::NeighborReady).has_value());

    clock->advance(std::chrono::milliseconds(5));
    Chunk& final = manager.getOrCreateChunk(finalNeighbor);
    final.setWorldGenVersion(generator->config().world.version);
    final.setLoadedFromDisk(true);
    final.clearDirty();
    completions.push_back({
        finalNeighbor,
        requests.at(finalNeighbor),
        ChunkLoadOutcome::Loaded,
        {}});
    streamer.processCompletions();

    records = tracer->snapshot();
    CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::
        hasDependencyPendingMesh(streamer, coord));
    CHECK_EQ(
        records.front().firstObservedMissingDesiredCardinalNeighborCount,
        std::optional<uint8_t>{2});
    const auto neighborReady =
        records.front().stage(ChunkVisibilityStage::NeighborReady);
    const auto schedulerWait =
        records.front().stage(ChunkVisibilityStage::SchedulerWait);
    CHECK(neighborReady.has_value());
    CHECK(schedulerWait.has_value());
    CHECK_EQ(*neighborReady, *schedulerWait);
    CHECK(!records.front().stage(ChunkVisibilityStage::PoolSubmit).has_value());

    clock->advance(std::chrono::milliseconds(25));
    streamer.update(coord.toWorldCenter());
    streamer.processCompletions();

    records = tracer->snapshot();
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(1));
    CHECK_EQ(streamer.workMetrics().meshJobsAccepted, static_cast<uint64_t>(1));
    CHECK_EQ(
        records.front().outcome,
        ChunkVisibilityOutcome::AcceptedNonemptyGeometry);
    CHECK(!records.front().stage(ChunkVisibilityStage::DataRequest).has_value());
    CHECK(!records.front().stage(ChunkVisibilityStage::DataReady).has_value());
    CHECK(records.front().meshTask.has_value());
    CHECK_EQ(
        records.front().meshTask->requestId,
        static_cast<uint64_t>(1));
    CHECK_EQ(
        records.front().firstObservedMissingDesiredCardinalNeighborCount,
        std::optional<uint8_t>{2});
    CHECK_EQ(
        records.front().durations().schedulerWait,
        std::chrono::milliseconds(25));
}

TEST_CASE(ChunkStreamer_VisibilityTraceOwnDataReadyIsNotNeighborReady) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    BlockID solid = registerTestBlock(
        registry, "rigel:trace_own_data_ready_solid");
    const ChunkCoord coord{0, 0, 0};

    addLoadedNeighborShell(
        manager,
        coord,
        std::nullopt,
        generator->config().world.version);

    auto clock = std::make_shared<ManualTraceClock>();
    auto tracer = std::make_shared<ChunkVisibilityTracer>(
        ChunkVisibilityTracer::Config{coord, 2},
        [clock]() { return clock->now(); });
    std::optional<ChunkLoadRequest> request;
    std::vector<ChunkLoadCompletion> completions;

    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 0;
    stream.workerThreads = 0;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    streamer.setVisibilityTracer(tracer);
    streamer.setChunkLoader([&](ChunkLoadRequest loadRequest) {
        request = loadRequest;
        return ChunkLoadRequestResult::Queued;
    });
    streamer.setChunkLoadDrain([&](size_t) {
        auto drained = std::move(completions);
        completions.clear();
        return drained;
    });

    streamer.update(coord.toWorldCenter());
    CHECK(request.has_value());
    auto records = tracer->snapshot();
    CHECK_EQ(records.size(), static_cast<size_t>(1));
    CHECK(records.front().stage(ChunkVisibilityStage::DataRequest).has_value());
    CHECK(!records.front().stage(ChunkVisibilityStage::DataReady).has_value());

    clock->advance(std::chrono::milliseconds(10));
    Chunk& chunk = manager.getOrCreateChunk(coord);
    chunk.setBlock(0, 0, 0, BlockState{solid}, registry);
    chunk.setWorldGenVersion(generator->config().world.version);
    chunk.setLoadedFromDisk(true);
    chunk.clearPersistDirty();
    chunk.clearDirty();
    completions.push_back({
        coord,
        request->requestId,
        ChunkLoadOutcome::Loaded,
        {}});
    streamer.processCompletions();

    records = tracer->snapshot();
    CHECK_EQ(records.front().origin, ChunkVisibilityOrigin::Persisted);
    CHECK_EQ(
        records.front().firstObservedMissingDesiredCardinalNeighborCount,
        std::optional<uint8_t>{0});
    CHECK(records.front().stage(ChunkVisibilityStage::DataReady).has_value());
    CHECK(!records.front().stage(ChunkVisibilityStage::NeighborReady).has_value());
    CHECK(records.front().stage(ChunkVisibilityStage::MeshEligible).has_value());
    CHECK(records.front().stage(ChunkVisibilityStage::SchedulerWait).has_value());
    CHECK(!records.front().durations().dependencyWait.has_value());

    clock->advance(std::chrono::milliseconds(25));
    streamer.update(coord.toWorldCenter());
    streamer.processCompletions();
    records = tracer->snapshot();
    CHECK_EQ(
        records.front().outcome,
        ChunkVisibilityOutcome::AcceptedNonemptyGeometry);
    CHECK(records.front().meshTask.has_value());
    CHECK_EQ(
        records.front().durations().schedulerWait,
        std::chrono::milliseconds(25));
}

TEST_CASE(ChunkStreamer_VisibilityTraceClockFailureCannotStrandMeshWork) {
    const std::array failures{
        std::pair{static_cast<size_t>(3), ChunkVisibilityStage::PoolSubmit},
        std::pair{static_cast<size_t>(4), ChunkVisibilityStage::WorkerStart},
        std::pair{static_cast<size_t>(5), ChunkVisibilityStage::WorkerFinish}
    };

    for (const auto& [throwOnRead, missingStage] : failures) {
        ChunkManager manager;
        BlockRegistry registry;
        WorldMeshStore meshStore;
        auto generator = makeGenerator(registry);
        BlockID solid = registerTestBlock(
            registry, "rigel:trace_throwing_clock_solid");
        const ChunkCoord coord{0, 0, 0};

        Chunk& chunk = manager.getOrCreateChunk(coord);
        chunk.setBlock(0, 0, 0, BlockState{solid}, registry);
        chunk.setWorldGenVersion(generator->config().world.version);
        chunk.setLoadedFromDisk(true);

        auto clock = std::make_shared<ThrowingTraceClock>(throwOnRead);
        auto tracer = std::make_shared<ChunkVisibilityTracer>(
            ChunkVisibilityTracer::Config{coord, 1},
            [clock]() { return clock->now(); });
        ChunkStreamer streamer(
            manager, meshStore, registry, nullptr, generator);
        StreamingConfig stream;
        stream.viewDistanceChunks = 0;
        stream.unloadDistanceChunks = 0;
        stream.workerThreads = 2;
        stream.maxResidentChunks = 0;
        streamer.setConfig(stream);
        streamer.setVisibilityTracer(tracer);

        bool exceptionEscaped = false;
        bool completionObserved = false;
        try {
            streamer.update(coord.toWorldCenter());
            completionObserved = waitForMeshCompletions(streamer, 1);
        } catch (...) {
            exceptionEscaped = true;
        }

        CHECK(!exceptionEscaped);
        CHECK(completionObserved);
        CHECK_EQ(
            streamer.workMetrics().meshJobsStarted,
            static_cast<uint64_t>(1));
        CHECK_EQ(
            streamer.workMetrics().meshJobsCompleted,
            static_cast<uint64_t>(1));
        CHECK_EQ(
            streamer.workMetrics().meshJobsAccepted,
            static_cast<uint64_t>(1));
        CHECK_EQ(
            streamer.diagnostics().mesh.inFlight,
            static_cast<size_t>(0));

        const auto records = tracer->snapshot();
        CHECK_EQ(records.size(), static_cast<size_t>(1));
        CHECK_EQ(
            records.front().outcome,
            ChunkVisibilityOutcome::AcceptedNonemptyGeometry);
        CHECK(records.front().meshTask.has_value());
        CHECK(!records.front().stage(missingStage).has_value());
    }
}

TEST_CASE(ChunkStreamer_VisibilityTraceKeepsStaleAndReplacementSeparate) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    BlockID solid = registerTestBlock(
        registry, "rigel:trace_replacement_solid");
    const ChunkCoord coord{0, 0, 0};

    Chunk& chunk = manager.getOrCreateChunk(coord);
    chunk.setBlock(0, 0, 0, BlockState{solid}, registry);
    chunk.setWorldGenVersion(generator->config().world.version);
    chunk.setLoadedFromDisk(true);

    auto tracer = std::make_shared<ChunkVisibilityTracer>(
        ChunkVisibilityTracer::Config{coord, 4});
    auto gate = std::make_shared<WorkerGate>();
    std::atomic<size_t> buildsEntered{0};

    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
    WorkerGateRelease releaseOnExit(gate);
    StreamingConfig stream;
    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 0;
    stream.meshQueueLimit = 1;
    stream.updateBudgetPerFrame = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 2;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    streamer.setVisibilityTracer(tracer);
    Rigel::Voxel::detail::ChunkStreamerTestAccess::setMeshBuildStartCallback(
        streamer,
        [gate, &buildsEntered]() {
            if (buildsEntered.fetch_add(1, std::memory_order_relaxed) == 0) {
                gate->enterAndWait();
            }
        });

    streamer.update(coord.toWorldCenter());
    bool workerEntered = gate->waitUntilEntered();
    if (!workerEntered) {
        gate->release();
    }
    CHECK(workerEntered);

    chunk.setBlock(1, 0, 0, BlockState{solid}, registry);
    streamer.update(coord.toWorldCenter());
    gate->release();
    CHECK(waitForMeshCompletions(streamer, 1));

    streamer.update(coord.toWorldCenter());
    CHECK(waitForMeshCompletions(streamer, 2));

    const auto records = tracer->snapshot();
    CHECK_EQ(records.size(), static_cast<size_t>(2));
    CHECK_EQ(records[0].outcome, ChunkVisibilityOutcome::Stale);
    CHECK_EQ(
        records[1].outcome,
        ChunkVisibilityOutcome::AcceptedNonemptyGeometry);
    CHECK_NE(records[0].key, records[1].key);
    CHECK(records[0].meshTask.has_value());
    CHECK(records[1].meshTask.has_value());
    CHECK_NE(
        records[0].meshTask->requestId,
        records[1].meshTask->requestId);
    CHECK_EQ(
        records[0].meshTask->chunkInstanceId,
        records[1].meshTask->chunkInstanceId);
    CHECK_NE(
        records[0].meshTask->revision,
        records[1].meshTask->revision);
}

TEST_CASE(ChunkStreamer_VisibilityTraceDoesNotHandLateResultToCameraReentry) {
    enum class ResultKind {
        Nonempty,
        EmptyGeometry,
        Failed
    };

    for (const ResultKind resultKind : {
             ResultKind::Nonempty,
             ResultKind::EmptyGeometry,
             ResultKind::Failed}) {
        ChunkManager manager;
        BlockRegistry registry;
        WorldMeshStore meshStore;
        auto generator = makeGenerator(registry);
        BlockID solid = registerTestBlock(
            registry, "rigel:trace_reentry_solid");
        const ChunkCoord coord{0, 0, 0};
        const ChunkCoord away{0, 2, 0};

        Chunk& chunk = manager.getOrCreateChunk(coord);
        if (resultKind == ResultKind::EmptyGeometry) {
            chunk.fill(BlockState{solid}, registry);
            for (int index = 0; index < DirectionCount; ++index) {
                int dx = 0;
                int dy = 0;
                int dz = 0;
                directionOffset(
                    static_cast<Direction>(index), dx, dy, dz);
                Chunk& neighbor = manager.getOrCreateChunk(
                    coord.offset(dx, dy, dz));
                neighbor.fill(BlockState{solid}, registry);
                neighbor.setWorldGenVersion(
                    generator->config().world.version);
                neighbor.setLoadedFromDisk(true);
            }
        } else {
            chunk.setBlock(0, 0, 0, BlockState{solid}, registry);
        }
        chunk.setWorldGenVersion(generator->config().world.version);
        chunk.setLoadedFromDisk(true);

        auto tracer = std::make_shared<ChunkVisibilityTracer>(
            ChunkVisibilityTracer::Config{coord, 4});
        auto gate = std::make_shared<WorkerGate>();

        ChunkStreamer streamer(
            manager, meshStore, registry, nullptr, generator);
        WorkerGateRelease releaseOnExit(gate);
        StreamingConfig stream;
        stream.viewDistanceChunks = 0;
        stream.unloadDistanceChunks = 10;
        stream.meshQueueLimit = 1;
        stream.workerThreads = 2;
        stream.maxResidentChunks = 0;
        streamer.setConfig(stream);
        streamer.markSpawnDiscoveryComplete();
        streamer.setVisibilityTracer(tracer);
        Rigel::Voxel::detail::ChunkStreamerTestAccess::setMeshBuildStartCallback(
            streamer,
            [gate, resultKind]() {
                gate->enterAndWait();
                if (resultKind == ResultKind::Failed) {
                    throw std::runtime_error("injected late mesh failure");
                }
            });

        streamer.update(coord.toWorldCenter());
        const bool workerEntered = gate->waitUntilEntered();
        if (!workerEntered) {
            gate->release();
        }
        CHECK(workerEntered);
        CHECK_EQ(
            streamer.workMetrics().meshJobsStarted,
            static_cast<uint64_t>(1));

        streamer.update(away.toWorldCenter());
        auto records = tracer->snapshot();
        CHECK_EQ(records.size(), static_cast<size_t>(1));
        CHECK_EQ(records.front().outcome, ChunkVisibilityOutcome::CameraLeft);
        CHECK(records.front().meshTask.has_value());
        const auto departedKey = records.front().key;

        streamer.update(coord.toWorldCenter());
        records = tracer->snapshot();
        CHECK_EQ(records.size(), static_cast<size_t>(2));
        CHECK_EQ(records[1].outcome, ChunkVisibilityOutcome::Pending);
        CHECK(!records[1].meshTask.has_value());
        CHECK_NE(records[0].key, records[1].key);
        const auto replacementKey = records[1].key;

        gate->release();
        CHECK(waitForMeshCompletions(streamer, 1));

        records = tracer->snapshot();
        CHECK_EQ(records.size(), static_cast<size_t>(2));
        CHECK_EQ(records[0].outcome, ChunkVisibilityOutcome::CameraLeft);
        CHECK(!records[1].meshTask.has_value());
        CHECK(!records[1]
                   .stage(ChunkVisibilityStage::PoolSubmit)
                   .has_value());
        CHECK(!records[1]
                   .stage(ChunkVisibilityStage::WorkerStart)
                   .has_value());
        CHECK(!records[1]
                   .stage(ChunkVisibilityStage::WorkerFinish)
                   .has_value());
        CHECK_EQ(records[1].outcome, ChunkVisibilityOutcome::Pending);
        CHECK(!records[1]
                   .stage(ChunkVisibilityStage::ResultAccepted)
                   .has_value());
        CHECK_EQ(
            streamer.workMetrics().meshJobsAccepted,
            static_cast<uint64_t>(0));
        CHECK_EQ(
            streamer.workMetrics().meshJobsFailed,
            static_cast<uint64_t>(0));
        CHECK_EQ(
            streamer.workMetrics().meshJobsRejectedStale,
            static_cast<uint64_t>(1));

        bool departedTracePublished = false;
        bool replacementTracePublished = false;
        meshStore.forEach([&](const WorldMeshEntry& entry) {
            if (entry.visibilityTrace) {
                departedTracePublished = departedTracePublished ||
                    entry.visibilityTrace->key == departedKey;
                replacementTracePublished = replacementTracePublished ||
                    entry.visibilityTrace->key == replacementKey;
            }
        });
        CHECK(!departedTracePublished);
        CHECK(!replacementTracePublished);
        CHECK(!meshStore.contains(coord));
        CHECK_EQ(
            std::count_if(
                records.begin(), records.end(),
                [](const ChunkVisibilityTraceRecord& record) {
                    return record.outcome == ChunkVisibilityOutcome::Pending;
                }),
            static_cast<std::ptrdiff_t>(1));

        streamer.update(coord.toWorldCenter());
        CHECK_EQ(
            streamer.workMetrics().meshJobsStarted,
            static_cast<uint64_t>(2));
        CHECK(waitForMeshCompletions(streamer, 2));

        records = tracer->snapshot();
        CHECK_EQ(records.size(), static_cast<size_t>(2));
        CHECK_EQ(records[0].outcome, ChunkVisibilityOutcome::CameraLeft);
        const auto expectedOutcome =
            resultKind == ResultKind::Nonempty
            ? ChunkVisibilityOutcome::AcceptedNonemptyGeometry
            : resultKind == ResultKind::EmptyGeometry
            ? ChunkVisibilityOutcome::AcceptedEmptyGeometry
            : ChunkVisibilityOutcome::Failed;
        CHECK_EQ(records[1].outcome, expectedOutcome);
        CHECK(records[1].meshTask.has_value());
        CHECK_NE(
            records[0].meshTask->requestId,
            records[1].meshTask->requestId);
        CHECK_EQ(
            records[1]
                .stage(ChunkVisibilityStage::ResultAccepted)
                .has_value(),
            resultKind != ResultKind::Failed);
        departedTracePublished = false;
        replacementTracePublished = false;
        meshStore.forEach([&](const WorldMeshEntry& entry) {
            if (entry.visibilityTrace) {
                departedTracePublished = departedTracePublished ||
                    entry.visibilityTrace->key == departedKey;
                replacementTracePublished = replacementTracePublished ||
                    entry.visibilityTrace->key == replacementKey;
            }
        });
        CHECK(!departedTracePublished);
        CHECK_EQ(
            replacementTracePublished,
            resultKind == ResultKind::Nonempty);
        CHECK_EQ(
            meshStore.contains(coord),
            resultKind != ResultKind::Failed);
        CHECK_EQ(
            streamer.workMetrics().meshJobsAccepted,
            resultKind == ResultKind::Failed
                ? static_cast<uint64_t>(0)
                : static_cast<uint64_t>(1));
        CHECK_EQ(
            streamer.workMetrics().meshJobsFailed,
            resultKind == ResultKind::Failed
                ? static_cast<uint64_t>(1)
                : static_cast<uint64_t>(0));
        CHECK_EQ(
            streamer.workMetrics().meshJobsRejectedStale,
            static_cast<uint64_t>(1));

        for (uint32_t update = 0;
             update < StreamingDiagnosticSnapshot::QuiescenceUpdateWindow + 2;
             ++update) {
            streamer.update(coord.toWorldCenter());
            streamer.processCompletions();
            CHECK_EQ(
                streamer.workMetrics().meshJobsStarted,
                static_cast<uint64_t>(2));
            CHECK_EQ(
                streamer.diagnostics().mesh.inFlight,
                static_cast<size_t>(0));
            CHECK_EQ(
                streamer.diagnostics().mesh.pending,
                static_cast<size_t>(0));
        }
        CHECK_EQ(
            streamer.diagnostics().mesh.terminalErrors,
            resultKind == ResultKind::Failed
                ? static_cast<size_t>(1)
                : static_cast<size_t>(0));
        if (resultKind == ResultKind::Failed) {
            CHECK_EQ(
                streamer.diagnostics().state,
                StreamingLifecycleState::Streaming);
        } else {
            CHECK_EQ(
                streamer.diagnostics().state,
                StreamingLifecycleState::Quiescent);
        }
    }
}

TEST_CASE(ChunkStreamer_LateVisibilityResultPreservesStayAwaySchedulerState) {
    enum class ResultKind {
        Nonempty,
        EmptyGeometry,
        Failed
    };

    for (const ResultKind resultKind : {
             ResultKind::Nonempty,
             ResultKind::EmptyGeometry,
             ResultKind::Failed}) {
        ChunkManager manager;
        BlockRegistry registry;
        WorldMeshStore meshStore;
        auto generator = makeGenerator(registry);
        BlockID solid = registerTestBlock(
            registry, "rigel:trace_stay_away_solid");
        const ChunkCoord coord{0, 0, 0};
        const ChunkCoord away{0, 2, 0};

        Chunk& chunk = manager.getOrCreateChunk(coord);
        if (resultKind == ResultKind::EmptyGeometry) {
            chunk.fill(BlockState{solid}, registry);
            for (int index = 0; index < DirectionCount; ++index) {
                int dx = 0;
                int dy = 0;
                int dz = 0;
                directionOffset(
                    static_cast<Direction>(index), dx, dy, dz);
                Chunk& neighbor = manager.getOrCreateChunk(
                    coord.offset(dx, dy, dz));
                neighbor.fill(BlockState{solid}, registry);
                neighbor.setWorldGenVersion(
                    generator->config().world.version);
                neighbor.setLoadedFromDisk(true);
            }
        } else {
            chunk.setBlock(0, 0, 0, BlockState{solid}, registry);
        }
        chunk.setWorldGenVersion(generator->config().world.version);
        chunk.setLoadedFromDisk(true);
        Chunk& awayChunk = manager.getOrCreateChunk(away);
        awayChunk.setWorldGenVersion(generator->config().world.version);
        awayChunk.setLoadedFromDisk(true);
        awayChunk.clearDirty();

        auto tracer = std::make_shared<ChunkVisibilityTracer>(
            ChunkVisibilityTracer::Config{coord, 2});
        auto gate = std::make_shared<WorkerGate>();
        ChunkStreamer streamer(
            manager, meshStore, registry, nullptr, generator);
        WorkerGateRelease releaseOnExit(gate);
        StreamingConfig stream;
        stream.viewDistanceChunks = 0;
        stream.unloadDistanceChunks = 10;
        stream.meshQueueLimit = 1;
        stream.workerThreads = 2;
        stream.maxResidentChunks = 0;
        streamer.setConfig(stream);
        streamer.markSpawnDiscoveryComplete();
        streamer.setVisibilityTracer(tracer);
        Rigel::Voxel::detail::ChunkStreamerTestAccess::setMeshBuildStartCallback(
            streamer,
            [gate, resultKind]() {
                gate->enterAndWait();
                if (resultKind == ResultKind::Failed) {
                    throw std::runtime_error("injected stay-away mesh failure");
                }
            });

        streamer.update(coord.toWorldCenter());
        const bool workerEntered = gate->waitUntilEntered();
        if (!workerEntered) {
            gate->release();
        }
        CHECK(workerEntered);
        streamer.update(away.toWorldCenter());
        CHECK_EQ(
            tracer->snapshot().front().outcome,
            ChunkVisibilityOutcome::CameraLeft);

        gate->release();
        CHECK(waitForMeshCompletions(streamer, 1));

        const auto records = tracer->snapshot();
        CHECK_EQ(records.size(), static_cast<size_t>(1));
        CHECK_EQ(records.front().outcome, ChunkVisibilityOutcome::CameraLeft);
        CHECK(records.front().meshTask.has_value());
        CHECK(records.front()
                  .stage(ChunkVisibilityStage::WorkerFinish)
                  .has_value());
        bool tracePublished = false;
        meshStore.forEach([&](const WorldMeshEntry& entry) {
            tracePublished = tracePublished ||
                (entry.visibilityTrace &&
                 entry.visibilityTrace->key == records.front().key);
        });
        CHECK(!tracePublished);
        CHECK(!meshStore.contains(coord));
        CHECK_EQ(
            streamer.workMetrics().meshJobsAccepted,
            static_cast<uint64_t>(0));
        CHECK_EQ(
            streamer.workMetrics().meshJobsFailed,
            static_cast<uint64_t>(0));
        CHECK_EQ(
            streamer.workMetrics().meshJobsRejectedStale,
            static_cast<uint64_t>(1));

        for (uint32_t update = 0;
             update <= StreamingDiagnosticSnapshot::QuiescenceUpdateWindow;
             ++update) {
            streamer.update(away.toWorldCenter());
            streamer.processCompletions();
        }
        CHECK_EQ(
            streamer.workMetrics().meshJobsStarted,
            static_cast<uint64_t>(1));
        CHECK_EQ(streamer.diagnostics().mesh.pending, static_cast<size_t>(0));
        CHECK_EQ(streamer.diagnostics().mesh.inFlight, static_cast<size_t>(0));
        CHECK_EQ(
            streamer.diagnostics().mesh.terminalErrors,
            static_cast<size_t>(0));
        CHECK_EQ(
            streamer.diagnostics().state,
            StreamingLifecycleState::Quiescent);
    }
}

TEST_CASE(ChunkStreamer_DepartureTraceModesPreserveStaleScheduling) {
    enum class TraceMode : uint8_t {
        Absent,
        Disabled,
        Enabled
    };
    enum class FirstResult : uint8_t {
        Successful,
        Failed
    };
    enum class CameraPath : uint8_t {
        StayAway,
        Reenter
    };
    struct Result {
        std::vector<ChunkCoord> dispatchOrder;
        std::array<uint64_t, 10> counters{};
        std::array<size_t, 9> finalWork{};
        StreamingLifecycleState lifecycle =
            StreamingLifecycleState::Streaming;
        std::optional<ChunkStreamer::DebugState> chunkState;
        bool meshInstalled = false;
        size_t clockReads = 0;
    };

    auto run = [](
        TraceMode mode,
        FirstResult firstResult,
        CameraPath cameraPath) {
        ChunkManager manager;
        BlockRegistry registry;
        WorldMeshStore meshStore;
        auto generator = makeGenerator(registry);
        BlockID solid = registerTestBlock(
            registry, "rigel:departure_trace_parity_solid");
        const ChunkCoord coord{0, 0, 0};
        const ChunkCoord away{0, 2, 0};

        Chunk& chunk = manager.getOrCreateChunk(coord);
        chunk.setBlock(0, 0, 0, BlockState{solid}, registry);
        chunk.setWorldGenVersion(generator->config().world.version);
        chunk.setLoadedFromDisk(true);
        Chunk& awayChunk = manager.getOrCreateChunk(away);
        awayChunk.setWorldGenVersion(generator->config().world.version);
        awayChunk.setLoadedFromDisk(true);
        awayChunk.clearDirty();

        auto clock = std::make_shared<IncrementingTraceClock>();
        std::shared_ptr<ChunkVisibilityTracer> tracer;
        if (mode != TraceMode::Absent) {
            tracer = std::make_shared<ChunkVisibilityTracer>(
                ChunkVisibilityTracer::Config{
                    coord,
                    mode == TraceMode::Enabled
                        ? static_cast<size_t>(2)
                        : static_cast<size_t>(0)},
                [clock]() { return clock->now(); });
        }
        auto gate = std::make_shared<WorkerGate>();
        std::atomic<size_t> buildsEntered{0};
        std::vector<ChunkCoord> dispatchOrder;

        ChunkStreamer streamer(
            manager, meshStore, registry, nullptr, generator);
        WorkerGateRelease releaseOnExit(gate);
        StreamingConfig stream;
        stream.viewDistanceChunks = 0;
        stream.unloadDistanceChunks = 10;
        stream.meshQueueLimit = 1;
        stream.workerThreads = 2;
        stream.maxResidentChunks = 0;
        streamer.setConfig(stream);
        streamer.markSpawnDiscoveryComplete();
        if (tracer) {
            streamer.setVisibilityTracer(tracer);
        }
        Rigel::Voxel::detail::ChunkStreamerTestAccess::setMeshBuildStartCallback(
            streamer,
            [gate, &buildsEntered, firstResult]() {
                const size_t build =
                    buildsEntered.fetch_add(1, std::memory_order_relaxed);
                if (build == 0) {
                    gate->enterAndWait();
                    if (firstResult == FirstResult::Failed) {
                        throw std::runtime_error(
                            "injected stale departure mesh failure");
                    }
                }
            });

        streamer.update(coord.toWorldCenter());
        const bool workerEntered = gate->waitUntilEntered();
        if (!workerEntered) {
            gate->release();
        }
        CHECK(workerEntered);
        CHECK_EQ(
            streamer.workMetrics().meshJobsStarted,
            static_cast<uint64_t>(1));
        const auto initialDispatch =
            Rigel::Voxel::detail::ChunkStreamerTestAccess::
                inFlightMeshDispatchOrder(streamer);
        CHECK_EQ(initialDispatch.size(), static_cast<size_t>(1));
        dispatchOrder.insert(
            dispatchOrder.end(),
            initialDispatch.begin(),
            initialDispatch.end());

        streamer.update(away.toWorldCenter());
        if (cameraPath == CameraPath::Reenter) {
            streamer.update(coord.toWorldCenter());
        }
        if (mode == TraceMode::Enabled) {
            const auto records = tracer->snapshot();
            CHECK_EQ(
                records.size(),
                cameraPath == CameraPath::Reenter
                    ? static_cast<size_t>(2)
                    : static_cast<size_t>(1));
            CHECK_EQ(
                records.front().outcome,
                ChunkVisibilityOutcome::CameraLeft);
            CHECK(records.front().meshTask.has_value());
            if (cameraPath == CameraPath::Reenter) {
                CHECK_EQ(
                    records.back().outcome,
                    ChunkVisibilityOutcome::Pending);
                CHECK(!records.back().meshTask.has_value());
            }
        }

        gate->release();
        CHECK(waitForMeshCompletions(streamer, 1));
        CHECK(!meshStore.contains(coord));
        CHECK_EQ(
            streamer.workMetrics().meshJobsAccepted,
            static_cast<uint64_t>(0));
        CHECK_EQ(
            streamer.workMetrics().meshJobsRejectedStale,
            static_cast<uint64_t>(1));
        CHECK_EQ(
            streamer.workMetrics().meshJobsFailed,
            static_cast<uint64_t>(0));
        CHECK_EQ(
            streamer.diagnostics().mesh.inFlight,
            static_cast<size_t>(0));
        CHECK_EQ(
            streamer.diagnostics().mesh.terminalErrors,
            static_cast<size_t>(0));
        std::vector<ChunkStreamer::DebugChunkState> debugStates;
        streamer.getDebugStates(debugStates, coord, 0);
        CHECK(std::none_of(
            debugStates.begin(),
            debugStates.end(),
            [coord](const ChunkStreamer::DebugChunkState& state) {
                return state.coord == coord &&
                    state.failure == ChunkStreamer::DebugFailure::Mesh;
            }));
        if (mode == TraceMode::Enabled) {
            const auto records = tracer->snapshot();
            CHECK(records.front().meshTask.has_value());
            CHECK_EQ(
                records.front().outcome,
                ChunkVisibilityOutcome::CameraLeft);
        }

        if (cameraPath == CameraPath::Reenter) {
            streamer.update(coord.toWorldCenter());
            CHECK_EQ(
                streamer.workMetrics().meshJobsStarted,
                static_cast<uint64_t>(2));
            const auto replacementDispatch =
                Rigel::Voxel::detail::ChunkStreamerTestAccess::
                    inFlightMeshDispatchOrder(streamer);
            CHECK_EQ(replacementDispatch.size(), static_cast<size_t>(1));
            dispatchOrder.insert(
                dispatchOrder.end(),
                replacementDispatch.begin(),
                replacementDispatch.end());
            CHECK(waitForMeshCompletions(streamer, 2));
            CHECK(meshStore.contains(coord));
            CHECK_EQ(
                streamer.workMetrics().meshJobsAccepted,
                static_cast<uint64_t>(1));
            if (mode == TraceMode::Enabled) {
                const auto records = tracer->snapshot();
                CHECK_EQ(records.size(), static_cast<size_t>(2));
                CHECK_EQ(
                    records.back().outcome,
                    ChunkVisibilityOutcome::AcceptedNonemptyGeometry);
                CHECK(records.back().meshTask.has_value());
                CHECK_NE(
                    records.front().meshTask->requestId,
                    records.back().meshTask->requestId);
            }
        }

        const glm::vec3 finalCamera =
            cameraPath == CameraPath::Reenter
            ? coord.toWorldCenter()
            : away.toWorldCenter();
        for (uint32_t update = 0;
             update < StreamingDiagnosticSnapshot::QuiescenceUpdateWindow + 2;
             ++update) {
            streamer.update(finalCamera);
            streamer.processCompletions();
        }

        Result result;
        CHECK_EQ(
            buildsEntered.load(std::memory_order_relaxed),
            dispatchOrder.size());
        result.dispatchOrder = std::move(dispatchOrder);
        const auto& metrics = streamer.workMetrics();
        result.counters = {
            metrics.generationJobsStarted,
            metrics.chunkLoadRequestsStarted,
            metrics.meshJobsStarted,
            metrics.meshJobsCompleted,
            metrics.meshJobsAccepted,
            metrics.meshJobsRejectedStale,
            metrics.meshJobsFailed,
            metrics.meshInvalidations,
            metrics.meshRequestsCoalesced,
            metrics.schedulerCoordinatesInspected
        };
        const auto& diagnostics = streamer.diagnostics();
        result.finalWork = {
            diagnostics.generation.pending,
            diagnostics.generation.inFlight,
            diagnostics.chunkLoad.pending,
            diagnostics.chunkLoad.inFlight,
            diagnostics.mesh.pending,
            diagnostics.mesh.inFlight,
            diagnostics.mesh.terminalErrors,
            diagnostics.eviction.pending,
            diagnostics.eviction.inFlight
        };
        result.lifecycle = diagnostics.state;
        streamer.getDebugStates(debugStates, coord, 0);
        const auto debugState = std::find_if(
            debugStates.begin(),
            debugStates.end(),
            [coord](const ChunkStreamer::DebugChunkState& state) {
                return state.coord == coord;
            });
        if (debugState != debugStates.end()) {
            result.chunkState = debugState->state;
        }
        result.meshInstalled = meshStore.contains(coord);
        result.clockReads = clock->reads();
        return result;
    };

    std::array<std::optional<Result>, 2> successfulResults;
    for (const FirstResult firstResult : {
             FirstResult::Successful,
             FirstResult::Failed}) {
        for (const CameraPath cameraPath : {
                 CameraPath::StayAway,
                 CameraPath::Reenter}) {
            const auto absent =
                run(TraceMode::Absent, firstResult, cameraPath);
            const auto disabled =
                run(TraceMode::Disabled, firstResult, cameraPath);
            const auto enabled =
                run(TraceMode::Enabled, firstResult, cameraPath);
            const std::vector<ChunkCoord> expectedDispatchOrder(
                cameraPath == CameraPath::Reenter ? 2 : 1,
                ChunkCoord{0, 0, 0});
            CHECK_EQ(absent.dispatchOrder, expectedDispatchOrder);
            CHECK_EQ(absent.dispatchOrder, disabled.dispatchOrder);
            CHECK_EQ(absent.dispatchOrder, enabled.dispatchOrder);
            CHECK_EQ(absent.counters, disabled.counters);
            CHECK_EQ(absent.counters, enabled.counters);
            CHECK_EQ(absent.finalWork, disabled.finalWork);
            CHECK_EQ(absent.finalWork, enabled.finalWork);
            CHECK_EQ(absent.finalWork, (std::array<size_t, 9>{}));
            CHECK_EQ(
                absent.counters[2],
                cameraPath == CameraPath::Reenter
                    ? static_cast<uint64_t>(2)
                    : static_cast<uint64_t>(1));
            CHECK_EQ(absent.counters[3], absent.counters[2]);
            CHECK_EQ(
                absent.counters[4],
                cameraPath == CameraPath::Reenter
                    ? static_cast<uint64_t>(1)
                    : static_cast<uint64_t>(0));
            CHECK_EQ(absent.counters[5], static_cast<uint64_t>(1));
            CHECK_EQ(absent.counters[6], static_cast<uint64_t>(0));
            CHECK_EQ(absent.lifecycle, StreamingLifecycleState::Quiescent);
            CHECK_EQ(disabled.lifecycle, absent.lifecycle);
            CHECK_EQ(enabled.lifecycle, absent.lifecycle);
            CHECK_EQ(absent.chunkState, disabled.chunkState);
            CHECK_EQ(absent.chunkState, enabled.chunkState);
            CHECK_EQ(
                absent.chunkState,
                cameraPath == CameraPath::Reenter
                    ? std::optional<ChunkStreamer::DebugState>(
                          ChunkStreamer::DebugState::AcceptedNonemptyGeometry)
                    : std::nullopt);
            CHECK_EQ(absent.meshInstalled, disabled.meshInstalled);
            CHECK_EQ(absent.meshInstalled, enabled.meshInstalled);
            CHECK_EQ(
                absent.meshInstalled,
                cameraPath == CameraPath::Reenter);
            CHECK_EQ(absent.clockReads, static_cast<size_t>(0));
            CHECK_EQ(disabled.clockReads, static_cast<size_t>(0));
            CHECK(enabled.clockReads > 0);

            const size_t pathIndex = static_cast<size_t>(cameraPath);
            if (firstResult == FirstResult::Successful) {
                successfulResults[pathIndex] = absent;
            } else {
                CHECK(successfulResults[pathIndex].has_value());
                const Result& successful = *successfulResults[pathIndex];
                CHECK_EQ(successful.dispatchOrder, absent.dispatchOrder);
                CHECK_EQ(successful.counters, absent.counters);
                CHECK_EQ(successful.finalWork, absent.finalWork);
                CHECK_EQ(successful.lifecycle, absent.lifecycle);
                CHECK_EQ(successful.chunkState, absent.chunkState);
                CHECK_EQ(successful.meshInstalled, absent.meshInstalled);
            }
        }
    }
}

TEST_CASE(ChunkStreamer_LateResultDoesNotAdoptReplacementTracerLifecycle) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    BlockID solid = registerTestBlock(
        registry, "rigel:trace_replacement_inflight_solid");
    const ChunkCoord coord{0, 0, 0};
    const ChunkCoord away{0, 2, 0};
    Chunk& chunk = manager.getOrCreateChunk(coord);
    chunk.setBlock(0, 0, 0, BlockState{solid}, registry);
    chunk.setWorldGenVersion(generator->config().world.version);
    chunk.setLoadedFromDisk(true);

    auto originalTracer = std::make_shared<ChunkVisibilityTracer>(
        ChunkVisibilityTracer::Config{coord, 2});
    auto replacementTracer = std::make_shared<ChunkVisibilityTracer>(
        ChunkVisibilityTracer::Config{coord, 2});
    auto gate = std::make_shared<WorkerGate>();
    ChunkStreamer streamer(
        manager, meshStore, registry, nullptr, generator);
    WorkerGateRelease releaseOnExit(gate);
    StreamingConfig stream;
    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 10;
    stream.meshQueueLimit = 1;
    stream.workerThreads = 2;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    streamer.setVisibilityTracer(originalTracer);
    Rigel::Voxel::detail::ChunkStreamerTestAccess::setMeshBuildStartCallback(
        streamer,
        [gate]() { gate->enterAndWait(); });

    streamer.update(coord.toWorldCenter());
    const bool workerEntered = gate->waitUntilEntered();
    if (!workerEntered) {
        gate->release();
    }
    CHECK(workerEntered);
    const auto originalKey = originalTracer->snapshot().front().key;

    streamer.setVisibilityTracer(replacementTracer);
    CHECK_EQ(
        originalTracer->snapshot().front().outcome,
        ChunkVisibilityOutcome::TracerReplaced);
    streamer.update(away.toWorldCenter());
    streamer.update(coord.toWorldCenter());
    auto replacementRecords = replacementTracer->snapshot();
    CHECK_EQ(replacementRecords.size(), static_cast<size_t>(1));
    CHECK_EQ(
        replacementRecords.front().outcome,
        ChunkVisibilityOutcome::Pending);
    CHECK(!replacementRecords.front().meshTask.has_value());
    const auto replacementKey = replacementRecords.front().key;

    gate->release();
    CHECK(waitForMeshCompletions(streamer, 1));

    CHECK_EQ(
        originalTracer->snapshot().front().outcome,
        ChunkVisibilityOutcome::TracerReplaced);
    replacementRecords = replacementTracer->snapshot();
    CHECK_EQ(replacementRecords.size(), static_cast<size_t>(1));
    CHECK_EQ(
        replacementRecords.front().outcome,
        ChunkVisibilityOutcome::Pending);
    CHECK(!replacementRecords.front().meshTask.has_value());
    CHECK(!replacementRecords.front()
               .stage(ChunkVisibilityStage::ResultAccepted)
               .has_value());
    bool originalTracePublished = false;
    bool replacementTracePublished = false;
    meshStore.forEach([&](const WorldMeshEntry& entry) {
        if (entry.visibilityTrace) {
            originalTracePublished = originalTracePublished ||
                entry.visibilityTrace->key == originalKey;
            replacementTracePublished = replacementTracePublished ||
                entry.visibilityTrace->key == replacementKey;
        }
    });
    CHECK(!originalTracePublished);
    CHECK(!replacementTracePublished);
    CHECK(!meshStore.contains(coord));
    CHECK_EQ(
        streamer.workMetrics().meshJobsStarted,
        static_cast<uint64_t>(1));
    CHECK_EQ(
        streamer.workMetrics().meshJobsAccepted,
        static_cast<uint64_t>(0));
    CHECK_EQ(
        streamer.workMetrics().meshJobsRejectedStale,
        static_cast<uint64_t>(1));

    streamer.update(coord.toWorldCenter());
    CHECK_EQ(
        streamer.workMetrics().meshJobsStarted,
        static_cast<uint64_t>(2));
    CHECK(waitForMeshCompletions(streamer, 2));

    replacementRecords = replacementTracer->snapshot();
    CHECK_EQ(replacementRecords.size(), static_cast<size_t>(1));
    CHECK_EQ(
        replacementRecords.front().outcome,
        ChunkVisibilityOutcome::AcceptedNonemptyGeometry);
    CHECK(replacementRecords.front().meshTask.has_value());
    CHECK_NE(
        originalTracer->snapshot().front().meshTask->requestId,
        replacementRecords.front().meshTask->requestId);
    originalTracePublished = false;
    replacementTracePublished = false;
    meshStore.forEach([&](const WorldMeshEntry& entry) {
        if (entry.visibilityTrace) {
            originalTracePublished = originalTracePublished ||
                entry.visibilityTrace->key == originalKey;
            replacementTracePublished = replacementTracePublished ||
                entry.visibilityTrace->key == replacementKey;
        }
    });
    CHECK(!originalTracePublished);
    CHECK(replacementTracePublished);
    CHECK(meshStore.contains(coord));
    CHECK_EQ(
        streamer.workMetrics().meshJobsAccepted,
        static_cast<uint64_t>(1));
    CHECK_EQ(
        streamer.workMetrics().meshJobsRejectedStale,
        static_cast<uint64_t>(1));
}

TEST_CASE(ChunkStreamer_VisibilityTracerReplacementDoesNotSynthesizeStages) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    BlockID solid = registerTestBlock(
        registry, "rigel:trace_tracer_replacement_solid");
    const ChunkCoord coord{0, 0, 0};
    const ChunkCoord finalNeighbor{1, 0, 0};

    Chunk& chunk = manager.getOrCreateChunk(coord);
    chunk.setBlock(0, 0, 0, BlockState{solid}, registry);
    chunk.setWorldGenVersion(generator->config().world.version);
    chunk.setLoadedFromDisk(true);
    addLoadedNeighborShell(
        manager,
        coord,
        finalNeighbor,
        generator->config().world.version);

    auto originalTracer = std::make_shared<ChunkVisibilityTracer>(
        ChunkVisibilityTracer::Config{coord, 2});
    auto replacementClock = std::make_shared<IncrementingTraceClock>();
    auto replacementTracer = std::make_shared<ChunkVisibilityTracer>(
        ChunkVisibilityTracer::Config{coord, 2},
        [replacementClock]() { return replacementClock->now(); });
    std::unordered_map<ChunkCoord, ChunkLoadRequestId, ChunkCoordHash> requests;
    std::vector<ChunkLoadCompletion> completions;

    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 1;
    stream.unloadDistanceChunks = 1;
    stream.workerThreads = 0;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    streamer.setVisibilityTracer(originalTracer);
    streamer.setChunkLoader([&](ChunkLoadRequest request) {
        requests[request.coord] = request.requestId;
        return ChunkLoadRequestResult::Queued;
    });
    streamer.setChunkLoadDrain([&](size_t) {
        auto drained = std::move(completions);
        completions.clear();
        return drained;
    });

    streamer.update(coord.toWorldCenter());
    const auto beforeReplacement = streamer.workMetrics();
    streamer.setVisibilityTracer(replacementTracer);

    const auto originalRecords = originalTracer->snapshot();
    CHECK_EQ(originalRecords.size(), static_cast<size_t>(1));
    CHECK_EQ(
        originalRecords.front().outcome,
        ChunkVisibilityOutcome::TracerReplaced);
    CHECK(replacementTracer->snapshot().empty());
    CHECK_EQ(replacementClock->reads(), static_cast<size_t>(0));
    CHECK_EQ(
        streamer.workMetrics().schedulerCoordinatesInspected,
        beforeReplacement.schedulerCoordinatesInspected);

    Chunk& neighbor = manager.getOrCreateChunk(finalNeighbor);
    neighbor.setWorldGenVersion(generator->config().world.version);
    neighbor.setLoadedFromDisk(true);
    neighbor.clearDirty();
    completions.push_back({
        finalNeighbor,
        requests.at(finalNeighbor),
        ChunkLoadOutcome::Loaded,
        {}});
    streamer.processCompletions();
    auto replacementRecords = replacementTracer->snapshot();
    CHECK_EQ(replacementRecords.size(), static_cast<size_t>(1));
    CHECK_EQ(
        replacementRecords.front().outcome,
        ChunkVisibilityOutcome::Pending);
    CHECK(replacementRecords.front()
              .stage(ChunkVisibilityStage::NeighborReady)
              .has_value());
    CHECK(replacementRecords.front()
              .stage(ChunkVisibilityStage::SchedulerWait)
              .has_value());

    streamer.update(coord.toWorldCenter());
    streamer.processCompletions();
    replacementRecords = replacementTracer->snapshot();
    CHECK_EQ(replacementRecords.size(), static_cast<size_t>(1));
    CHECK_EQ(
        replacementRecords.front().outcome,
        ChunkVisibilityOutcome::AcceptedNonemptyGeometry);
    CHECK(replacementRecords.front().meshTask.has_value());
    CHECK(!replacementRecords.front()
               .stage(ChunkVisibilityStage::Desired)
               .has_value());
    CHECK(replacementRecords.front()
              .stage(ChunkVisibilityStage::NeighborReady)
              .has_value());
    CHECK(replacementRecords.front()
              .stage(ChunkVisibilityStage::SchedulerWait)
              .has_value());
}

TEST_CASE(ChunkStreamer_VisibilityTraceStartsAtDispatchAfterLateInstallation) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    BlockID solid = registerTestBlock(
        registry, "rigel:trace_late_install_solid");
    const ChunkCoord blockerCoord{0, 0, 0};
    const ChunkCoord tracedCoord{1, 0, 0};
    const std::array desired{
        blockerCoord,
        tracedCoord,
        ChunkCoord{-1, 0, 0},
        ChunkCoord{0, 1, 0},
        ChunkCoord{0, -1, 0},
        ChunkCoord{0, 0, 1},
        ChunkCoord{0, 0, -1}
    };
    for (const ChunkCoord coord : desired) {
        Chunk& chunk = manager.getOrCreateChunk(coord);
        chunk.setBlock(0, 0, 0, BlockState{solid}, registry);
        chunk.setWorldGenVersion(generator->config().world.version);
        chunk.setLoadedFromDisk(true);
    }
    auto gate = std::make_shared<WorkerGate>();
    auto tracer = std::make_shared<ChunkVisibilityTracer>(
        ChunkVisibilityTracer::Config{tracedCoord, 2});

    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
    WorkerGateRelease releaseOnExit(gate);
    StreamingConfig stream;
    stream.viewDistanceChunks = 1;
    stream.unloadDistanceChunks = 1;
    stream.meshQueueLimit = 1;
    stream.workerThreads = 2;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    Rigel::Voxel::detail::ChunkStreamerTestAccess::setMeshBuildStartCallback(
        streamer,
        [gate]() { gate->enterAndWait(); });

    streamer.update(blockerCoord.toWorldCenter());
    bool workerEntered = gate->waitUntilEntered();
    if (!workerEntered) {
        gate->release();
    }
    CHECK(workerEntered);
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(1));

    streamer.setVisibilityTracer(tracer);
    streamer.update(blockerCoord.toWorldCenter());
    CHECK(tracer->snapshot().empty());

    gate->release();
    CHECK(waitForMeshCompletions(streamer, 1));
    CHECK(tracer->snapshot().empty());
    uint64_t expectedCompletions = 2;
    while (tracer->snapshot().empty() &&
           expectedCompletions <= desired.size()) {
        streamer.update(blockerCoord.toWorldCenter());
        CHECK(waitForMeshCompletions(streamer, expectedCompletions));
        ++expectedCompletions;
    }

    const auto records = tracer->snapshot();
    CHECK_EQ(records.size(), static_cast<size_t>(1));
    CHECK(records.front().meshTask.has_value());
    CHECK_EQ(
        records.front().origin,
        ChunkVisibilityOrigin::ResidentLeftCensored);
    CHECK(!records.front().stage(ChunkVisibilityStage::Desired).has_value());
    CHECK(!records.front()
               .stage(ChunkVisibilityStage::MeshEligible)
               .has_value());
    CHECK(!records.front()
               .stage(ChunkVisibilityStage::SchedulerWait)
               .has_value());
    CHECK(records.front().stage(ChunkVisibilityStage::PoolSubmit).has_value());
}

TEST_CASE(ChunkStreamer_VisibilityTraceSeparatesFringeRemeshAndCameraReentry) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    BlockID solid = registerTestBlock(
        registry, "rigel:trace_fringe_reentry_solid");
    const ChunkCoord coord{0, 0, 0};
    const ChunkCoord away{0, 2, 0};
    Chunk& chunk = manager.getOrCreateChunk(coord);
    chunk.setBlock(0, 0, 0, BlockState{solid}, registry);
    chunk.setWorldGenVersion(generator->config().world.version);
    chunk.setLoadedFromDisk(true);
    chunk.clearDirty();
    ChunkMesh cachedMesh;
    cachedMesh.vertices.push_back(VoxelVertex{});
    cachedMesh.indices.push_back(0);
    meshStore.set(coord, std::move(cachedMesh));

    auto gate = std::make_shared<WorkerGate>();
    auto tracer = std::make_shared<ChunkVisibilityTracer>(
        ChunkVisibilityTracer::Config{coord, 4});
    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
    WorkerGateRelease releaseOnExit(gate);
    StreamingConfig stream;
    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 10;
    stream.meshQueueLimit = 1;
    stream.workerThreads = 2;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    streamer.update(coord.toWorldCenter());
    streamer.update(away.toWorldCenter());
    streamer.setVisibilityTracer(tracer);
    Rigel::Voxel::detail::ChunkStreamerTestAccess::setMeshBuildStartCallback(
        streamer,
        [gate]() { gate->enterAndWait(); });

    chunk.setBlock(1, 0, 0, BlockState{solid}, registry);
    streamer.update(away.toWorldCenter());
    bool workerEntered = gate->waitUntilEntered();
    if (!workerEntered) {
        gate->release();
    }
    CHECK(workerEntered);

    streamer.update(coord.toWorldCenter());
    auto records = tracer->snapshot();
    CHECK_EQ(records.size(), static_cast<size_t>(2));
    CHECK_EQ(records[0].kind, ChunkVisibilityLifecycleKind::Remesh);
    CHECK_EQ(records[0].origin, ChunkVisibilityOrigin::Remesh);
    CHECK(records[0].meshTask.has_value());
    CHECK_EQ(records[0].outcome, ChunkVisibilityOutcome::Pending);
    CHECK_EQ(records[1].kind, ChunkVisibilityLifecycleKind::CameraDemand);
    CHECK_EQ(
        records[1].origin,
        ChunkVisibilityOrigin::ResidentLeftCensored);
    CHECK_EQ(
        records[1].outcome,
        ChunkVisibilityOutcome::CachedNonemptyGeometry);
    CHECK(!records[1].meshTask.has_value());

    gate->release();
    CHECK(waitForMeshCompletions(streamer, 1));
    records = tracer->snapshot();
    CHECK_EQ(
        records[0].outcome,
        ChunkVisibilityOutcome::AcceptedNonemptyGeometry);
    CHECK_EQ(
        records[1].drawOutcome,
        ChunkVisibilityDrawOutcome::MeshReplacedBeforeDraw);
}

TEST_CASE(ChunkStreamer_VisibilityTraceSeparatesBlockedRemeshAndCachedReentry) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    BlockID solid = registerTestBlock(
        registry, "rigel:trace_blocked_reentry_solid");
    const ChunkCoord tracedCoord{0, 0, 0};
    const ChunkCoord blockerCoord{0, 2, 0};
    addLoadedNeighborShell(
        manager,
        tracedCoord,
        std::nullopt,
        generator->config().world.version);
    addLoadedNeighborShell(
        manager,
        blockerCoord,
        std::nullopt,
        generator->config().world.version);

    for (const ChunkCoord coord : {tracedCoord, blockerCoord}) {
        Chunk& chunk = manager.getOrCreateChunk(coord);
        chunk.setBlock(0, 0, 0, BlockState{solid}, registry);
        chunk.setWorldGenVersion(generator->config().world.version);
        chunk.setLoadedFromDisk(true);
        chunk.clearPersistDirty();
        chunk.clearDirty();
        ChunkMesh cachedMesh;
        cachedMesh.vertices.push_back(VoxelVertex{});
        cachedMesh.indices.push_back(0);
        meshStore.set(coord, std::move(cachedMesh));
    }

    auto tracer = std::make_shared<ChunkVisibilityTracer>(
        ChunkVisibilityTracer::Config{tracedCoord, 4});
    auto gate = std::make_shared<WorkerGate>();
    ChunkStreamer streamer(
        manager, meshStore, registry, nullptr, generator);
    WorkerGateRelease releaseOnExit(gate);
    StreamingConfig stream;
    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 10;
    stream.meshQueueLimit = 1;
    stream.updateBudgetPerFrame = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 2;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    streamer.markSpawnDiscoveryComplete();

    streamer.update(tracedCoord.toWorldCenter());
    streamer.processCompletions();
    streamer.update(blockerCoord.toWorldCenter());
    streamer.processCompletions();
    streamer.setVisibilityTracer(tracer);
    Rigel::Voxel::detail::ChunkStreamerTestAccess::setMeshBuildStartCallback(
        streamer,
        [gate]() { gate->enterAndWait(); });

    Chunk& traced = *manager.getChunk(tracedCoord);
    Chunk& blocker = *manager.getChunk(blockerCoord);
    traced.setBlock(1, 0, 0, BlockState{solid}, registry);
    blocker.setBlock(1, 0, 0, BlockState{solid}, registry);
    streamer.update(blockerCoord.toWorldCenter());
    bool workerEntered = gate->waitUntilEntered();
    if (!workerEntered) {
        gate->release();
    }
    CHECK(workerEntered);
    CHECK_EQ(
        streamer.workMetrics().meshJobsStarted,
        static_cast<uint64_t>(1));
    CHECK_EQ(
        Rigel::Voxel::detail::ChunkStreamerTestAccess::inFlightMeshDirty(
            streamer),
        static_cast<size_t>(1));
    CHECK_EQ(streamer.diagnostics().mesh.pending, static_cast<size_t>(1));
    CHECK_EQ(streamer.diagnostics().mesh.inFlight, static_cast<size_t>(1));

    auto records = tracer->snapshot();
    CHECK_EQ(records.size(), static_cast<size_t>(1));
    CHECK_EQ(records.front().kind, ChunkVisibilityLifecycleKind::Remesh);
    CHECK_EQ(records.front().origin, ChunkVisibilityOrigin::Remesh);
    CHECK_EQ(records.front().outcome, ChunkVisibilityOutcome::Pending);
    CHECK(!records.front().meshTask.has_value());
    const auto remeshKey = records.front().key;

    streamer.update(tracedCoord.toWorldCenter());
    records = tracer->snapshot();
    CHECK_EQ(records.size(), static_cast<size_t>(2));
    CHECK_EQ(records[0].key, remeshKey);
    CHECK_EQ(records[0].outcome, ChunkVisibilityOutcome::Pending);
    CHECK(!records[0].meshTask.has_value());
    CHECK_EQ(records[1].kind, ChunkVisibilityLifecycleKind::CameraDemand);
    CHECK_EQ(
        records[1].origin,
        ChunkVisibilityOrigin::ResidentLeftCensored);
    CHECK_EQ(
        records[1].outcome,
        ChunkVisibilityOutcome::CachedNonemptyGeometry);
    CHECK(!records[1].meshTask.has_value());
    CHECK_NE(records[0].key, records[1].key);
    CHECK_EQ(
        streamer.workMetrics().meshJobsStarted,
        static_cast<uint64_t>(1));
    CHECK_EQ(streamer.diagnostics().mesh.pending, static_cast<size_t>(1));
    CHECK_EQ(streamer.diagnostics().mesh.inFlight, static_cast<size_t>(1));

    std::optional<ChunkVisibilityLifecycleKey> publishedTrace;
    meshStore.forEach([&](const WorldMeshEntry& entry) {
        if (entry.coord == tracedCoord && entry.visibilityTrace) {
            publishedTrace = entry.visibilityTrace->key;
        }
    });
    CHECK(publishedTrace.has_value());
    CHECK_EQ(*publishedTrace, records[1].key);
    tracer->observeDraw(*publishedTrace);
    meshStore.finishVisibilityDraw(*publishedTrace);
    records = tracer->snapshot();
    CHECK_EQ(
        records[1].drawOutcome,
        ChunkVisibilityDrawOutcome::Drawn);
    CHECK(records[1].stage(ChunkVisibilityStage::FirstDraw).has_value());

    gate->release();
    CHECK(waitForMeshCompletions(streamer, 1));
    streamer.update(tracedCoord.toWorldCenter());
    CHECK_EQ(
        streamer.workMetrics().meshJobsStarted,
        static_cast<uint64_t>(2));
    records = tracer->snapshot();
    CHECK(records[0].meshTask.has_value());
    CHECK_EQ(records[0].outcome, ChunkVisibilityOutcome::Pending);
    CHECK_EQ(records[1].key, *publishedTrace);

    CHECK(waitForMeshCompletions(streamer, 2));
    records = tracer->snapshot();
    CHECK_EQ(records.size(), static_cast<size_t>(2));
    CHECK_EQ(records[0].key, remeshKey);
    CHECK_EQ(
        records[0].outcome,
        ChunkVisibilityOutcome::AcceptedNonemptyGeometry);
    CHECK(records[0].meshTask.has_value());
    CHECK_NE(records[0].meshTask->requestId, static_cast<uint64_t>(1));
    CHECK_EQ(
        records[1].outcome,
        ChunkVisibilityOutcome::CachedNonemptyGeometry);
    CHECK_EQ(
        records[1].drawOutcome,
        ChunkVisibilityDrawOutcome::Drawn);

    publishedTrace.reset();
    meshStore.forEach([&](const WorldMeshEntry& entry) {
        if (entry.coord == tracedCoord && entry.visibilityTrace) {
            publishedTrace = entry.visibilityTrace->key;
        }
    });
    CHECK(publishedTrace.has_value());
    CHECK_EQ(*publishedTrace, remeshKey);
    tracer->observeDraw(*publishedTrace);
    meshStore.finishVisibilityDraw(*publishedTrace);
    records = tracer->snapshot();
    CHECK_EQ(
        records[0].drawOutcome,
        ChunkVisibilityDrawOutcome::Drawn);
    CHECK(records[0].stage(ChunkVisibilityStage::FirstDraw).has_value());

    const auto& metrics = streamer.workMetrics();
    CHECK_EQ(metrics.meshJobsStarted, static_cast<uint64_t>(2));
    CHECK_EQ(metrics.meshJobsCompleted, static_cast<uint64_t>(2));
    CHECK_EQ(metrics.meshJobsAccepted, static_cast<uint64_t>(2));
    CHECK_EQ(metrics.meshJobsRejectedStale, static_cast<uint64_t>(0));
    CHECK_EQ(metrics.meshJobsFailed, static_cast<uint64_t>(0));
    CHECK_EQ(metrics.meshInvalidations, static_cast<uint64_t>(2));
    CHECK_EQ(streamer.diagnostics().mesh.pending, static_cast<size_t>(0));
    CHECK_EQ(streamer.diagnostics().mesh.inFlight, static_cast<size_t>(0));

    for (uint32_t stable = 1;
         stable <= StreamingDiagnosticSnapshot::QuiescenceUpdateWindow;
         ++stable) {
        streamer.update(tracedCoord.toWorldCenter());
        streamer.processCompletions();
        CHECK_EQ(streamer.workMetrics().meshJobsStarted,
                 static_cast<uint64_t>(2));
        CHECK_EQ(streamer.diagnostics().stableUpdates, stable);
    }
    CHECK_EQ(
        streamer.diagnostics().state,
        StreamingLifecycleState::Quiescent);
}

TEST_CASE(ChunkStreamer_VisibilityTraceTeardownStalesPendingAndLateResults) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    BlockID solid = registerTestBlock(
        registry, "rigel:trace_teardown_solid");
    const ChunkCoord coord{0, 0, 0};

    Chunk& chunk = manager.getOrCreateChunk(coord);
    chunk.setBlock(0, 0, 0, BlockState{solid}, registry);
    chunk.setWorldGenVersion(generator->config().world.version);
    chunk.setLoadedFromDisk(true);

    auto tracer = std::make_shared<ChunkVisibilityTracer>(
        ChunkVisibilityTracer::Config{coord, 4});
    auto gate = std::make_shared<WorkerGate>();

    {
        ChunkStreamer streamer(
            manager, meshStore, registry, nullptr, generator);
        WorkerGateRelease releaseOnExit(gate);
        StreamingConfig stream;
        stream.viewDistanceChunks = 0;
        stream.unloadDistanceChunks = 0;
        stream.meshQueueLimit = 1;
        stream.updateBudgetPerFrame = 0;
        stream.applyBudgetPerFrame = 0;
        stream.workerThreads = 2;
        stream.maxResidentChunks = 0;
        streamer.setConfig(stream);
        streamer.setVisibilityTracer(tracer);
        Rigel::Voxel::detail::ChunkStreamerTestAccess::setMeshBuildStartCallback(
            streamer,
            [gate]() { gate->enterAndWait(); });

        streamer.update(coord.toWorldCenter());
        bool workerEntered = gate->waitUntilEntered();
        if (!workerEntered) {
            gate->release();
        }
        CHECK(workerEntered);

        chunk.setBlock(1, 0, 0, BlockState{solid}, registry);
        streamer.update(coord.toWorldCenter());
        Rigel::Voxel::detail::ChunkStreamerTestAccess::reset(streamer);

        auto records = tracer->snapshot();
        CHECK_EQ(records.size(), static_cast<size_t>(2));
        CHECK_EQ(records[0].outcome, ChunkVisibilityOutcome::Reset);
        CHECK_EQ(records[1].outcome, ChunkVisibilityOutcome::Reset);
        CHECK_NE(
            records[0].key,
            records[1].key);

        gate->release();
        CHECK(waitForPendingMeshCompletion(streamer));
    }

    const auto records = tracer->snapshot();
    CHECK_EQ(records.size(), static_cast<size_t>(2));
    CHECK_EQ(records[0].outcome, ChunkVisibilityOutcome::Reset);
    CHECK_EQ(records[1].outcome, ChunkVisibilityOutcome::Reset);
    CHECK(records[0].stage(ChunkVisibilityStage::WorkerFinish).has_value());
    CHECK(!records[0]
               .stage(ChunkVisibilityStage::ResultAccepted)
               .has_value());
    CHECK(!records[1]
               .stage(ChunkVisibilityStage::PoolSubmit)
               .has_value());
}

TEST_CASE(ChunkStreamer_VisibilityTraceCompletesOnStreamerDestruction) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    BlockID solid = registerTestBlock(
        registry, "rigel:trace_destruction_solid");
    const ChunkCoord coord{0, 0, 0};
    const ChunkCoord missingNeighbor{1, 0, 0};

    Chunk& chunk = manager.getOrCreateChunk(coord);
    chunk.setBlock(0, 0, 0, BlockState{solid}, registry);
    chunk.setWorldGenVersion(generator->config().world.version);
    chunk.setLoadedFromDisk(true);
    addLoadedNeighborShell(
        manager,
        coord,
        missingNeighbor,
        generator->config().world.version);
    auto tracer = std::make_shared<ChunkVisibilityTracer>(
        ChunkVisibilityTracer::Config{coord, 1});

    {
        ChunkStreamer streamer(
            manager, meshStore, registry, nullptr, generator);
        StreamingConfig stream;
        stream.viewDistanceChunks = 1;
        stream.unloadDistanceChunks = 1;
        stream.workerThreads = 0;
        stream.maxResidentChunks = 0;
        streamer.setConfig(stream);
        streamer.setVisibilityTracer(tracer);
        streamer.setChunkLoader([](ChunkLoadRequest) {
            return ChunkLoadRequestResult::Queued;
        });
        streamer.update(coord.toWorldCenter());

        const auto records = tracer->snapshot();
        CHECK_EQ(records.size(), static_cast<size_t>(1));
        CHECK_EQ(records.front().outcome, ChunkVisibilityOutcome::Pending);
        CHECK(!records.front().meshTask.has_value());
    }

    const auto records = tracer->snapshot();
    CHECK_EQ(records.size(), static_cast<size_t>(1));
    CHECK_EQ(
        records.front().outcome,
        ChunkVisibilityOutcome::StreamerDestroyed);
}

TEST_CASE(ChunkStreamer_VisibilityTracerIsReusableByReplacementStreamer) {
    BlockRegistry registry;
    auto generator = makeGenerator(registry);
    BlockID solid = registerTestBlock(
        registry, "rigel:trace_reused_streamer_solid");
    const ChunkCoord coord{0, 0, 0};
    auto tracer = std::make_shared<ChunkVisibilityTracer>(
        ChunkVisibilityTracer::Config{coord, 4});

    for (size_t lifetime = 0; lifetime < 2; ++lifetime) {
        ChunkManager manager;
        WorldMeshStore meshStore;
        Chunk& chunk = manager.getOrCreateChunk(coord);
        chunk.setBlock(0, 0, 0, BlockState{solid}, registry);
        chunk.setWorldGenVersion(generator->config().world.version);
        chunk.setLoadedFromDisk(true);

        ChunkStreamer streamer(
            manager, meshStore, registry, nullptr, generator);
        StreamingConfig stream;
        stream.viewDistanceChunks = 0;
        stream.unloadDistanceChunks = 0;
        stream.workerThreads = 0;
        stream.maxResidentChunks = 0;
        streamer.setConfig(stream);
        streamer.setVisibilityTracer(tracer);
        streamer.update(coord.toWorldCenter());
        streamer.processCompletions();
    }

    const auto records = tracer->snapshot();
    CHECK_EQ(records.size(), static_cast<size_t>(2));
    CHECK_EQ(
        records[0].outcome,
        ChunkVisibilityOutcome::AcceptedNonemptyGeometry);
    CHECK_EQ(
        records[1].outcome,
        ChunkVisibilityOutcome::AcceptedNonemptyGeometry);
    CHECK_NE(records[0].key, records[1].key);
    CHECK(records[0].meshTask.has_value());
    CHECK(records[1].meshTask.has_value());
    CHECK_NE(
        *records[0].meshTask,
        *records[1].meshTask);
}

TEST_CASE(ChunkStreamer_VisibilityTraceCompletesOnGeneratorReplacement) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    BlockID solid = registerTestBlock(
        registry, "rigel:trace_generator_replacement_solid");
    const ChunkCoord coord{0, 0, 0};
    const ChunkCoord missingNeighbor{1, 0, 0};

    Chunk& chunk = manager.getOrCreateChunk(coord);
    chunk.setBlock(0, 0, 0, BlockState{solid}, registry);
    chunk.setWorldGenVersion(generator->config().world.version);
    chunk.setLoadedFromDisk(true);
    addLoadedNeighborShell(
        manager,
        coord,
        missingNeighbor,
        generator->config().world.version);
    auto tracer = std::make_shared<ChunkVisibilityTracer>(
        ChunkVisibilityTracer::Config{coord, 1});

    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 1;
    stream.unloadDistanceChunks = 1;
    stream.workerThreads = 0;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    streamer.setVisibilityTracer(tracer);
    streamer.setChunkLoader([](ChunkLoadRequest) {
        return ChunkLoadRequestResult::Queued;
    });
    streamer.update(coord.toWorldCenter());

    auto replacement =
        std::make_shared<WorldGenerator>(registry, generator->config());
    streamer.setGenerator(replacement);

    const auto records = tracer->snapshot();
    CHECK_EQ(records.size(), static_cast<size_t>(1));
    CHECK_EQ(
        records.front().outcome,
        ChunkVisibilityOutcome::GeneratorReplaced);
    CHECK(!records.front().meshTask.has_value());
}

TEST_CASE(ChunkStreamer_VisibilityTraceTracksCachedCameraLifecycle) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    BlockID solid = registerTestBlock(
        registry, "rigel:trace_cached_solid");
    const ChunkCoord coord{0, 0, 0};
    const ChunkCoord away{0, 2, 0};

    Chunk& chunk = manager.getOrCreateChunk(coord);
    chunk.setBlock(0, 0, 0, BlockState{solid}, registry);
    chunk.setWorldGenVersion(generator->config().world.version);
    chunk.setLoadedFromDisk(true);
    chunk.clearDirty();
    ChunkMesh cachedMesh;
    cachedMesh.vertices.push_back(VoxelVertex{});
    cachedMesh.indices.push_back(0);
    meshStore.set(coord, std::move(cachedMesh));
    auto tracer = std::make_shared<ChunkVisibilityTracer>(
        ChunkVisibilityTracer::Config{coord, 2});

    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 10;
    stream.workerThreads = 0;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    streamer.setVisibilityTracer(tracer);

    streamer.update(coord.toWorldCenter());
    auto records = tracer->snapshot();
    CHECK_EQ(records.size(), static_cast<size_t>(1));
    CHECK_EQ(
        records.front().outcome,
        ChunkVisibilityOutcome::CachedNonemptyGeometry);
    CHECK(records.front().stage(ChunkVisibilityStage::Desired).has_value());
    CHECK(!records.front().stage(ChunkVisibilityStage::DataRequest).has_value());
    CHECK(!records.front().stage(ChunkVisibilityStage::DataReady).has_value());
    CHECK(!records.front().meshTask.has_value());
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(0));

    streamer.update(away.toWorldCenter());
    records = tracer->snapshot();
    CHECK_EQ(
        records.front().drawOutcome,
        ChunkVisibilityDrawOutcome::CameraLeftBeforeDraw);
    std::vector<ChunkStreamer::DebugChunkState> historicalStates;
    streamer.getDebugStates(historicalStates, coord, 0);
    CHECK_EQ(historicalStates.size(), static_cast<size_t>(1));
    CHECK_EQ(historicalStates.front().pipelineOwner,
             ChunkStreamer::DebugPipelineOwner::Complete);
    CHECK_EQ(historicalStates.front().historicalTraceKey,
             std::optional<ChunkVisibilityLifecycleKey>{records.front().key});
    CHECK_EQ(historicalStates.front().historicalTraceKind,
             ChunkVisibilityLifecycleKind::CameraDemand);
    CHECK_EQ(historicalStates.front().historicalTraceOutcome,
             ChunkVisibilityOutcome::CachedNonemptyGeometry);
    CHECK_EQ(historicalStates.front().historicalTraceDrawOutcome,
             ChunkVisibilityDrawOutcome::CameraLeftBeforeDraw);

    streamer.update(coord.toWorldCenter());
    records = tracer->snapshot();
    CHECK_EQ(records.size(), static_cast<size_t>(2));
    CHECK_EQ(
        records.back().outcome,
        ChunkVisibilityOutcome::CachedNonemptyGeometry);
    CHECK_NE(records.front().key, records.back().key);
    streamer.getDebugStates(historicalStates, coord, 0);
    CHECK_EQ(historicalStates.size(), static_cast<size_t>(1));
    CHECK_EQ(historicalStates.front().pipelineOwner,
             ChunkStreamer::DebugPipelineOwner::Complete);
    CHECK_EQ(historicalStates.front().historicalTraceKey,
             std::optional<ChunkVisibilityLifecycleKey>{records.back().key});
    CHECK_NE(historicalStates.front().historicalTraceKey,
             std::optional<ChunkVisibilityLifecycleKey>{records.front().key});
}

TEST_CASE(ChunkStreamer_VisibilityTraceDistinguishesVoxelAndGeometryEmpty) {
    const ChunkCoord coord{0, 0, 0};

    {
        ChunkManager manager;
        BlockRegistry registry;
        WorldMeshStore meshStore;
        auto generator = makeGenerator(registry);
        Chunk& chunk = manager.getOrCreateChunk(coord);
        chunk.setWorldGenVersion(generator->config().world.version);
        chunk.setLoadedFromDisk(true);

        auto tracer = std::make_shared<ChunkVisibilityTracer>(
            ChunkVisibilityTracer::Config{coord, 2});
        ChunkStreamer streamer(
            manager, meshStore, registry, nullptr, generator);
        StreamingConfig stream;
        stream.viewDistanceChunks = 0;
        stream.unloadDistanceChunks = 0;
        stream.workerThreads = 0;
        stream.maxResidentChunks = 0;
        streamer.setConfig(stream);
        streamer.setVisibilityTracer(tracer);

        streamer.update(coord.toWorldCenter());
        streamer.processCompletions();

        const auto records = tracer->snapshot();
        CHECK_EQ(records.size(), static_cast<size_t>(1));
        CHECK_EQ(records.front().outcome, ChunkVisibilityOutcome::VoxelEmpty);
        CHECK(!records.front()
                   .stage(ChunkVisibilityStage::ResultAccepted)
                   .has_value());
        CHECK_EQ(
            streamer.workMetrics().meshJobsStarted,
            static_cast<uint64_t>(0));
        std::vector<ChunkStreamer::DebugChunkState> states;
        streamer.getDebugStates(states, coord, 0);
        CHECK_EQ(states.size(), static_cast<size_t>(1));
        CHECK_EQ(states.front().state, ChunkStreamer::DebugState::VoxelEmpty);
        CHECK_EQ(states.front().pipelineOwner,
                 ChunkStreamer::DebugPipelineOwner::Complete);
        CHECK_EQ(states.front().voxelOccupancy,
                 ChunkStreamer::DebugVoxelOccupancy::Empty);
        CHECK_EQ(states.front().installedGeometry,
                 ChunkStreamer::DebugInstalledGeometry::None);
        CHECK_EQ(states.front().historicalTraceOutcome,
                 ChunkVisibilityOutcome::VoxelEmpty);
        CHECK_EQ(states.front().drawEvidence,
                 ChunkStreamer::DebugDrawEvidence::NotApplicable);
        const auto metricsBeforeDebugCollection = streamer.workMetrics();
        std::vector<ChunkStreamer::DebugChunkState> outsideStates;
        streamer.getDebugStates(outsideStates, ChunkCoord{1, 0, 0}, 0);
        CHECK(outsideStates.empty());
        streamer.getDebugStates(states, coord, 0);
        CHECK_EQ(streamer.workMetrics().meshJobsStarted,
                 metricsBeforeDebugCollection.meshJobsStarted);
        CHECK_EQ(streamer.workMetrics().meshJobsCompleted,
                 metricsBeforeDebugCollection.meshJobsCompleted);
        CHECK_EQ(streamer.workMetrics().schedulerCoordinatesInspected,
                 metricsBeforeDebugCollection.schedulerCoordinatesInspected);

        std::weak_ptr<ChunkVisibilityTracer> traceOwner = tracer;
        streamer.setVisibilityTracer({});
        tracer.reset();
        CHECK(traceOwner.expired());
        CHECK_EQ(states.front().historicalTraceOutcome,
                 ChunkVisibilityOutcome::VoxelEmpty);
    }

    {
        ChunkManager manager;
        BlockRegistry registry;
        WorldMeshStore meshStore;
        auto generator = makeGenerator(registry);
        BlockID solid = registerTestBlock(
            registry, "rigel:trace_occluded_solid");

        Chunk& chunk = manager.getOrCreateChunk(coord);
        chunk.fill(BlockState{solid}, registry);
        chunk.setWorldGenVersion(generator->config().world.version);
        chunk.setLoadedFromDisk(true);
        for (int index = 0; index < DirectionCount; ++index) {
            int dx = 0;
            int dy = 0;
            int dz = 0;
            directionOffset(static_cast<Direction>(index), dx, dy, dz);
            Chunk& neighbor = manager.getOrCreateChunk(
                coord.offset(dx, dy, dz));
            neighbor.fill(BlockState{solid}, registry);
            neighbor.setWorldGenVersion(generator->config().world.version);
            neighbor.setLoadedFromDisk(true);
        }

        auto tracer = std::make_shared<ChunkVisibilityTracer>(
            ChunkVisibilityTracer::Config{coord, 2});
        ChunkStreamer streamer(
            manager, meshStore, registry, nullptr, generator);
        StreamingConfig stream;
        stream.viewDistanceChunks = 0;
        stream.unloadDistanceChunks = 0;
        stream.workerThreads = 0;
        stream.maxResidentChunks = 0;
        streamer.setConfig(stream);
        streamer.setVisibilityTracer(tracer);

        streamer.update(coord.toWorldCenter());
        streamer.processCompletions();

        const auto records = tracer->snapshot();
        CHECK_EQ(records.size(), static_cast<size_t>(1));
        CHECK_EQ(
            records.front().outcome,
            ChunkVisibilityOutcome::AcceptedEmptyGeometry);
        CHECK(records.front()
                  .stage(ChunkVisibilityStage::ResultAccepted)
                  .has_value());
        CHECK(!records.front()
                   .stage(ChunkVisibilityStage::FirstDraw)
                   .has_value());
        CHECK(meshStore.contains(coord));
        std::vector<ChunkStreamer::DebugChunkState> states;
        streamer.getDebugStates(states, coord, 0);
        CHECK_EQ(states.size(), static_cast<size_t>(1));
        CHECK_EQ(states.front().state,
                 ChunkStreamer::DebugState::AcceptedEmptyGeometry);
        CHECK_EQ(states.front().voxelOccupancy,
                 ChunkStreamer::DebugVoxelOccupancy::Nonempty);
        CHECK_EQ(states.front().installedGeometry,
                 ChunkStreamer::DebugInstalledGeometry::Empty);
        CHECK_EQ(states.front().historicalTraceOutcome,
                 ChunkVisibilityOutcome::AcceptedEmptyGeometry);
        CHECK_EQ(states.front().drawEvidence,
                 ChunkStreamer::DebugDrawEvidence::NotApplicable);
    }
}

TEST_CASE(ChunkStreamer_VisibilityTraceRemeshesResidentVoxelEmptyChunk) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    BlockID solid = registerTestBlock(
        registry, "rigel:trace_voxel_empty_remesh_solid");
    const ChunkCoord coord{0, 0, 0};
    Chunk& chunk = manager.getOrCreateChunk(coord);
    chunk.setWorldGenVersion(generator->config().world.version);
    chunk.setLoadedFromDisk(true);
    chunk.clearPersistDirty();
    chunk.clearDirty();
    addLoadedNeighborShell(
        manager,
        coord,
        std::nullopt,
        generator->config().world.version);

    ChunkStreamer streamer(
        manager, meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 10;
    stream.updateBudgetPerFrame = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 0;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    streamer.markSpawnDiscoveryComplete();
    streamer.update(coord.toWorldCenter());
    streamer.processCompletions();
    CHECK(!meshStore.contains(coord));
    CHECK_EQ(
        streamer.workMetrics().meshJobsStarted,
        static_cast<uint64_t>(0));

    auto tracer = std::make_shared<ChunkVisibilityTracer>(
        ChunkVisibilityTracer::Config{coord, 2});
    streamer.setVisibilityTracer(tracer);
    chunk.setBlock(0, 0, 0, BlockState{solid}, registry);
    streamer.update(coord.toWorldCenter());
    streamer.processCompletions();

    const auto records = tracer->snapshot();
    CHECK_EQ(records.size(), static_cast<size_t>(1));
    CHECK_EQ(records.front().kind, ChunkVisibilityLifecycleKind::Remesh);
    CHECK_EQ(records.front().origin, ChunkVisibilityOrigin::Remesh);
    CHECK_EQ(
        records.front().outcome,
        ChunkVisibilityOutcome::AcceptedNonemptyGeometry);
    CHECK(records.front().meshTask.has_value());
    CHECK(meshStore.contains(coord));
    CHECK_EQ(
        streamer.workMetrics().meshJobsStarted,
        static_cast<uint64_t>(1));
    CHECK_EQ(
        streamer.workMetrics().meshJobsCompleted,
        static_cast<uint64_t>(1));
    CHECK_EQ(
        streamer.workMetrics().meshJobsAccepted,
        static_cast<uint64_t>(1));
    CHECK_EQ(streamer.diagnostics().mesh.pending, static_cast<size_t>(0));
    CHECK_EQ(streamer.diagnostics().mesh.inFlight, static_cast<size_t>(0));
    std::vector<ChunkStreamer::DebugChunkState> states;
    streamer.getDebugStates(states, coord, 0);
    CHECK_EQ(states.size(), static_cast<size_t>(1));
    CHECK_EQ(states.front().state,
             ChunkStreamer::DebugState::AcceptedNonemptyGeometry);
    CHECK_EQ(states.front().pipelineOwner,
             ChunkStreamer::DebugPipelineOwner::Complete);
    CHECK_EQ(states.front().voxelOccupancy,
             ChunkStreamer::DebugVoxelOccupancy::Nonempty);
    CHECK_EQ(states.front().installedGeometry,
             ChunkStreamer::DebugInstalledGeometry::Nonempty);
    CHECK_EQ(states.front().historicalTraceOutcome,
             ChunkVisibilityOutcome::AcceptedNonemptyGeometry);
    CHECK_EQ(states.front().historicalTraceDrawOutcome, std::nullopt);
    CHECK_EQ(states.front().drawEvidence,
             ChunkStreamer::DebugDrawEvidence::NotDrawn);
}

TEST_CASE(ChunkStreamer_DisabledVisibilityTracePreservesStationaryWork) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    BlockID solid = registerTestBlock(
        registry, "rigel:disabled_trace_solid");
    const ChunkCoord coord{0, 0, 0};

    Chunk& chunk = manager.getOrCreateChunk(coord);
    chunk.setBlock(0, 0, 0, BlockState{solid}, registry);
    chunk.setWorldGenVersion(generator->config().world.version);
    chunk.setLoadedFromDisk(true);

    ChunkStreamer streamer(manager, meshStore, registry, nullptr, generator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 0;
    stream.workerThreads = 0;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    streamer.markSpawnDiscoveryComplete();
    streamer.update(coord.toWorldCenter());
    streamer.processCompletions();
    for (uint32_t update = 0;
         update <= StreamingDiagnosticSnapshot::QuiescenceUpdateWindow;
         ++update) {
        streamer.update(coord.toWorldCenter());
        streamer.processCompletions();
    }
    CHECK_EQ(
        streamer.diagnostics().state,
        StreamingLifecycleState::Quiescent);
    const auto baseline = streamer.workMetrics();

    auto clock = std::make_shared<IncrementingTraceClock>();
    streamer.setVisibilityTracer(std::make_shared<ChunkVisibilityTracer>(
        ChunkVisibilityTracer::Config{coord, 0},
        [clock]() { return clock->now(); }));
    streamer.update(coord.toWorldCenter());
    streamer.processCompletions();
    const auto disabled = streamer.workMetrics();

    CHECK_EQ(clock->reads(), static_cast<size_t>(0));
    CHECK_EQ(
        streamer.diagnostics().state,
        StreamingLifecycleState::Quiescent);
    CHECK_EQ(disabled.meshJobsStarted, baseline.meshJobsStarted);
    CHECK_EQ(
        disabled.lastUpdateDesiredBuildCoordinatesInspected,
        baseline.lastUpdateDesiredBuildCoordinatesInspected);
    CHECK_EQ(
        disabled.lastUpdateSchedulerCoordinatesInspected,
        baseline.lastUpdateSchedulerCoordinatesInspected);
    CHECK_EQ(
        disabled.lastUpdateCacheEvictionCoordinatesInspected,
        baseline.lastUpdateCacheEvictionCoordinatesInspected);
    CHECK_EQ(
        disabled.lastUpdateResidentEvictionCoordinatesInspected,
        baseline.lastUpdateResidentEvictionCoordinatesInspected);
    CHECK_EQ(
        disabled.lastUpdateDeferredEvictionCoordinatesInspected,
        baseline.lastUpdateDeferredEvictionCoordinatesInspected);
}

TEST_CASE(ChunkStreamer_VisibilityTraceModesPreserveSchedulingAndQuiescence) {
    enum class TraceMode : uint8_t {
        Absent,
        Disabled,
        Enabled
    };
    struct Result {
        std::vector<ChunkCoord> dispatchOrder;
        std::vector<std::array<size_t, 8>> schedulerStates;
        std::array<uint64_t, 10> counters{};
        StreamingLifecycleState lifecycle = StreamingLifecycleState::Streaming;
        size_t clockReads = 0;
    };

    auto run = [](TraceMode mode) {
        ChunkManager manager;
        BlockRegistry registry;
        WorldMeshStore meshStore;
        auto generator = makeGenerator(registry);
        BlockID solid = registerTestBlock(
            registry, "rigel:trace_mode_parity_solid");
        const ChunkCoord center{0, 0, 0};
        const std::array desired{
            center,
            ChunkCoord{1, 0, 0},
            ChunkCoord{-1, 0, 0},
            ChunkCoord{0, 1, 0},
            ChunkCoord{0, -1, 0},
            ChunkCoord{0, 0, 1},
            ChunkCoord{0, 0, -1}
        };
        for (const ChunkCoord coord : desired) {
            Chunk& chunk = manager.getOrCreateChunk(coord);
            chunk.setBlock(0, 0, 0, BlockState{solid}, registry);
            chunk.setWorldGenVersion(generator->config().world.version);
            chunk.setLoadedFromDisk(true);
        }

        auto clock = std::make_shared<IncrementingTraceClock>();
        ChunkStreamer streamer(
            manager, meshStore, registry, nullptr, generator);
        StreamingConfig stream;
        stream.viewDistanceChunks = 1;
        stream.unloadDistanceChunks = 1;
        stream.meshQueueLimit = 2;
        stream.updateBudgetPerFrame = 2;
        stream.workerThreads = 0;
        stream.maxResidentChunks = 0;
        streamer.setConfig(stream);
        streamer.markSpawnDiscoveryComplete();
        if (mode != TraceMode::Absent) {
            streamer.setVisibilityTracer(
                std::make_shared<ChunkVisibilityTracer>(
                    ChunkVisibilityTracer::Config{
                        center,
                        mode == TraceMode::Enabled
                            ? static_cast<size_t>(8)
                            : static_cast<size_t>(0)},
                    [clock]() { return clock->now(); }));
        }

        Result result;
        auto captureState = [&]() {
            const auto& diagnostics = streamer.diagnostics();
            result.schedulerStates.push_back({
                diagnostics.generation.pending,
                diagnostics.generation.inFlight,
                diagnostics.chunkLoad.pending,
                diagnostics.chunkLoad.inFlight,
                diagnostics.mesh.pending,
                diagnostics.mesh.inFlight,
                diagnostics.eviction.pending,
                diagnostics.eviction.inFlight
            });
        };

        for (size_t update = 0; update < 16; ++update) {
            streamer.update(center.toWorldCenter());
            const auto dispatched =
                Rigel::Voxel::detail::ChunkStreamerTestAccess::
                    inFlightMeshDispatchOrder(streamer);
            result.dispatchOrder.insert(
                result.dispatchOrder.end(),
                dispatched.begin(),
                dispatched.end());
            captureState();
            streamer.processCompletions();
            captureState();
        }
        const auto& metrics = streamer.workMetrics();
        result.counters = {
            metrics.generationJobsStarted,
            metrics.chunkLoadRequestsStarted,
            metrics.meshJobsStarted,
            metrics.meshJobsCompleted,
            metrics.meshJobsAccepted,
            metrics.meshJobsRejectedStale,
            metrics.meshJobsFailed,
            metrics.meshInvalidations,
            metrics.meshRequestsCoalesced,
            metrics.schedulerCoordinatesInspected
        };
        result.lifecycle = streamer.diagnostics().state;
        result.clockReads = clock->reads();
        return result;
    };

    const auto absent = run(TraceMode::Absent);
    const auto disabled = run(TraceMode::Disabled);
    const auto enabled = run(TraceMode::Enabled);
    CHECK_EQ(absent.dispatchOrder, disabled.dispatchOrder);
    CHECK_EQ(absent.dispatchOrder, enabled.dispatchOrder);
    CHECK_EQ(absent.schedulerStates, disabled.schedulerStates);
    CHECK_EQ(absent.schedulerStates, enabled.schedulerStates);
    CHECK_EQ(absent.counters, disabled.counters);
    CHECK_EQ(absent.counters, enabled.counters);
    CHECK_EQ(disabled.clockReads, static_cast<size_t>(0));
    CHECK(enabled.clockReads > 0);
    CHECK_EQ(absent.lifecycle, StreamingLifecycleState::Quiescent);
    CHECK_EQ(disabled.lifecycle, absent.lifecycle);
    CHECK_EQ(enabled.lifecycle, absent.lifecycle);
}

TEST_CASE(ChunkStreamer_ExternalLoadTraceModesPreserveSchedulerState) {
    enum class TraceMode : uint8_t {
        Absent,
        Disabled,
        Enabled
    };
    struct Result {
        std::vector<ChunkCoord> dispatchOrder;
        std::vector<std::array<size_t, 8>> schedulerStates;
        std::array<uint64_t, 8> counters{};
        StreamingLifecycleState lifecycle = StreamingLifecycleState::Streaming;
        size_t clockReads = 0;
    };

    auto run = [](TraceMode mode) {
        ChunkManager manager;
        BlockRegistry registry;
        WorldMeshStore meshStore;
        auto generator = makeGenerator(registry);
        BlockID solid = registerTestBlock(
            registry, "rigel:external_load_trace_parity_solid");
        const ChunkCoord coord{0, 0, 0};
        addLoadedNeighborShell(
            manager,
            coord,
            std::nullopt,
            generator->config().world.version);

        auto clock = std::make_shared<IncrementingTraceClock>();
        std::optional<ChunkLoadRequest> request;
        std::vector<ChunkLoadCompletion> completions;
        ChunkStreamer streamer(
            manager, meshStore, registry, nullptr, generator);
        StreamingConfig stream;
        stream.viewDistanceChunks = 0;
        stream.unloadDistanceChunks = 1;
        stream.workerThreads = 0;
        stream.maxResidentChunks = 0;
        streamer.setConfig(stream);
        streamer.markSpawnDiscoveryComplete();
        if (mode != TraceMode::Absent) {
            streamer.setVisibilityTracer(
                std::make_shared<ChunkVisibilityTracer>(
                    ChunkVisibilityTracer::Config{
                        coord,
                        mode == TraceMode::Enabled
                            ? static_cast<size_t>(2)
                            : static_cast<size_t>(0)},
                    [clock]() { return clock->now(); }));
        }
        streamer.setChunkLoader([&](ChunkLoadRequest value) {
            request = value;
            return ChunkLoadRequestResult::Queued;
        });
        streamer.setChunkLoadDrain([&](size_t) {
            auto drained = std::move(completions);
            completions.clear();
            return drained;
        });

        Result result;
        auto captureState = [&]() {
            const auto& diagnostics = streamer.diagnostics();
            result.schedulerStates.push_back({
                diagnostics.generation.pending,
                diagnostics.generation.inFlight,
                diagnostics.chunkLoad.pending,
                diagnostics.chunkLoad.inFlight,
                diagnostics.mesh.pending,
                diagnostics.mesh.inFlight,
                diagnostics.eviction.pending,
                diagnostics.eviction.inFlight
            });
        };
        auto updateAndCapture = [&]() {
            streamer.update(coord.toWorldCenter());
            const auto dispatched =
                Rigel::Voxel::detail::ChunkStreamerTestAccess::
                    inFlightMeshDispatchOrder(streamer);
            result.dispatchOrder.insert(
                result.dispatchOrder.end(),
                dispatched.begin(),
                dispatched.end());
            captureState();
            streamer.processCompletions();
            captureState();
        };

        streamer.update(coord.toWorldCenter());
        captureState();
        CHECK(request.has_value());
        Chunk& chunk = manager.getOrCreateChunk(coord);
        chunk.setBlock(0, 0, 0, BlockState{solid}, registry);
        chunk.setWorldGenVersion(generator->config().world.version);
        chunk.setLoadedFromDisk(true);
        chunk.clearPersistDirty();
        completions.push_back({
            coord,
            request->requestId,
            ChunkLoadOutcome::Loaded,
            {}});
        streamer.processCompletions();
        captureState();

        for (size_t update = 0;
             update < StreamingDiagnosticSnapshot::QuiescenceUpdateWindow + 2;
             ++update) {
            updateAndCapture();
        }

        const auto& metrics = streamer.workMetrics();
        result.counters = {
            metrics.generationJobsStarted,
            metrics.chunkLoadRequestsStarted,
            metrics.meshJobsStarted,
            metrics.meshJobsCompleted,
            metrics.meshJobsAccepted,
            metrics.meshJobsRejectedStale,
            metrics.meshJobsFailed,
            metrics.schedulerCoordinatesInspected
        };
        result.lifecycle = streamer.diagnostics().state;
        result.clockReads = clock->reads();
        return result;
    };

    const auto absent = run(TraceMode::Absent);
    const auto disabled = run(TraceMode::Disabled);
    const auto enabled = run(TraceMode::Enabled);
    const std::vector<ChunkCoord> expectedDispatchOrder{ChunkCoord{0, 0, 0}};
    CHECK_EQ(absent.dispatchOrder, expectedDispatchOrder);
    CHECK_EQ(absent.dispatchOrder, disabled.dispatchOrder);
    CHECK_EQ(absent.dispatchOrder, enabled.dispatchOrder);
    CHECK_EQ(absent.schedulerStates, disabled.schedulerStates);
    CHECK_EQ(absent.schedulerStates, enabled.schedulerStates);
    CHECK_EQ(absent.counters, disabled.counters);
    CHECK_EQ(absent.counters, enabled.counters);
    CHECK_EQ(disabled.clockReads, static_cast<size_t>(0));
    CHECK(enabled.clockReads > 0);
    CHECK_EQ(absent.lifecycle, StreamingLifecycleState::Quiescent);
    CHECK_EQ(disabled.lifecycle, absent.lifecycle);
    CHECK_EQ(enabled.lifecycle, absent.lifecycle);
}

TEST_CASE(ChunkStreamer_AbsentAndDisabledTraceIgnoreDepartureTraceState) {
    for (const bool installDisabledTracer : {false, true}) {
        ChunkManager manager;
        BlockRegistry registry;
        WorldMeshStore meshStore;
        auto generator = makeGenerator(registry);
        const ChunkCoord departed{0, 0, 0};

        ChunkStreamer streamer(
            manager, meshStore, registry, nullptr, generator);
        StreamingConfig stream;
        stream.viewDistanceChunks = 0;
        stream.unloadDistanceChunks = 8;
        stream.updateBudgetPerFrame = 0;
        stream.workerThreads = 0;
        stream.maxResidentChunks = 0;
        streamer.setConfig(stream);
        streamer.markSpawnDiscoveryComplete();

        auto disabledClock = std::make_shared<IncrementingTraceClock>();
        if (installDisabledTracer) {
            streamer.setVisibilityTracer(
                std::make_shared<ChunkVisibilityTracer>(
                    ChunkVisibilityTracer::Config{departed, 0},
                    [disabledClock]() { return disabledClock->now(); }));
        }
        streamer.update(departed.toWorldCenter());

        auto drawTracer = std::make_shared<ChunkVisibilityTracer>(
            ChunkVisibilityTracer::Config{departed, 1});
        const auto drawKey = *drawTracer->begin(
            ChunkVisibilityLifecycleKind::CameraDemand);
        drawTracer->complete(
            drawKey,
            ChunkVisibilityOutcome::AcceptedNonemptyGeometry);
        ChunkMesh mesh;
        mesh.vertices.resize(3);
        mesh.indices.resize(3);
        meshStore.set(
            departed,
            std::move(mesh),
            ChunkVisibilityTraceLink{
                drawKey,
                ChunkVisibilityLifecycleKind::CameraDemand,
                drawTracer});

        streamer.update(ChunkCoord{1, 0, 0}.toWorldCenter());

        const auto measurement = drawTracer->measurement();
        CHECK_EQ(measurement.records.size(), static_cast<size_t>(1));
        CHECK(!measurement.records.front().drawOutcome.has_value());
        CHECK_EQ(disabledClock->reads(), static_cast<size_t>(0));
    }
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
        auto visibilityTracer = std::make_shared<ChunkVisibilityTracer>(
            ChunkVisibilityTracer::Config{coord, 1});
        streamer.setVisibilityTracer(visibilityTracer);
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
        const auto visibilityRecords = visibilityTracer->snapshot();
        CHECK_EQ(visibilityRecords.size(), static_cast<size_t>(1));
        CHECK_EQ(
            visibilityRecords.front().outcome,
            ChunkVisibilityOutcome::Failed);
        CHECK(visibilityRecords.front()
                  .stage(ChunkVisibilityStage::WorkerFinish)
                  .has_value());
        CHECK(!visibilityRecords.front()
                   .stage(ChunkVisibilityStage::ResultAccepted)
                   .has_value());

        std::vector<ChunkStreamer::DebugChunkState> states;
        streamer.getDebugStates(states, coord, 0);
        CHECK_EQ(states.size(), static_cast<size_t>(1));
        CHECK_EQ(states.front().coord, coord);
        CHECK_EQ(states.front().state,
                 ChunkStreamer::DebugState::TerminalFailure);
        CHECK_EQ(states.front().failure, ChunkStreamer::DebugFailure::Mesh);

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
    streamer.getDebugStates(states, coord, 0);
    CHECK_EQ(states.size(), static_cast<size_t>(1));
    CHECK_NE(states.front().state,
             ChunkStreamer::DebugState::WaitingForData);
    CHECK_EQ(states.front().state,
             ChunkStreamer::DebugState::MeshSchedulerWait);
    CHECK_EQ(states.front().pipelineOwner,
             ChunkStreamer::DebugPipelineOwner::MeshScheduler);
    CHECK_EQ(states.front().voxelOccupancy,
             ChunkStreamer::DebugVoxelOccupancy::Nonempty);
    CHECK_EQ(states.front().installedGeometry,
             ChunkStreamer::DebugInstalledGeometry::None);

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

TEST_CASE(ChunkStreamer_EmptyGeneratedArrivalKeepsSolidNeighborMesh) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    const BlockID solid =
        registerTestBlock(registry, "rigel:empty_generated_arrival_solid");
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
    streamer.markSpawnDiscoveryComplete();

    streamer.update(survivingCoord.toWorldCenter());
    streamer.processCompletions();
    CHECK(meshStore.contains(survivingCoord));
    CHECK(!surviving.isDirty());
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(1));
    CHECK_EQ(streamer.workMetrics().meshJobsCompleted, static_cast<uint64_t>(1));
    CHECK_EQ(streamer.workMetrics().meshJobsAccepted, static_cast<uint64_t>(1));
    const uint32_t settledRevision = surviving.meshRevision();
    uint64_t settledMeshRevision = 0;
    size_t settledIndexCount = 0;
    meshStore.forEach([&](const WorldMeshEntry& entry) {
        if (entry.coord == survivingCoord) {
            settledMeshRevision = entry.revision.value;
            settledIndexCount = entry.mesh.indexCount();
        }
    });
    CHECK(settledMeshRevision > 0);
    CHECK_EQ(settledIndexCount, static_cast<size_t>(36));

    stream.viewDistanceChunks = 1;
    streamer.setConfig(stream);
    streamer.update(survivingCoord.toWorldCenter());
    CHECK_EQ(streamer.workMetrics().generationJobsStarted,
             static_cast<uint64_t>(1));
    streamer.processCompletions();

    const Chunk* arrived = manager.getChunk(arrivingCoord);
    CHECK(arrived != nullptr);
    CHECK(arrived->isEmpty());
    CHECK_EQ(surviving.meshRevision(), settledRevision);
    CHECK(!surviving.isDirty());

    for (int update = 0; update < 4; ++update) {
        streamer.update(survivingCoord.toWorldCenter());
        streamer.processCompletions();
    }
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(1));
    CHECK_EQ(streamer.workMetrics().meshJobsCompleted, static_cast<uint64_t>(1));
    CHECK_EQ(streamer.workMetrics().meshJobsAccepted, static_cast<uint64_t>(1));
    CHECK_EQ(streamer.diagnostics().mesh.pending, static_cast<size_t>(0));
    CHECK_EQ(streamer.diagnostics().mesh.inFlight, static_cast<size_t>(0));
    CHECK_EQ(streamer.diagnostics().state, StreamingLifecycleState::Quiescent);
    meshStore.forEach([&](const WorldMeshEntry& entry) {
        if (entry.coord == survivingCoord) {
            CHECK_EQ(entry.revision.value, settledMeshRevision);
            CHECK_EQ(entry.mesh.indexCount(), settledIndexCount);
        }
    });
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
    streamer.markSpawnDiscoveryComplete();

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

TEST_CASE(ChunkStreamer_EmptyPersistedArrivalKeepsSolidNeighborMesh) {
    WorldResources resources;
    World world(resources);
    auto& registry = resources.registry();
    auto generator = makeGenerator(registry);
    world.setGenerator(generator);
    auto& manager = world.chunkManager();
    WorldMeshStore meshStore;
    const BlockID solid =
        registerTestBlock(registry, "rigel:empty_persisted_arrival_solid");
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
    streamer.markSpawnDiscoveryComplete();

    streamer.update(survivingCoord.toWorldCenter());
    streamer.processCompletions();
    CHECK(meshStore.contains(survivingCoord));
    CHECK(!surviving.isDirty());
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(1));
    CHECK_EQ(streamer.workMetrics().meshJobsCompleted, static_cast<uint64_t>(1));
    CHECK_EQ(streamer.workMetrics().meshJobsAccepted, static_cast<uint64_t>(1));
    const uint32_t settledRevision = surviving.meshRevision();
    uint64_t settledMeshRevision = 0;
    size_t settledIndexCount = 0;
    meshStore.forEach([&](const WorldMeshEntry& entry) {
        if (entry.coord == survivingCoord) {
            settledMeshRevision = entry.revision.value;
            settledIndexCount = entry.mesh.indexCount();
        }
    });
    CHECK(settledMeshRevision > 0);
    CHECK_EQ(settledIndexCount, static_cast<size_t>(36));

    stream.viewDistanceChunks = 1;
    streamer.setConfig(stream);
    streamer.update(survivingCoord.toWorldCenter());
    CHECK(loader->isPending(arrivingCoord));
    streamer.processCompletions();

    const Chunk* arrived = manager.getChunk(arrivingCoord);
    CHECK(arrived != nullptr);
    CHECK(arrived->loadedFromDisk());
    CHECK(arrived->isEmpty());
    CHECK_EQ(surviving.meshRevision(), settledRevision);
    CHECK(!surviving.isDirty());
    CHECK(!loader->isPending(arrivingCoord));

    for (int update = 0; update < 4; ++update) {
        streamer.update(survivingCoord.toWorldCenter());
        streamer.processCompletions();
    }
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(1));
    CHECK_EQ(streamer.workMetrics().meshJobsCompleted, static_cast<uint64_t>(1));
    CHECK_EQ(streamer.workMetrics().meshJobsAccepted, static_cast<uint64_t>(1));
    CHECK_EQ(streamer.diagnostics().mesh.pending, static_cast<size_t>(0));
    CHECK_EQ(streamer.diagnostics().mesh.inFlight, static_cast<size_t>(0));
    CHECK_EQ(streamer.diagnostics().state, StreamingLifecycleState::Quiescent);
    meshStore.forEach([&](const WorldMeshEntry& entry) {
        if (entry.coord == survivingCoord) {
            CHECK_EQ(entry.revision.value, settledMeshRevision);
            CHECK_EQ(entry.mesh.indexCount(), settledIndexCount);
        }
    });
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
             static_cast<uint64_t>(1));
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
    const ChunkCoord survivingCoord{0, -1, 0};
    const ChunkCoord arrivingCoord{1, -1, 0};

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
    const Chunk* arrived = manager.getChunk(arrivingCoord);
    CHECK(arrived != nullptr);
    CHECK(!arrived->isEmpty());
    CHECK_EQ(surviving.meshRevision(), queuedRevision + 1);

    stream.viewDistanceChunks = 0;
    streamer.setConfig(stream);
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
    CHECK_EQ(*replacementIndexCount, static_cast<size_t>(30));
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

TEST_CASE(ChunkStreamer_ExposesOccludedFringeMeshAfterNeighborEviction) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    const BlockID visible =
        registerTestBlock(registry, "rigel:occluded_fringe_visible");
    BlockType hiddenType;
    hiddenType.identifier = "rigel:occluded_fringe_hidden";
    hiddenType.model = "hidden";
    hiddenType.isOpaque = true;
    const BlockID hidden =
        registry.registerBlock(hiddenType.identifier, hiddenType);

    const ChunkCoord survivingCoord{0, 0, 0};
    const ChunkCoord removedCoord{1, 0, 0};
    const ChunkCoord cameraCoord{-2, 0, 0};
    Chunk& surviving = manager.getOrCreateChunk(survivingCoord);
    surviving.setBlock(
        Chunk::SIZE - 1, 16, 16, BlockState{visible}, registry);
    for (const auto& local : std::array<glm::ivec3, 5>{
             glm::ivec3{Chunk::SIZE - 2, 16, 16},
             glm::ivec3{Chunk::SIZE - 1, 15, 16},
             glm::ivec3{Chunk::SIZE - 1, 17, 16},
             glm::ivec3{Chunk::SIZE - 1, 16, 15},
             glm::ivec3{Chunk::SIZE - 1, 16, 17}}) {
        surviving.setBlock(
            local.x, local.y, local.z, BlockState{hidden}, registry);
    }
    surviving.setWorldGenVersion(generator->config().world.version);
    surviving.setLoadedFromDisk(true);
    surviving.clearPersistDirty();

    for (int i = 0; i < DirectionCount; ++i) {
        Direction direction = static_cast<Direction>(i);
        int dx = 0;
        int dy = 0;
        int dz = 0;
        directionOffset(direction, dx, dy, dz);
        const ChunkCoord neighborCoord =
            survivingCoord.offset(dx, dy, dz);
        Chunk& neighbor = manager.getOrCreateChunk(neighborCoord);
        if (neighborCoord == removedCoord) {
            neighbor.setBlock(0, 16, 16, BlockState{hidden}, registry);
        }
        neighbor.setWorldGenVersion(generator->config().world.version);
        neighbor.setLoadedFromDisk(true);
        neighbor.clearPersistDirty();
        neighbor.clearDirty();
    }

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
    streamer.markSpawnDiscoveryComplete();

    streamer.update(survivingCoord.toWorldCenter());
    CHECK(waitForMeshCompletions(streamer, 1));
    CHECK(!surviving.isEmpty());
    CHECK(!surviving.isDirty());
    CHECK(meshStore.contains(survivingCoord));
    CHECK_EQ(installedMeshIndexCount(meshStore, survivingCoord),
             static_cast<size_t>(0));
    const uint64_t emptyMeshRevision =
        installedMeshRevision(meshStore, survivingCoord);
    CHECK(emptyMeshRevision > 0);
    for (int i = 0; i < 8; ++i) {
        streamer.update(survivingCoord.toWorldCenter());
        streamer.processCompletions();
    }
    CHECK_EQ(streamer.diagnostics().state,
             StreamingLifecycleState::Quiescent);

    Chunk& cameraChunk = manager.getOrCreateChunk(cameraCoord);
    cameraChunk.setWorldGenVersion(generator->config().world.version);
    cameraChunk.setLoadedFromDisk(true);
    cameraChunk.clearPersistDirty();
    cameraChunk.clearDirty();
    const uint32_t revisionBeforeEviction = surviving.meshRevision();
    streamer.update(cameraCoord.toWorldCenter());

    CHECK(manager.hasChunk(survivingCoord));
    CHECK(!manager.hasChunk(removedCoord));
    CHECK(surviving.meshRevision() > revisionBeforeEviction);
    CHECK(surviving.isDirty());
    CHECK(meshStore.contains(survivingCoord));
    CHECK_EQ(installedMeshIndexCount(meshStore, survivingCoord),
             static_cast<size_t>(0));
    CHECK_NE(streamer.diagnostics().state,
             StreamingLifecycleState::Quiescent);

    streamer.update(cameraCoord.toWorldCenter());
    CHECK(waitForMeshCompletions(streamer, 2));
    CHECK_EQ(installedMeshIndexCount(meshStore, survivingCoord),
             static_cast<size_t>(6));
    CHECK(installedMeshRevision(meshStore, survivingCoord) > emptyMeshRevision);
    CHECK(!surviving.isDirty());
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(2));
    CHECK_EQ(streamer.workMetrics().meshJobsAccepted, static_cast<uint64_t>(2));

    for (int i = 0; i < 8; ++i) {
        streamer.update(cameraCoord.toWorldCenter());
        streamer.processCompletions();
    }
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(2));
    CHECK_EQ(streamer.diagnostics().state,
             StreamingLifecycleState::Quiescent);
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
    streamer.getDebugStates(states, survivingCoord, 1);
    const auto survivingState = stateFor(survivingCoord);
    const auto removedState = stateFor(removedCoord);
    CHECK(survivingState.has_value());
    CHECK(removedState.has_value());
    CHECK_EQ(*survivingState,
             ChunkStreamer::DebugState::AcceptedNonemptyGeometry);
    CHECK_EQ(*removedState,
             ChunkStreamer::DebugState::AcceptedNonemptyGeometry);

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
    streamer.getDebugStates(states, survivingCoord, 1);
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
    streamer.getDebugStates(states, survivingCoord, 1);
    const auto finalSurvivingState = stateFor(survivingCoord);
    CHECK(finalSurvivingState.has_value());
    CHECK_EQ(*finalSurvivingState,
             ChunkStreamer::DebugState::AcceptedNonemptyGeometry);

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

    streamer.prioritizeMesh(coord);
    Rigel::Voxel::detail::ChunkStreamerTestAccess::
        rememberConfigRetiredDirtyMesh(streamer, coord);
    Rigel::Voxel::detail::ChunkStreamerTestAccess::refreshDiagnostics(
        streamer);
    CHECK_EQ(
        Rigel::Voxel::detail::ChunkStreamerTestAccess::
            configRetiredWorkCount(streamer),
        static_cast<size_t>(1));
    CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::
        inFlightMeshIsPrioritized(streamer, coord));

    Rigel::Voxel::detail::ChunkStreamerTestAccess::reset(streamer);
    CHECK_EQ(
        Rigel::Voxel::detail::ChunkStreamerTestAccess::
            configRetiredWorkCount(streamer),
        static_cast<size_t>(0));
    CHECK(!Rigel::Voxel::detail::ChunkStreamerTestAccess::
        inFlightMeshIsPrioritized(streamer, coord));
    CHECK(!Rigel::Voxel::detail::ChunkStreamerTestAccess::
        hasReplacementPendingMesh(streamer, coord));
    CHECK_EQ(streamer.diagnostics().mesh.pending,
             static_cast<size_t>(0));
    CHECK_EQ(streamer.diagnostics().mesh.inFlight,
             static_cast<size_t>(1));
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

TEST_CASE(ChunkStreamer_ObsoleteReplacementPreservesExplicitPriority) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto originalGenerator = makeGenerator(registry);
    auto replacementGenerator = std::make_shared<WorldGenerator>(
        registry, originalGenerator->config());
    const BlockID solid = registerTestBlock(
        registry, "rigel:replacement_priority_solid");
    const ChunkCoord replacementCoord{0, 0, 0};
    const ChunkCoord cameraCoord{-1, 0, 0};
    const ChunkCoord ordinaryCoord{6, 0, 0};

    for (const ChunkCoord& coord :
         {replacementCoord, cameraCoord, ordinaryCoord}) {
        Chunk& chunk = manager.getOrCreateChunk(coord);
        chunk.setBlock(0, 0, 0, BlockState{solid}, registry);
        chunk.setWorldGenVersion(originalGenerator->config().world.version);
        chunk.setLoadedFromDisk(true);
        chunk.clearPersistDirty();
        chunk.clearDirty();
        meshStore.set(coord, {});
    }
    manager.getChunk(replacementCoord)->invalidateMesh();
    manager.getChunk(ordinaryCoord)->invalidateMesh();

    auto gate = std::make_shared<WorkerGate>();
    std::atomic<size_t> buildsEntered{0};
    ChunkStreamer streamer(
        manager, meshStore, registry, nullptr, originalGenerator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 10;
    stream.meshQueueLimit = 1;
    stream.workerThreads = 2;
    streamer.setConfig(stream);
    streamer.prioritizeMesh(replacementCoord);
    Rigel::Voxel::detail::ChunkStreamerTestAccess::setMeshBuildStartCallback(
        streamer,
        [gate, &buildsEntered]() {
            if (buildsEntered.fetch_add(1, std::memory_order_relaxed) == 0) {
                gate->enterAndWait();
            }
        });
    WorkerGateRelease releaseOnExit(gate);

    streamer.update(replacementCoord.toWorldCenter());
    CHECK(gate->waitUntilEntered());
    streamer.update(cameraCoord.toWorldCenter());
    CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::hasReadyPendingMesh(
        streamer, ordinaryCoord));

    streamer.setGenerator(replacementGenerator);
    CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::
        hasReplacementPendingMesh(streamer, replacementCoord));
    CHECK_EQ(
        Rigel::Voxel::detail::ChunkStreamerTestAccess::
            replacementPendingMeshCount(streamer),
        static_cast<size_t>(1));
    CHECK_EQ(streamer.diagnostics().mesh.pending, static_cast<size_t>(2));
    CHECK_EQ(streamer.diagnostics().mesh.inFlight, static_cast<size_t>(1));

    streamer.update(cameraCoord.toWorldCenter());
    streamer.update(cameraCoord.toWorldCenter());
    CHECK_EQ(streamer.workMetrics().lastUpdateSchedulerCoordinatesInspected,
             static_cast<uint64_t>(0));

    gate->release();
    CHECK(waitForMeshCompletions(streamer, 1));
    CHECK_EQ(streamer.workMetrics().meshJobsRejectedStale,
             static_cast<uint64_t>(1));
    CHECK(!Rigel::Voxel::detail::ChunkStreamerTestAccess::
        hasReplacementPendingMesh(streamer, replacementCoord));
    CHECK_EQ(
        Rigel::Voxel::detail::ChunkStreamerTestAccess::
            replacementPendingMeshCount(streamer),
        static_cast<size_t>(0));
    CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::
        pendingMeshIsPrioritized(streamer, replacementCoord));

    streamer.update(cameraCoord.toWorldCenter());
    const auto dispatchOrder =
        Rigel::Voxel::detail::ChunkStreamerTestAccess::
            inFlightMeshDispatchOrder(streamer);
    CHECK_EQ(dispatchOrder.size(), static_cast<size_t>(1));
    if (!dispatchOrder.empty()) {
        CHECK_EQ(dispatchOrder.front(), replacementCoord);
    }
    CHECK(waitForMeshCompletions(streamer, 2));
    CHECK_EQ(streamer.workMetrics().meshJobsAccepted,
             static_cast<uint64_t>(1));
    CHECK_EQ(streamer.workMetrics().meshJobsRejectedStale,
             static_cast<uint64_t>(1));

    streamer.update(cameraCoord.toWorldCenter());
    CHECK(waitForMeshCompletions(streamer, 3));
    CHECK_EQ(streamer.workMetrics().meshJobsAccepted,
             static_cast<uint64_t>(2));
    CHECK_EQ(streamer.workMetrics().meshJobsStarted,
             static_cast<uint64_t>(3));
}

TEST_CASE(ChunkStreamer_ConfigShrinkCannotRecreateObsoleteReplacement) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto originalGenerator = makeGenerator(registry);
    auto replacementGenerator = std::make_shared<WorldGenerator>(
        registry, originalGenerator->config());
    const BlockID solid = registerTestBlock(
        registry, "rigel:shrink_obsolete_replacement_solid");
    const ChunkCoord cameraCoord{0, 0, 0};
    const ChunkCoord retiringCoord{1, 0, 0};
    const std::array<ChunkCoord, 7> desired{
        cameraCoord,
        retiringCoord,
        ChunkCoord{-1, 0, 0},
        ChunkCoord{0, 1, 0},
        ChunkCoord{0, -1, 0},
        ChunkCoord{0, 0, 1},
        ChunkCoord{0, 0, -1}
    };
    for (const ChunkCoord& coord : desired) {
        Chunk& chunk = manager.getOrCreateChunk(coord);
        chunk.setWorldGenVersion(originalGenerator->config().world.version);
        chunk.setLoadedFromDisk(true);
        chunk.clearPersistDirty();
        chunk.clearDirty();
    }
    Chunk& retiring = *manager.getChunk(retiringCoord);
    retiring.setBlock(0, 0, 0, BlockState{solid}, registry);
    retiring.clearPersistDirty();
    retiring.clearDirty();
    meshStore.set(retiringCoord, {});
    retiring.invalidateMesh();

    auto gate = std::make_shared<WorkerGate>();
    ChunkStreamer streamer(
        manager, meshStore, registry, nullptr, originalGenerator);
    WorkerGateRelease releaseOnExit(gate);
    StreamingConfig stream;
    stream.viewDistanceChunks = 1;
    stream.unloadDistanceChunks = 1;
    stream.meshQueueLimit = 1;
    stream.workerThreads = 2;
    streamer.setConfig(stream);
    streamer.markSpawnDiscoveryComplete();
    streamer.prioritizeMesh(retiringCoord);
    Rigel::Voxel::detail::ChunkStreamerTestAccess::setMeshBuildStartCallback(
        streamer, [gate]() { gate->enterAndWait(); });

    streamer.update(cameraCoord.toWorldCenter());
    CHECK(gate->waitUntilEntered());
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(1));
    CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::desiredContains(
        streamer, retiringCoord));

    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 0;
    streamer.setConfig(stream);
    CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::desiredContains(
        streamer, retiringCoord));
    streamer.setGenerator(replacementGenerator);

    CHECK(!Rigel::Voxel::detail::ChunkStreamerTestAccess::
        hasReplacementPendingMesh(streamer, retiringCoord));
    CHECK(!Rigel::Voxel::detail::ChunkStreamerTestAccess::
        hasExplicitMeshPriority(streamer, retiringCoord));
    CHECK_EQ(
        Rigel::Voxel::detail::ChunkStreamerTestAccess::
            replacementPendingMeshCount(streamer),
        static_cast<size_t>(0));
    CHECK_EQ(streamer.diagnostics().mesh.pending, static_cast<size_t>(0));

    gate->release();
    CHECK(waitForMeshCompletions(streamer, 1));
    CHECK_EQ(streamer.workMetrics().meshJobsRejectedStale,
             static_cast<uint64_t>(1));
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(1));
    CHECK(streamer.diagnostics().mesh.empty());

    streamer.update(cameraCoord.toWorldCenter());
    CHECK(!Rigel::Voxel::detail::ChunkStreamerTestAccess::desiredContains(
        streamer, retiringCoord));
    for (uint32_t stable = 0;
         stable < StreamingDiagnosticSnapshot::QuiescenceUpdateWindow;
         ++stable) {
        streamer.update(cameraCoord.toWorldCenter());
        streamer.processCompletions();
    }
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(1));
    CHECK_EQ(streamer.diagnostics().state,
             StreamingLifecycleState::Quiescent);
}

TEST_CASE(ChunkStreamer_TerminalDataFailureRetiresExplicitMeshPriority) {
    enum class FailureKind {
        Load,
        Generation
    };

    for (const FailureKind failureKind :
         {FailureKind::Load, FailureKind::Generation}) {
        ChunkManager manager;
        BlockRegistry registry;
        WorldMeshStore meshStore;
        auto generator = makeGenerator(registry);
        const BlockID solid = registerTestBlock(
            registry,
            failureKind == FailureKind::Load
                ? "rigel:load_failure_priority_solid"
                : "rigel:generation_failure_priority_solid");
        const ChunkCoord failedCoord{0, 0, 0};
        const ChunkCoord genuinePriorityCoord{3, 0, 0};

        ChunkStreamer streamer(
            manager, meshStore, registry, nullptr, generator);
        StreamingConfig stream;
        stream.viewDistanceChunks = 0;
        stream.unloadDistanceChunks = 4;
        stream.genQueueLimit = 1;
        stream.meshQueueLimit = 1;
        stream.workerThreads = 2;
        streamer.setConfig(stream);
        streamer.markSpawnDiscoveryComplete();

        ChunkLoadRequestId failedLoadRequest = 0;
        bool loadCompletionPending = failureKind == FailureKind::Load;
        if (failureKind == FailureKind::Load) {
            streamer.setChunkLoader([&](ChunkLoadRequest request) {
                failedLoadRequest = request.requestId;
                return ChunkLoadRequestResult::Queued;
            });
            streamer.setChunkLoadDrain([&](size_t) {
                if (!loadCompletionPending) {
                    return std::vector<ChunkLoadCompletion>{};
                }
                loadCompletionPending = false;
                return std::vector<ChunkLoadCompletion>{
                    {failedCoord,
                     failedLoadRequest,
                     ChunkLoadOutcome::Failed,
                     "injected priority load failure"}
                };
            });
        } else {
            Rigel::Voxel::detail::ChunkStreamerTestAccess::
                setGenerationStartCallback(streamer, []() {
                    throw std::runtime_error(
                        "injected priority generation failure");
                });
        }

        streamer.update(failedCoord.toWorldCenter());
        streamer.prioritizeMesh(failedCoord);
        CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::
            hasExplicitMeshPriority(streamer, failedCoord));
        if (failureKind == FailureKind::Generation) {
            CHECK(waitForGenerationCompletion(streamer));
        }
        streamer.processCompletions();

        CHECK(!Rigel::Voxel::detail::ChunkStreamerTestAccess::
            hasExplicitMeshPriority(streamer, failedCoord));
        CHECK_EQ(streamer.diagnostics().mesh.pending, static_cast<size_t>(0));

        Rigel::Voxel::detail::ChunkStreamerTestAccess::
            setGenerationStartCallback(streamer, {});
        Chunk& recovered = manager.getOrCreateChunk(failedCoord);
        recovered.setBlock(0, 0, 0, BlockState{solid}, registry);
        recovered.setWorldGenVersion(generator->config().world.version);
        recovered.setLoadedFromDisk(true);
        recovered.clearPersistDirty();

        Chunk& genuine = manager.getOrCreateChunk(genuinePriorityCoord);
        genuine.setBlock(0, 0, 0, BlockState{solid}, registry);
        genuine.setWorldGenVersion(generator->config().world.version);
        genuine.setLoadedFromDisk(true);
        genuine.clearPersistDirty();
        genuine.clearDirty();
        meshStore.set(genuinePriorityCoord, {});
        genuine.invalidateMesh();

        auto gate = std::make_shared<WorkerGate>();
        WorkerGateRelease releaseOnExit(gate);
        Rigel::Voxel::detail::ChunkStreamerTestAccess::
            setMeshBuildStartCallback(
                streamer, [gate]() { gate->enterAndWait(); });
        streamer.prioritizeMesh(genuinePriorityCoord);
        streamer.update(failedCoord.toWorldCenter());
        CHECK(gate->waitUntilEntered());
        const auto firstDispatch =
            Rigel::Voxel::detail::ChunkStreamerTestAccess::
                inFlightMeshDispatchOrder(streamer);
        CHECK_EQ(firstDispatch.size(), static_cast<size_t>(1));
        if (!firstDispatch.empty()) {
            CHECK_EQ(firstDispatch.front(), genuinePriorityCoord);
        }

        gate->release();
        CHECK(waitForMeshCompletions(streamer, 1));
        streamer.update(failedCoord.toWorldCenter());
        CHECK(waitForMeshCompletions(streamer, 2));
        CHECK_EQ(streamer.workMetrics().meshJobsAccepted,
                 static_cast<uint64_t>(2));
        CHECK(!Rigel::Voxel::detail::ChunkStreamerTestAccess::
            hasExplicitMeshPriority(streamer, failedCoord));
        CHECK(!Rigel::Voxel::detail::ChunkStreamerTestAccess::
            hasExplicitMeshPriority(streamer, genuinePriorityCoord));
        CHECK(streamer.diagnostics().mesh.empty());
    }
}

TEST_CASE(ChunkStreamer_ReplacementRetirementPreventsLateWake) {
    enum class RetirementKind {
        Eviction,
        ConfigShrink,
        Reset
    };

    for (const RetirementKind retirement :
         {RetirementKind::Eviction,
          RetirementKind::ConfigShrink,
          RetirementKind::Reset}) {
        ChunkManager manager;
        BlockRegistry registry;
        WorldMeshStore meshStore;
        auto originalGenerator = makeGenerator(registry);
        auto replacementGenerator = std::make_shared<WorldGenerator>(
            registry, originalGenerator->config());
        const BlockID solid = registerTestBlock(
            registry, "rigel:replacement_retirement_solid");
        const ChunkCoord cameraCoord{0, 0, 0};
        const ChunkCoord coord = retirement == RetirementKind::ConfigShrink
            ? ChunkCoord{1, 0, 0}
            : cameraCoord;

        Chunk& chunk = manager.getOrCreateChunk(coord);
        chunk.setBlock(0, 0, 0, BlockState{solid}, registry);
        chunk.setWorldGenVersion(originalGenerator->config().world.version);
        chunk.setLoadedFromDisk(true);
        chunk.clearPersistDirty();
        chunk.clearDirty();
        meshStore.set(coord, {});
        chunk.invalidateMesh();
        if (coord != cameraCoord) {
            Chunk& camera = manager.getOrCreateChunk(cameraCoord);
            camera.setWorldGenVersion(
                originalGenerator->config().world.version);
            camera.setLoadedFromDisk(true);
            camera.clearPersistDirty();
            camera.clearDirty();
        }

        auto gate = std::make_shared<WorkerGate>();
        ChunkStreamer streamer(
            manager, meshStore, registry, nullptr, originalGenerator);
        WorkerGateRelease releaseOnExit(gate);
        StreamingConfig stream;
        stream.viewDistanceChunks =
            retirement == RetirementKind::ConfigShrink ? 1 : 0;
        stream.unloadDistanceChunks = stream.viewDistanceChunks;
        stream.meshQueueLimit = 1;
        stream.workerThreads = 2;
        streamer.setConfig(stream);
        streamer.prioritizeMesh(coord);
        Rigel::Voxel::detail::ChunkStreamerTestAccess::setMeshBuildStartCallback(
            streamer, [gate]() { gate->enterAndWait(); });

        streamer.update(cameraCoord.toWorldCenter());
        CHECK(gate->waitUntilEntered());
        streamer.setGenerator(replacementGenerator);
        CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::
            hasReplacementPendingMesh(streamer, coord));
        CHECK_EQ(
            Rigel::Voxel::detail::ChunkStreamerTestAccess::
                replacementPendingMeshCount(streamer),
            static_cast<size_t>(1));
        CHECK_EQ(streamer.diagnostics().mesh.pending, static_cast<size_t>(1));
        CHECK_EQ(streamer.diagnostics().mesh.inFlight, static_cast<size_t>(1));

        if (retirement == RetirementKind::Eviction) {
            CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::evictChunk(
                streamer, coord));
            Rigel::Voxel::detail::ChunkStreamerTestAccess::refreshDiagnostics(
                streamer);
        } else if (retirement == RetirementKind::ConfigShrink) {
            stream.viewDistanceChunks = 0;
            stream.unloadDistanceChunks = 0;
            streamer.setConfig(stream);
        } else {
            Rigel::Voxel::detail::ChunkStreamerTestAccess::reset(streamer);
        }

        CHECK(!Rigel::Voxel::detail::ChunkStreamerTestAccess::
            hasReplacementPendingMesh(streamer, coord));
        CHECK(!Rigel::Voxel::detail::ChunkStreamerTestAccess::
            hasExplicitMeshPriority(streamer, coord));
        CHECK_EQ(
            Rigel::Voxel::detail::ChunkStreamerTestAccess::
                replacementPendingMeshCount(streamer),
            static_cast<size_t>(0));
        CHECK_EQ(streamer.diagnostics().mesh.pending, static_cast<size_t>(0));
        CHECK_EQ(streamer.diagnostics().mesh.inFlight, static_cast<size_t>(1));

        gate->release();
        CHECK(waitForMeshCompletions(streamer, 1));
        CHECK_EQ(streamer.workMetrics().meshJobsStarted,
                 static_cast<uint64_t>(1));
        CHECK_EQ(streamer.workMetrics().meshJobsAccepted,
                 static_cast<uint64_t>(0));
        CHECK_EQ(streamer.workMetrics().meshJobsRejectedStale,
                 static_cast<uint64_t>(1));
        CHECK(streamer.diagnostics().mesh.empty());
    }
}

TEST_CASE(ChunkStreamer_CameraDepartureRetiresObsoleteReplacementIdentity) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto originalGenerator = makeGenerator(registry);
    auto replacementGenerator = std::make_shared<WorldGenerator>(
        registry, originalGenerator->config());
    const BlockID solid = registerTestBlock(
        registry, "rigel:departed_replacement_identity_solid");
    const ChunkCoord coord{0, 4, 0};
    const ChunkCoord departedCamera{4, 4, 0};

    Chunk& original = manager.getOrCreateChunk(coord);
    original.setBlock(0, 0, 0, BlockState{solid}, registry);
    original.setWorldGenVersion(originalGenerator->config().world.version);
    original.setLoadedFromDisk(true);
    original.clearPersistDirty();
    original.clearDirty();
    meshStore.set(coord, {});
    original.invalidateMesh();

    auto gate = std::make_shared<WorkerGate>();
    ChunkStreamer streamer(
        manager, meshStore, registry, nullptr, originalGenerator);
    WorkerGateRelease releaseOnExit(gate);
    StreamingConfig stream;
    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 1;
    stream.meshQueueLimit = 1;
    stream.workerThreads = 2;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    streamer.markSpawnDiscoveryComplete();
    streamer.prioritizeMesh(coord);
    std::atomic<size_t> buildsEntered{0};
    Rigel::Voxel::detail::ChunkStreamerTestAccess::setMeshBuildStartCallback(
        streamer,
        [gate, &buildsEntered]() {
            if (buildsEntered.fetch_add(1, std::memory_order_relaxed) == 0) {
                gate->enterAndWait();
            }
        });

    streamer.update(coord.toWorldCenter());
    CHECK(gate->waitUntilEntered());
    const auto oldRequestId =
        Rigel::Voxel::detail::ChunkStreamerTestAccess::inFlightMeshRequestId(
            streamer, coord);
    CHECK(oldRequestId.has_value());
    CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::
        inFlightMeshIsPrioritized(streamer, coord));

    streamer.setGenerator(replacementGenerator);
    CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::
        hasReplacementPendingMesh(streamer, coord));
    CHECK_EQ(
        Rigel::Voxel::detail::ChunkStreamerTestAccess::
            replacementPendingMeshCount(streamer),
        static_cast<size_t>(1));

    Chunk& camera = manager.getOrCreateChunk(departedCamera);
    camera.setWorldGenVersion(replacementGenerator->config().world.version);
    camera.setLoadedFromDisk(true);
    camera.clearPersistDirty();
    camera.clearDirty();
    streamer.update(departedCamera.toWorldCenter());

    CHECK(!manager.hasChunk(coord));
    CHECK(!meshStore.contains(coord));
    CHECK(!Rigel::Voxel::detail::ChunkStreamerTestAccess::
        hasReplacementPendingMesh(streamer, coord));
    CHECK(!Rigel::Voxel::detail::ChunkStreamerTestAccess::
        hasExplicitMeshPriority(streamer, coord));
    CHECK(!Rigel::Voxel::detail::ChunkStreamerTestAccess::
        inFlightMeshIsPrioritized(streamer, coord));
    CHECK_EQ(
        Rigel::Voxel::detail::ChunkStreamerTestAccess::
            replacementPendingMeshCount(streamer),
        static_cast<size_t>(0));
    CHECK_EQ(
        Rigel::Voxel::detail::ChunkStreamerTestAccess::inFlightMeshRequestId(
            streamer, coord),
        oldRequestId);

    gate->release();
    CHECK(waitForMeshCompletions(streamer, 1));
    CHECK_EQ(streamer.workMetrics().meshJobsStarted,
             static_cast<uint64_t>(1));
    CHECK_EQ(streamer.workMetrics().meshJobsAccepted,
             static_cast<uint64_t>(0));
    CHECK_EQ(streamer.workMetrics().meshJobsRejectedStale,
             static_cast<uint64_t>(1));
    CHECK(!Rigel::Voxel::detail::ChunkStreamerTestAccess::
        hasReadyPendingMesh(streamer, coord));
    CHECK(!Rigel::Voxel::detail::ChunkStreamerTestAccess::
        hasDependencyPendingMesh(streamer, coord));
    const auto lateWakeOrder =
        Rigel::Voxel::detail::ChunkStreamerTestAccess::pendingLoadGenOrder(
            streamer);
    CHECK(std::find(lateWakeOrder.begin(), lateWakeOrder.end(), coord) ==
          lateWakeOrder.end());
    CHECK(streamer.diagnostics().mesh.empty());

    Chunk& incarnation = manager.getOrCreateChunk(coord);
    incarnation.setBlock(0, 0, 0, BlockState{solid}, registry);
    incarnation.setWorldGenVersion(
        replacementGenerator->config().world.version);
    incarnation.setLoadedFromDisk(true);
    incarnation.clearPersistDirty();
    incarnation.clearDirty();
    streamer.update(coord.toWorldCenter());

    const auto currentRequestId =
        Rigel::Voxel::detail::ChunkStreamerTestAccess::inFlightMeshRequestId(
            streamer, coord);
    CHECK(currentRequestId.has_value());
    CHECK_NE(currentRequestId, oldRequestId);
    CHECK(!Rigel::Voxel::detail::ChunkStreamerTestAccess::
        inFlightMeshIsPrioritized(streamer, coord));
    CHECK(!Rigel::Voxel::detail::ChunkStreamerTestAccess::
        hasExplicitMeshPriority(streamer, coord));
    CHECK(waitForMeshCompletions(streamer, 2));

    CHECK_EQ(streamer.workMetrics().meshJobsStarted,
             static_cast<uint64_t>(2));
    CHECK_EQ(streamer.workMetrics().meshJobsAccepted,
             static_cast<uint64_t>(1));
    CHECK_EQ(streamer.workMetrics().meshJobsRejectedStale,
             static_cast<uint64_t>(1));
    CHECK(meshStore.contains(coord));
    CHECK(streamer.diagnostics().mesh.empty());

    for (uint32_t stable = 0;
         stable < StreamingDiagnosticSnapshot::QuiescenceUpdateWindow;
         ++stable) {
        streamer.update(coord.toWorldCenter());
        streamer.processCompletions();
    }
    CHECK_EQ(streamer.workMetrics().meshJobsStarted,
             static_cast<uint64_t>(2));
    CHECK_EQ(streamer.diagnostics().state,
             StreamingLifecycleState::Quiescent);
}

TEST_CASE(ChunkStreamer_VersionRegenerationDoesNotRecoverOldMeshPriority) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto originalGenerator = makeGenerator(registry);
    WorldGenConfig replacementConfig = originalGenerator->config();
    ++replacementConfig.world.version;
    replacementConfig.terrain.baseHeight = 64.0f;
    auto replacementGenerator =
        std::make_shared<WorldGenerator>(registry, replacementConfig);
    const BlockID solid = registerTestBlock(
        registry, "rigel:version_priority_retirement_solid");
    const ChunkCoord regeneratedCoord{0, 0, 0};
    const ChunkCoord explicitCurrentCoord{3, 0, 0};

    Chunk& oldData = manager.getOrCreateChunk(regeneratedCoord);
    oldData.setBlock(0, 0, 0, BlockState{solid}, registry);
    oldData.setWorldGenVersion(originalGenerator->config().world.version);
    oldData.setLoadedFromDisk(true);
    oldData.clearPersistDirty();
    oldData.clearDirty();
    meshStore.set(regeneratedCoord, {});
    oldData.invalidateMesh();

    auto oldMeshGate = std::make_shared<WorkerGate>();
    ChunkStreamer streamer(
        manager, meshStore, registry, nullptr, originalGenerator);
    WorkerGateRelease releaseOnExit(oldMeshGate);
    StreamingConfig stream;
    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 4;
    stream.genQueueLimit = 1;
    stream.meshQueueLimit = 1;
    stream.updateBudgetPerFrame = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 2;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    streamer.markSpawnDiscoveryComplete();
    streamer.prioritizeMesh(regeneratedCoord);

    std::atomic<size_t> buildsEntered{0};
    Rigel::Voxel::detail::ChunkStreamerTestAccess::setMeshBuildStartCallback(
        streamer,
        [oldMeshGate, &buildsEntered]() {
            if (buildsEntered.fetch_add(1, std::memory_order_relaxed) == 0) {
                oldMeshGate->enterAndWait();
            }
        });
    streamer.update(regeneratedCoord.toWorldCenter());
    CHECK(oldMeshGate->waitUntilEntered());
    CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::
        inFlightMeshIsPrioritized(streamer, regeneratedCoord));

    streamer.setGenerator(replacementGenerator);
    CHECK(!Rigel::Voxel::detail::ChunkStreamerTestAccess::
        hasExplicitMeshPriority(streamer, regeneratedCoord));
    CHECK(!Rigel::Voxel::detail::ChunkStreamerTestAccess::
        hasReplacementPendingMesh(streamer, regeneratedCoord));
    CHECK(!Rigel::Voxel::detail::ChunkStreamerTestAccess::
        inFlightMeshIsPrioritized(streamer, regeneratedCoord));

    Chunk& explicitCurrent =
        manager.getOrCreateChunk(explicitCurrentCoord);
    explicitCurrent.setBlock(0, 0, 0, BlockState{solid}, registry);
    explicitCurrent.setWorldGenVersion(replacementConfig.world.version);
    explicitCurrent.setLoadedFromDisk(true);
    explicitCurrent.clearPersistDirty();
    explicitCurrent.clearDirty();
    meshStore.set(explicitCurrentCoord, {});
    explicitCurrent.invalidateMesh();
    streamer.prioritizeMesh(explicitCurrentCoord);
    CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::
        hasExplicitMeshPriority(streamer, explicitCurrentCoord));

    streamer.update(regeneratedCoord.toWorldCenter());
    CHECK(waitForGenerationCompletion(streamer));
    streamer.processCompletions();
    Chunk* regenerated = manager.getChunk(regeneratedCoord);
    CHECK(regenerated != nullptr);
    if (!regenerated) {
        return;
    }
    CHECK_EQ(regenerated->worldGenVersion(), replacementConfig.world.version);
    CHECK(!regenerated->isEmpty());
    CHECK_EQ(streamer.workMetrics().generationJobsStarted,
             static_cast<uint64_t>(1));

    streamer.update(regeneratedCoord.toWorldCenter());
    CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::
        hasReplacementPendingMesh(streamer, regeneratedCoord));
    CHECK(!Rigel::Voxel::detail::ChunkStreamerTestAccess::
        inFlightMeshIsPrioritized(streamer, regeneratedCoord));
    CHECK_EQ(
        Rigel::Voxel::detail::ChunkStreamerTestAccess::
            replacementPendingMeshCount(streamer),
        static_cast<size_t>(1));

    oldMeshGate->release();
    CHECK(waitForMeshCompletions(streamer, 1));
    CHECK_EQ(streamer.workMetrics().meshJobsRejectedStale,
             static_cast<uint64_t>(1));
    CHECK(!Rigel::Voxel::detail::ChunkStreamerTestAccess::
        hasReadyPendingMesh(streamer, regeneratedCoord));
    const auto regenerationWakeOrder =
        Rigel::Voxel::detail::ChunkStreamerTestAccess::pendingLoadGenOrder(
            streamer);
    CHECK(std::find(
              regenerationWakeOrder.begin(),
              regenerationWakeOrder.end(),
              regeneratedCoord) != regenerationWakeOrder.end());
    CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::
        pendingMeshIsPrioritized(streamer, explicitCurrentCoord));

    streamer.update(regeneratedCoord.toWorldCenter());
    auto dispatchOrder =
        Rigel::Voxel::detail::ChunkStreamerTestAccess::
            inFlightMeshDispatchOrder(streamer);
    CHECK_EQ(dispatchOrder.size(), static_cast<size_t>(1));
    if (!dispatchOrder.empty()) {
        CHECK_EQ(dispatchOrder.front(), explicitCurrentCoord);
    }
    CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::
        hasReadyPendingMesh(streamer, regeneratedCoord));
    CHECK(!Rigel::Voxel::detail::ChunkStreamerTestAccess::
        pendingMeshIsPrioritized(streamer, regeneratedCoord));
    CHECK(waitForMeshCompletions(streamer, 2));

    streamer.update(regeneratedCoord.toWorldCenter());
    dispatchOrder =
        Rigel::Voxel::detail::ChunkStreamerTestAccess::
            inFlightMeshDispatchOrder(streamer);
    CHECK_EQ(dispatchOrder.size(), static_cast<size_t>(1));
    if (!dispatchOrder.empty()) {
        CHECK_EQ(dispatchOrder.front(), regeneratedCoord);
    }
    CHECK(!Rigel::Voxel::detail::ChunkStreamerTestAccess::
        inFlightMeshIsPrioritized(streamer, regeneratedCoord));
    CHECK(waitForMeshCompletions(streamer, 3));

    CHECK_EQ(streamer.workMetrics().generationJobsStarted,
             static_cast<uint64_t>(1));
    CHECK_EQ(streamer.workMetrics().meshJobsStarted,
             static_cast<uint64_t>(3));
    CHECK_EQ(streamer.workMetrics().meshJobsCompleted,
             static_cast<uint64_t>(3));
    CHECK_EQ(streamer.workMetrics().meshJobsAccepted,
             static_cast<uint64_t>(2));
    CHECK_EQ(streamer.workMetrics().meshJobsRejectedStale,
             static_cast<uint64_t>(1));
    CHECK(!Rigel::Voxel::detail::ChunkStreamerTestAccess::
        hasExplicitMeshPriority(streamer, regeneratedCoord));
    CHECK(!Rigel::Voxel::detail::ChunkStreamerTestAccess::
        hasExplicitMeshPriority(streamer, explicitCurrentCoord));
    CHECK(meshStore.contains(regeneratedCoord));
    CHECK(meshStore.contains(explicitCurrentCoord));
    CHECK(installedMeshIndexCount(meshStore, regeneratedCoord) > 0);
    CHECK(!regenerated->isDirty());
    CHECK(streamer.diagnostics().mesh.empty());

    for (uint32_t stable = 0;
         stable < StreamingDiagnosticSnapshot::QuiescenceUpdateWindow;
         ++stable) {
        streamer.update(regeneratedCoord.toWorldCenter());
        streamer.processCompletions();
    }
    CHECK_EQ(streamer.workMetrics().meshJobsStarted,
             static_cast<uint64_t>(3));
    CHECK_EQ(streamer.diagnostics().state,
             StreamingLifecycleState::Quiescent);
}

TEST_CASE(ChunkStreamer_SameVersionReplacementRebuildsRetainedOwnersOnce) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto originalGenerator = makeGenerator(registry);
    auto replacementGenerator = std::make_shared<WorldGenerator>(
        registry, originalGenerator->config());
    const BlockID solid = registerTestBlock(
        registry, "rigel:retained_replacement_owner_solid");
    const ChunkCoord blockerCoord{-1, 0, 0};
    const ChunkCoord missingDependency{1, 0, 0};
    const ChunkCoord dependencyCoord{2, 0, 0};
    const ChunkCoord readyCoord{6, 0, 0};

    const std::array<ChunkCoord, 6> loadedDesired{
        ChunkCoord{0, 0, 0},
        blockerCoord,
        ChunkCoord{0, 1, 0},
        ChunkCoord{0, -1, 0},
        ChunkCoord{0, 0, 1},
        ChunkCoord{0, 0, -1}
    };
    for (const ChunkCoord& coord : loadedDesired) {
        Chunk& chunk = manager.getOrCreateChunk(coord);
        chunk.setWorldGenVersion(originalGenerator->config().world.version);
        chunk.setLoadedFromDisk(true);
        chunk.clearPersistDirty();
        chunk.clearDirty();
        if (coord == blockerCoord) {
            chunk.setBlock(0, 0, 0, BlockState{solid}, registry);
            chunk.clearPersistDirty();
            chunk.clearDirty();
            meshStore.set(coord, {});
            chunk.invalidateMesh();
        }
    }
    for (const ChunkCoord& coord : {dependencyCoord, readyCoord}) {
        Chunk& chunk = manager.getOrCreateChunk(coord);
        chunk.setBlock(0, 0, 0, BlockState{solid}, registry);
        chunk.setWorldGenVersion(originalGenerator->config().world.version);
        chunk.setLoadedFromDisk(true);
        chunk.clearPersistDirty();
        chunk.clearDirty();
        meshStore.set(coord, {});
        chunk.invalidateMesh();
    }

    auto gate = std::make_shared<WorkerGate>();
    ChunkStreamer streamer(
        manager, meshStore, registry, nullptr, originalGenerator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 1;
    stream.unloadDistanceChunks = 8;
    stream.meshQueueLimit = 1;
    stream.workerThreads = 2;
    streamer.setConfig(stream);
    streamer.markSpawnDiscoveryComplete();
    streamer.setChunkLoader([missingDependency](ChunkLoadRequest request) {
        return request.coord == missingDependency
            ? ChunkLoadRequestResult::Queued
            : ChunkLoadRequestResult::Missing;
    });
    streamer.prioritizeMesh(blockerCoord);
    Rigel::Voxel::detail::ChunkStreamerTestAccess::setMeshBuildStartCallback(
        streamer, [gate]() { gate->enterAndWait(); });
    WorkerGateRelease releaseOnExit(gate);

    streamer.update(glm::vec3(0.0f));
    CHECK(gate->waitUntilEntered());
    CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::
        hasDependencyPendingMesh(streamer, dependencyCoord));
    CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::hasReadyPendingMesh(
        streamer, readyCoord));
    streamer.prioritizeMesh(dependencyCoord);
    CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::
        hasExplicitMeshPriority(streamer, dependencyCoord));
    const auto oldReadySequence =
        Rigel::Voxel::detail::ChunkStreamerTestAccess::pendingMeshSequence(
            streamer, readyCoord);
    CHECK(oldReadySequence.has_value());

    streamer.setGenerator(replacementGenerator);

    const auto replacementReadySequence =
        Rigel::Voxel::detail::ChunkStreamerTestAccess::pendingMeshSequence(
            streamer, readyCoord);
    CHECK(replacementReadySequence.has_value());
    CHECK_NE(replacementReadySequence, oldReadySequence);
    CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::
        hasDependencyPendingMesh(streamer, dependencyCoord));
    CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::
        hasExplicitMeshPriority(streamer, dependencyCoord));
    CHECK_EQ(
        Rigel::Voxel::detail::ChunkStreamerTestAccess::
            readyPendingMeshCount(streamer),
        static_cast<size_t>(1));
    CHECK_EQ(
        Rigel::Voxel::detail::ChunkStreamerTestAccess::
            pendingMeshQueueRecordCount(streamer),
        static_cast<size_t>(1));
    CHECK_EQ(
        Rigel::Voxel::detail::ChunkStreamerTestAccess::
            replacementPendingMeshCount(streamer),
        static_cast<size_t>(1));
    CHECK_EQ(streamer.diagnostics().mesh.pending, static_cast<size_t>(3));

    gate->release();
    CHECK(waitForMeshCompletions(streamer, 1));

    Chunk& resolvedDependency = manager.getOrCreateChunk(missingDependency);
    resolvedDependency.setWorldGenVersion(
        replacementGenerator->config().world.version);
    resolvedDependency.setLoadedFromDisk(true);
    resolvedDependency.clearPersistDirty();
    resolvedDependency.clearDirty();

    streamer.update(glm::vec3(0.0f));
    auto dispatchOrder =
        Rigel::Voxel::detail::ChunkStreamerTestAccess::
            inFlightMeshDispatchOrder(streamer);
    CHECK_EQ(dispatchOrder.size(), static_cast<size_t>(1));
    if (!dispatchOrder.empty()) {
        CHECK_EQ(dispatchOrder.front(), blockerCoord);
    }
    CHECK(waitForMeshCompletions(streamer, 2));

    streamer.update(glm::vec3(0.0f));
    dispatchOrder = Rigel::Voxel::detail::ChunkStreamerTestAccess::
        inFlightMeshDispatchOrder(streamer);
    CHECK_EQ(dispatchOrder.size(), static_cast<size_t>(1));
    if (!dispatchOrder.empty()) {
        CHECK_EQ(dispatchOrder.front(), dependencyCoord);
    }
    CHECK(waitForMeshCompletions(streamer, 3));

    streamer.update(glm::vec3(0.0f));
    dispatchOrder = Rigel::Voxel::detail::ChunkStreamerTestAccess::
        inFlightMeshDispatchOrder(streamer);
    CHECK_EQ(dispatchOrder.size(), static_cast<size_t>(1));
    if (!dispatchOrder.empty()) {
        CHECK_EQ(dispatchOrder.front(), readyCoord);
    }
    CHECK(waitForMeshCompletions(streamer, 4));

    CHECK_EQ(streamer.workMetrics().meshJobsStarted,
             static_cast<uint64_t>(4));
    CHECK_EQ(streamer.workMetrics().meshJobsAccepted,
             static_cast<uint64_t>(3));
    CHECK_EQ(streamer.workMetrics().meshJobsRejectedStale,
             static_cast<uint64_t>(1));
    CHECK(!manager.getChunk(blockerCoord)->isDirty());
    CHECK(!manager.getChunk(dependencyCoord)->isDirty());
    CHECK(!manager.getChunk(readyCoord)->isDirty());
    CHECK(meshStore.contains(blockerCoord));
    CHECK(meshStore.contains(dependencyCoord));
    CHECK(meshStore.contains(readyCoord));
    CHECK(streamer.diagnostics().mesh.empty());

    for (uint32_t stable = 0;
         stable < StreamingDiagnosticSnapshot::QuiescenceUpdateWindow;
         ++stable) {
        streamer.update(glm::vec3(0.0f));
        streamer.processCompletions();
    }
    CHECK_EQ(streamer.workMetrics().meshJobsStarted,
             static_cast<uint64_t>(4));
    CHECK_EQ(streamer.workMetrics().lastUpdateSchedulerCoordinatesInspected,
             static_cast<uint64_t>(0));
    CHECK_EQ(streamer.diagnostics().state,
             StreamingLifecycleState::Quiescent);
}

TEST_CASE(ChunkStreamer_InlineGeneratorReplacementSettlesExactlyOnce) {
    for (const int workerThreads : {0, 1}) {
        ChunkManager manager;
        BlockRegistry registry;
        WorldMeshStore meshStore;
        auto originalGenerator = makeGenerator(registry);
        auto replacementGenerator = std::make_shared<WorldGenerator>(
            registry, originalGenerator->config());
        const BlockID solid = registerTestBlock(
            registry,
            workerThreads == 0
                ? "rigel:inline_replacement_zero_solid"
                : "rigel:inline_replacement_one_solid");
        const ChunkCoord coord{0, 0, 0};

        Chunk& chunk = manager.getOrCreateChunk(coord);
        chunk.setBlock(0, 0, 0, BlockState{solid}, registry);
        chunk.setWorldGenVersion(originalGenerator->config().world.version);
        chunk.setLoadedFromDisk(true);
        chunk.clearPersistDirty();

        ChunkStreamer streamer(
            manager, meshStore, registry, nullptr, originalGenerator);
        StreamingConfig stream;
        stream.viewDistanceChunks = 0;
        stream.unloadDistanceChunks = 0;
        stream.meshQueueLimit = 1;
        stream.workerThreads = workerThreads;
        streamer.setConfig(stream);
        streamer.markSpawnDiscoveryComplete();

        streamer.update(coord.toWorldCenter());
        CHECK_EQ(streamer.workMetrics().meshJobsStarted,
                 static_cast<uint64_t>(1));
        CHECK_EQ(streamer.diagnostics().mesh.inFlight,
                 static_cast<size_t>(1));

        streamer.setGenerator(replacementGenerator);
        CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::
            hasReplacementPendingMesh(streamer, coord));
        CHECK_EQ(streamer.diagnostics().mesh.pending, static_cast<size_t>(1));
        CHECK_EQ(streamer.diagnostics().mesh.inFlight, static_cast<size_t>(1));

        streamer.processCompletions();
        CHECK_EQ(streamer.workMetrics().meshJobsRejectedStale,
                 static_cast<uint64_t>(1));
        CHECK_EQ(
            Rigel::Voxel::detail::ChunkStreamerTestAccess::
                replacementPendingMeshCount(streamer),
            static_cast<size_t>(0));
        streamer.update(coord.toWorldCenter());
        streamer.processCompletions();

        CHECK_EQ(streamer.workMetrics().meshJobsStarted,
                 static_cast<uint64_t>(2));
        CHECK_EQ(streamer.workMetrics().meshJobsAccepted,
                 static_cast<uint64_t>(1));
        CHECK_EQ(streamer.workMetrics().meshJobsRejectedStale,
                 static_cast<uint64_t>(1));
        CHECK(streamer.diagnostics().mesh.empty());

        for (uint32_t stable = 1;
             stable <= StreamingDiagnosticSnapshot::QuiescenceUpdateWindow;
             ++stable) {
            streamer.update(coord.toWorldCenter());
            streamer.processCompletions();
        }
        CHECK_EQ(streamer.workMetrics().meshJobsStarted,
                 static_cast<uint64_t>(2));
        CHECK_EQ(streamer.workMetrics().lastUpdateSchedulerCoordinatesInspected,
                 static_cast<uint64_t>(0));
        CHECK_EQ(streamer.diagnostics().state,
                 StreamingLifecycleState::Quiescent);
    }
}

TEST_CASE(ChunkStreamer_VoxelEmptyRetiresObsoleteReplacementImmediately) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto originalGenerator = makeGenerator(registry);
    auto replacementGenerator = std::make_shared<WorldGenerator>(
        registry, originalGenerator->config());
    const BlockID solid = registerTestBlock(
        registry, "rigel:empty_obsolete_replacement_solid");
    const ChunkCoord coord{0, 0, 0};

    Chunk& chunk = manager.getOrCreateChunk(coord);
    chunk.setBlock(0, 0, 0, BlockState{solid}, registry);
    chunk.setWorldGenVersion(originalGenerator->config().world.version);
    chunk.setLoadedFromDisk(true);
    chunk.clearPersistDirty();

    ChunkStreamer streamer(
        manager, meshStore, registry, nullptr, originalGenerator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 0;
    stream.meshQueueLimit = 1;
    stream.workerThreads = 0;
    streamer.setConfig(stream);
    streamer.markSpawnDiscoveryComplete();

    streamer.update(coord.toWorldCenter());
    CHECK_EQ(streamer.diagnostics().mesh.inFlight, static_cast<size_t>(1));
    streamer.setGenerator(replacementGenerator);
    CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::
        hasReplacementPendingMesh(streamer, coord));
    CHECK_EQ(streamer.diagnostics().mesh.pending, static_cast<size_t>(1));

    chunk.setBlock(0, 0, 0, BlockState{}, registry);
    chunk.clearPersistDirty();
    streamer.update(coord.toWorldCenter());
    CHECK(!Rigel::Voxel::detail::ChunkStreamerTestAccess::
        hasReplacementPendingMesh(streamer, coord));
    CHECK_EQ(
        Rigel::Voxel::detail::ChunkStreamerTestAccess::
            replacementPendingMeshCount(streamer),
        static_cast<size_t>(0));
    CHECK_EQ(streamer.diagnostics().mesh.pending, static_cast<size_t>(0));
    CHECK_EQ(streamer.diagnostics().mesh.inFlight, static_cast<size_t>(1));

    streamer.processCompletions();
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(1));
    CHECK_EQ(streamer.workMetrics().meshJobsAccepted, static_cast<uint64_t>(0));
    CHECK_EQ(streamer.workMetrics().meshJobsRejectedStale,
             static_cast<uint64_t>(1));
    CHECK(streamer.diagnostics().mesh.empty());
    for (uint32_t stable = 0;
         stable < StreamingDiagnosticSnapshot::QuiescenceUpdateWindow;
         ++stable) {
        streamer.update(coord.toWorldCenter());
        streamer.processCompletions();
    }
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(1));
    CHECK_EQ(streamer.diagnostics().state,
             StreamingLifecycleState::Quiescent);
}

TEST_CASE(ChunkStreamer_GeneratorReplacementRetiresPendingMeshesBeforeDispatch) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto originalGenerator = makeGenerator(registry);
    WorldGenConfig replacementConfig = originalGenerator->config();
    ++replacementConfig.world.version;
    replacementConfig.terrain.baseHeight = 64.0f;
    auto replacementGenerator =
        std::make_shared<WorldGenerator>(registry, replacementConfig);
    const BlockID solid = registerTestBlock(
        registry, "rigel:pending_generator_replacement_solid");
    const ChunkCoord cameraCoord{0, 0, 0};
    const ChunkCoord oldPendingCoord{10, 0, 0};

    auto loadFaceNeighborhood = [&](ChunkCoord center) {
        Chunk& centerChunk = manager.getOrCreateChunk(center);
        centerChunk.setWorldGenVersion(
            originalGenerator->config().world.version);
        centerChunk.setLoadedFromDisk(true);
        centerChunk.clearPersistDirty();
        centerChunk.clearDirty();
        for (int i = 0; i < DirectionCount; ++i) {
            int dx = 0;
            int dy = 0;
            int dz = 0;
            directionOffset(static_cast<Direction>(i), dx, dy, dz);
            Chunk& neighbor = manager.getOrCreateChunk(
                center.offset(dx, dy, dz));
            neighbor.setWorldGenVersion(
                originalGenerator->config().world.version);
            neighbor.setLoadedFromDisk(true);
            neighbor.clearPersistDirty();
            neighbor.clearDirty();
        }
    };
    loadFaceNeighborhood(cameraCoord);
    loadFaceNeighborhood(oldPendingCoord);

    Chunk& cameraChunk = *manager.getChunk(cameraCoord);
    cameraChunk.setBlock(0, 0, 0, BlockState{solid}, registry);
    cameraChunk.clearPersistDirty();
    cameraChunk.clearDirty();
    Chunk& oldPending = *manager.getChunk(oldPendingCoord);
    oldPending.setBlock(0, 0, 0, BlockState{solid}, registry);
    oldPending.clearPersistDirty();
    oldPending.clearDirty();
    meshStore.set(oldPendingCoord, {});
    oldPending.invalidateMesh();

    ChunkStreamer streamer(
        manager, meshStore, registry, nullptr, originalGenerator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 20;
    stream.genQueueLimit = 1;
    stream.meshQueueLimit = 1;
    stream.updateBudgetPerFrame = 0;
    stream.applyBudgetPerFrame = 1;
    stream.workerThreads = 4;
    streamer.setConfig(stream);

    auto originalGate = std::make_shared<WorkerGate>();
    auto replacementGate = std::make_shared<WorkerGate>();
    std::atomic<size_t> buildsEntered{0};
    Rigel::Voxel::detail::ChunkStreamerTestAccess::setMeshBuildStartCallback(
        streamer,
        [originalGate, replacementGate, &buildsEntered]() {
            const size_t buildIndex =
                buildsEntered.fetch_add(1, std::memory_order_relaxed);
            if (buildIndex == 0) {
                originalGate->enterAndWait();
            } else if (buildIndex == 1) {
                replacementGate->enterAndWait();
            }
        });
    WorkerGateRelease releaseOriginalOnExit(originalGate);
    WorkerGateRelease releaseReplacementOnExit(replacementGate);

    streamer.update(cameraCoord.toWorldCenter());
    CHECK(originalGate->waitUntilEntered());
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(1));
    CHECK_EQ(streamer.diagnostics().mesh.pending, static_cast<size_t>(1));
    CHECK(Rigel::Voxel::detail::ChunkStreamerTestAccess::hasReadyPendingMesh(
        streamer, oldPendingCoord));
    CHECK(!Rigel::Voxel::detail::ChunkStreamerTestAccess::
        hasDependencyPendingMesh(streamer, oldPendingCoord));

    stream.updateBudgetPerFrame = 1;
    streamer.setConfig(stream);
    streamer.setGenerator(replacementGenerator);
    CHECK(!Rigel::Voxel::detail::ChunkStreamerTestAccess::hasReadyPendingMesh(
        streamer, oldPendingCoord));
    CHECK(!Rigel::Voxel::detail::ChunkStreamerTestAccess::
        hasDependencyPendingMesh(streamer, oldPendingCoord));
    CHECK_EQ(
        Rigel::Voxel::detail::ChunkStreamerTestAccess::readyPendingMeshCount(
            streamer),
        static_cast<size_t>(0));
    originalGate->release();
    CHECK(waitForMeshCompletions(streamer, 1));
    CHECK_EQ(streamer.workMetrics().meshJobsRejectedStale,
             static_cast<uint64_t>(1));

    streamer.update(cameraCoord.toWorldCenter());
    CHECK(waitForGenerationCompletion(streamer));
    streamer.processCompletions();
    Chunk* replacementChunk = manager.getChunk(cameraCoord);
    CHECK(replacementChunk != nullptr);
    if (replacementChunk) {
        CHECK_EQ(replacementChunk->worldGenVersion(),
                 replacementGenerator->config().world.version);
    }

    streamer.update(cameraCoord.toWorldCenter());
    CHECK(replacementGate->waitUntilEntered());
    const auto replacementDispatch =
        Rigel::Voxel::detail::ChunkStreamerTestAccess::
            inFlightMeshDispatchOrder(streamer);
    CHECK_EQ(replacementDispatch.size(), static_cast<size_t>(1));
    if (!replacementDispatch.empty()) {
        CHECK_EQ(replacementDispatch.front(), cameraCoord);
    }
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(2));
    CHECK(oldPending.isDirty());

    replacementGate->release();
    CHECK(waitForMeshCompletions(streamer, 2));

    CHECK_EQ(streamer.workMetrics().meshJobsAccepted, static_cast<uint64_t>(1));
    CHECK_EQ(streamer.workMetrics().meshJobsRejectedStale,
             static_cast<uint64_t>(1));
    CHECK(meshStore.contains(cameraCoord));
    CHECK(meshStore.contains(oldPendingCoord));
    CHECK(oldPending.isDirty());
}

TEST_CASE(ChunkStreamer_ObsoleteDirtyFlightDoesNotBlockUnrelatedDirtyMesh) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto originalGenerator = makeGenerator(registry);
    auto replacementGenerator = std::make_shared<WorldGenerator>(
        registry, originalGenerator->config());
    const BlockID solid = registerTestBlock(
        registry, "rigel:obsolete_head_unrelated_solid");
    const ChunkCoord obsoleteCoord{0, 0, 0};
    const ChunkCoord unrelatedCoord{1, 0, 0};

    Chunk& obsolete = manager.getOrCreateChunk(obsoleteCoord);
    obsolete.setBlock(0, 0, 0, BlockState{solid}, registry);
    obsolete.setWorldGenVersion(originalGenerator->config().world.version);
    obsolete.setLoadedFromDisk(true);
    obsolete.clearPersistDirty();
    meshStore.set(obsoleteCoord, {});

    ChunkStreamer streamer(
        manager, meshStore, registry, nullptr, originalGenerator);
    StreamingConfig stream;
    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 1;
    stream.genQueueLimit = 0;
    stream.meshQueueLimit = 8;
    stream.updateBudgetPerFrame = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 4;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    streamer.markSpawnDiscoveryComplete();

    auto obsoleteGate = std::make_shared<WorkerGate>();
    auto unrelatedGate = std::make_shared<WorkerGate>();
    WorkerGateRelease releaseObsoleteOnExit(obsoleteGate);
    WorkerGateRelease releaseUnrelatedOnExit(unrelatedGate);
    std::atomic<size_t> buildsEntered{0};
    Rigel::Voxel::detail::ChunkStreamerTestAccess::setMeshBuildStartCallback(
        streamer,
        [obsoleteGate, unrelatedGate, &buildsEntered]() {
            const size_t buildIndex =
                buildsEntered.fetch_add(1, std::memory_order_relaxed);
            if (buildIndex == 0) {
                obsoleteGate->enterAndWait();
            } else if (buildIndex == 1) {
                unrelatedGate->enterAndWait();
            }
        });

    streamer.update(obsoleteCoord.toWorldCenter());
    CHECK(obsoleteGate->waitUntilEntered());
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(1));
    CHECK_EQ(streamer.diagnostics().mesh.inFlight, static_cast<size_t>(1));
    CHECK_EQ(
        Rigel::Voxel::detail::ChunkStreamerTestAccess::inFlightMeshDirty(streamer),
        static_cast<size_t>(1));
    CHECK_EQ(
        Rigel::Voxel::detail::ChunkStreamerTestAccess::inFlightMeshMissing(streamer),
        static_cast<size_t>(0));

    streamer.setGenerator(replacementGenerator);
    Chunk& unrelated = manager.getOrCreateChunk(unrelatedCoord);
    unrelated.setBlock(0, 0, 0, BlockState{solid}, registry);
    unrelated.setWorldGenVersion(replacementGenerator->config().world.version);
    unrelated.setLoadedFromDisk(true);
    unrelated.clearPersistDirty();
    meshStore.set(unrelatedCoord, {});

    streamer.update(obsoleteCoord.toWorldCenter());
    CHECK(unrelatedGate->waitUntilEntered());
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(2));
    CHECK_EQ(buildsEntered.load(std::memory_order_relaxed), static_cast<size_t>(2));
    CHECK_EQ(streamer.diagnostics().mesh.inFlight, static_cast<size_t>(2));
    CHECK_EQ(
        Rigel::Voxel::detail::ChunkStreamerTestAccess::inFlightMeshDirty(streamer),
        static_cast<size_t>(2));
    CHECK_EQ(
        Rigel::Voxel::detail::ChunkStreamerTestAccess::inFlightMeshMissing(streamer),
        static_cast<size_t>(0));
    CHECK_EQ(streamer.diagnostics().state, StreamingLifecycleState::Streaming);

    unrelatedGate->release();
    CHECK(waitForMeshCompletions(streamer, 1));
    CHECK_EQ(streamer.workMetrics().meshJobsAccepted, static_cast<uint64_t>(1));
    CHECK_EQ(streamer.workMetrics().meshJobsRejectedStale, static_cast<uint64_t>(0));
    CHECK_EQ(streamer.diagnostics().mesh.inFlight, static_cast<size_t>(1));
    CHECK_EQ(
        Rigel::Voxel::detail::ChunkStreamerTestAccess::inFlightMeshDirty(streamer),
        static_cast<size_t>(1));
    CHECK(!unrelated.isDirty());
    CHECK(obsolete.isDirty());
    CHECK(meshStore.contains(unrelatedCoord));
    CHECK_EQ(streamer.diagnostics().state, StreamingLifecycleState::Streaming);

    obsoleteGate->release();
    CHECK(waitForMeshCompletions(streamer, 2));
    CHECK_EQ(streamer.workMetrics().meshJobsRejectedStale, static_cast<uint64_t>(1));
    CHECK_EQ(streamer.diagnostics().mesh.inFlight, static_cast<size_t>(0));
    CHECK(obsolete.isDirty());

    streamer.update(obsoleteCoord.toWorldCenter());
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(3));
    CHECK(waitForMeshCompletions(streamer, 3));
    CHECK_EQ(streamer.workMetrics().meshJobsCompleted, static_cast<uint64_t>(3));
    CHECK_EQ(streamer.workMetrics().meshJobsAccepted, static_cast<uint64_t>(2));
    CHECK_EQ(streamer.workMetrics().meshJobsRejectedStale, static_cast<uint64_t>(1));
    CHECK_EQ(buildsEntered.load(std::memory_order_relaxed), static_cast<size_t>(3));
    CHECK_EQ(streamer.diagnostics().mesh.inFlight, static_cast<size_t>(0));
    CHECK_EQ(
        Rigel::Voxel::detail::ChunkStreamerTestAccess::inFlightMeshDirty(streamer),
        static_cast<size_t>(0));
    CHECK(!obsolete.isDirty());
    CHECK(meshStore.contains(obsoleteCoord));

    const uint64_t settledMeshJobs = streamer.workMetrics().meshJobsStarted;
    for (int update = 0; update < 5; ++update) {
        streamer.update(obsoleteCoord.toWorldCenter());
        streamer.processCompletions();
    }
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, static_cast<uint64_t>(3));
    CHECK_EQ(streamer.workMetrics().meshJobsStarted, settledMeshJobs);
    CHECK_EQ(streamer.workMetrics().lastUpdateDesiredBuildCoordinatesInspected,
             static_cast<uint64_t>(0));
    CHECK_EQ(streamer.workMetrics().lastUpdateSchedulerCoordinatesInspected,
             static_cast<uint64_t>(0));
    CHECK_EQ(streamer.diagnostics().state, StreamingLifecycleState::Quiescent);
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
    streamer.getDebugStates(states, coord, 0);
    CHECK_EQ(states.size(), static_cast<size_t>(1));
    CHECK_EQ(states.front().coord, coord);
    CHECK_EQ(states.front().state,
             ChunkStreamer::DebugState::TerminalFailure);
    CHECK_EQ(states.front().failure,
             ChunkStreamer::DebugFailure::Generation);

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
    streamer.getDebugStates(states, coord, 0);
    CHECK_EQ(states.size(), static_cast<size_t>(1));
    CHECK_EQ(states.front().coord, coord);
    CHECK_EQ(states.front().state,
             ChunkStreamer::DebugState::TerminalFailure);
    CHECK_EQ(states.front().failure,
             ChunkStreamer::DebugFailure::Generation);

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
    streamer.getDebugStates(states, coord, 0);
    CHECK_EQ(states.size(), static_cast<size_t>(1));
    CHECK_EQ(states.front().state,
             ChunkStreamer::DebugState::TerminalFailure);
    CHECK_EQ(states.front().failure,
             ChunkStreamer::DebugFailure::Generation);
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

            const uint64_t expectedJobs = static_cast<uint64_t>(arrived);
            CHECK_EQ(streamer.workMetrics().meshJobsStarted, expectedJobs);
            CHECK_EQ(
                meshStore.contains(centerCoord),
                arrived == arrivalOrder.size());
        }

        streamer.update(glm::vec3(0.0f));
        streamer.processCompletions();
        CHECK_EQ(streamer.workMetrics().meshJobsStarted,
                 static_cast<uint64_t>(arrivalOrder.size() + 1));
        CHECK(meshStore.contains(centerCoord));

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

    bool settledAllWork = false;
    for (int update = 0; update < 32; ++update) {
        streamer.update(glm::vec3(0.0f));
        streamer.processCompletions();
        if (streamer.diagnostics().workEmpty()) {
            settledAllWork = true;
            break;
        }
    }
    CHECK(settledAllWork);

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
    CHECK_EQ(quiescent.lastUpdateSchedulerCoordinatesInspected, static_cast<uint64_t>(2));
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
    RegionSchedulerDiagnosticSnapshot regionDiagnostics;
    regionDiagnostics.directOrigin.logicalAdmissions = 7;
    regionDiagnostics.directOrigin.poolSubmissions = 9;
    regionDiagnostics.directOrigin.admissionToWorkerStartNanoseconds = 11;
    regionDiagnostics.demandOwnedDispatchedUndrained = 1;
    streamer.setChunkLoadDiagnosticsCallback([&]() {
        return ChunkLoadDiagnosticSnapshot{
            .work = loadWork,
            .regionScheduler = regionDiagnostics
        };
    });

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
    CHECK_EQ(streamer.diagnostics().regionScheduler.directOrigin.logicalAdmissions,
             static_cast<uint64_t>(7));
    CHECK_EQ(streamer.diagnostics().regionScheduler.directOrigin.poolSubmissions,
             static_cast<uint64_t>(9));
    CHECK_EQ(
        streamer.diagnostics().regionScheduler.directOrigin
            .admissionToWorkerStartNanoseconds,
        static_cast<uint64_t>(11));
    CHECK_EQ(
        streamer.diagnostics().regionScheduler
            .demandOwnedDispatchedUndrained,
        static_cast<size_t>(1));

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
