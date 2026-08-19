#include "TestFramework.h"

#include "Rigel/Persistence/Backends/Memory/MemoryFormat.h"
#include "Rigel/Persistence/Backends/CR/CRChunkMapping.h"
#include "Rigel/Persistence/Backends/CR/CRFormat.h"
#include "Rigel/Persistence/ChunkSerializer.h"
#include "Rigel/Persistence/PersistenceService.h"
#include "Rigel/Persistence/Providers.h"
#include "Rigel/Persistence/Storage.h"
#include "Rigel/Voxel/ChunkStreamer.h"
#include "Rigel/Voxel/BlockType.h"

#include <chrono>
#include <filesystem>
#include <optional>
#include <random>

using namespace Rigel::Voxel;

namespace {
std::shared_ptr<WorldGenerator> makeGenerator(BlockRegistry& registry) {
    BlockType solid;
    solid.identifier = "rigel:stone";
    registry.registerBlock(solid.identifier, solid);

    BlockType surface;
    surface.identifier = "rigel:grass";
    registry.registerBlock(surface.identifier, surface);

    auto generator = std::make_shared<WorldGenerator>(registry);
    WorldGenConfig config;
    config.seed = 1;
    config.solidBlock = solid.identifier;
    config.surfaceBlock = surface.identifier;
    config.terrain.baseHeight = 0.0f;
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
}

TEST_CASE(ChunkStreamer_GeneratesSphere) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);

    ChunkStreamer streamer;
    WorldGenConfig::StreamConfig stream;
    stream.viewDistanceChunks = 1;
    stream.unloadDistanceChunks = 1;
    stream.genQueueLimit = 0;
    stream.meshQueueLimit = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 0;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    streamer.bind(&manager, &meshStore, &registry, nullptr, generator);

    streamer.update(glm::vec3(0.0f));
    streamer.processCompletions();
    CHECK_EQ(manager.loadedChunkCount(), static_cast<size_t>(7));
}

TEST_CASE(ChunkStreamer_RespectsQueueLimit) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);

    ChunkStreamer streamer;
    WorldGenConfig::StreamConfig stream;
    stream.viewDistanceChunks = 1;
    stream.unloadDistanceChunks = 1;
    stream.genQueueLimit = 2;
    stream.meshQueueLimit = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 0;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    streamer.bind(&manager, &meshStore, &registry, nullptr, generator);

    streamer.update(glm::vec3(0.0f));
    streamer.processCompletions();
    CHECK_EQ(manager.loadedChunkCount(), static_cast<size_t>(2));
}

TEST_CASE(ChunkStreamer_UpdateBudget_DoesNotStarveOuterChunks) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);

    ChunkStreamer streamer;
    WorldGenConfig::StreamConfig stream;
    stream.viewDistanceChunks = 2;
    stream.unloadDistanceChunks = 2;
    stream.genQueueLimit = 0;
    stream.meshQueueLimit = 0;
    stream.updateBudgetPerFrame = 1;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 0;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    streamer.bind(&manager, &meshStore, &registry, nullptr, generator);

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

    ChunkStreamer streamer;
    WorldGenConfig::StreamConfig stream;
    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 0;
    stream.genQueueLimit = 0;
    stream.meshQueueLimit = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 0;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    streamer.bind(&manager, &meshStore, &registry, nullptr, generator);

    streamer.update(glm::vec3(0.0f));
    streamer.processCompletions();
    CHECK_EQ(manager.loadedChunkCount(), static_cast<size_t>(1));

    streamer.update(glm::vec3(static_cast<float>(ChunkSize * 4), 0.0f, 0.0f));
    streamer.processCompletions();
    CHECK_EQ(manager.loadedChunkCount(), static_cast<size_t>(1));
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

    ChunkStreamer streamer;
    WorldGenConfig::StreamConfig stream;
    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 0;
    stream.genQueueLimit = 0;
    stream.meshQueueLimit = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 0;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    streamer.bind(&manager, &meshStore, &registry, nullptr, generator);

    streamer.setChunkLoader([&](ChunkCoord request) {
        if (request != coord) {
            return false;
        }
        Chunk& target = manager.getOrCreateChunk(request);
        Rigel::Persistence::applyChunkData(payload, target, registry);
        target.setWorldGenVersion(generator->config().world.version);
        target.clearPersistDirty();
        return true;
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

    ChunkStreamer streamer;
    WorldGenConfig::StreamConfig stream;
    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 0;
    stream.genQueueLimit = 0;
    stream.meshQueueLimit = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 0;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    streamer.bind(&manager, &meshStore, &registry, nullptr, generator);

    streamer.setChunkLoader([&](ChunkCoord request) {
        if (request != coord) {
            return false;
        }
        Chunk& target = manager.getOrCreateChunk(request);
        Rigel::Persistence::applyChunkData(payload, target, registry);
        target.setWorldGenVersion(generator->config().world.version);
        target.clearPersistDirty();
        return true;
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

    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("rigel_chunk_payload_test_" + std::to_string(now));
    std::filesystem::create_directories(root);

    auto storage = std::make_shared<Rigel::Persistence::FilesystemBackend>();
    Rigel::Persistence::PersistenceContext context;
    context.rootPath = root.string();
    context.preferredFormat = "memory";
    context.storage = storage;

    auto format = service.openFormat(context);
    Rigel::Persistence::RegionKey regionKey =
        format->regionLayout().regionForChunk(snapshot.key.zoneId, coord);
    Rigel::Persistence::ChunkRegionSnapshot region;
    region.key = regionKey;
    region.chunks.push_back(snapshot);
    format->chunkContainer().saveRegion(region);

    ChunkStreamer streamer;
    WorldGenConfig::StreamConfig stream;
    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 0;
    stream.genQueueLimit = 0;
    stream.meshQueueLimit = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 0;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    streamer.bind(&manager, &meshStore, &registry, nullptr, generator);

    streamer.setChunkLoader([&](ChunkCoord request) {
        Rigel::Persistence::ChunkRegionSnapshot loaded = service.loadRegion(regionKey, context);
        for (const auto& chunk : loaded.chunks) {
            if (chunk.key.x == request.x &&
                chunk.key.y == request.y &&
                chunk.key.z == request.z) {
                Chunk& target = manager.getOrCreateChunk(request);
                Rigel::Persistence::applyChunkData(chunk.data, target, registry);
                target.setWorldGenVersion(generator->config().world.version);
                target.clearPersistDirty();
                return true;
            }
        }
        return false;
    });

    streamer.update(coord.toWorldCenter());
    streamer.processCompletions();

    Chunk* loaded = manager.getChunk(coord);
    CHECK(loaded != nullptr);
    if (!loaded) {
        return;
    }
    verifyPayloadMatches(*loaded, payload);

    std::filesystem::remove_all(root);
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

    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("rigel_chunk_payload_random_test_" + std::to_string(now));
    std::filesystem::create_directories(root);

    auto storage = std::make_shared<Rigel::Persistence::FilesystemBackend>();
    Rigel::Persistence::PersistenceContext context;
    context.rootPath = root.string();
    context.preferredFormat = "memory";
    context.storage = storage;

    auto format = service.openFormat(context);
    Rigel::Persistence::RegionKey regionKey =
        format->regionLayout().regionForChunk(snapshot.key.zoneId, coord);
    Rigel::Persistence::ChunkRegionSnapshot region;
    region.key = regionKey;
    region.chunks.push_back(snapshot);
    format->chunkContainer().saveRegion(region);

    ChunkStreamer streamer;
    WorldGenConfig::StreamConfig stream;
    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 0;
    stream.genQueueLimit = 0;
    stream.meshQueueLimit = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 0;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    streamer.bind(&manager, &meshStore, &registry, nullptr, generator);

    streamer.setChunkLoader([&](ChunkCoord request) {
        Rigel::Persistence::ChunkRegionSnapshot loaded = service.loadRegion(regionKey, context);
        for (const auto& chunk : loaded.chunks) {
            if (chunk.key.x == request.x &&
                chunk.key.y == request.y &&
                chunk.key.z == request.z) {
                Chunk& target = manager.getOrCreateChunk(request);
                Rigel::Persistence::applyChunkData(chunk.data, target, registry);
                target.setWorldGenVersion(generator->config().world.version);
                target.clearPersistDirty();
                return true;
            }
        }
        return false;
    });

    streamer.update(coord.toWorldCenter());
    streamer.processCompletions();

    Chunk* loaded = manager.getChunk(coord);
    CHECK(loaded != nullptr);
    if (!loaded) {
        return;
    }
    verifyPayloadMatches(*loaded, payload);

    std::filesystem::remove_all(root);
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

    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("rigel_chunk_payload_cr_test_" + std::to_string(now));
    std::filesystem::create_directories(root);

    auto storage = std::make_shared<Rigel::Persistence::FilesystemBackend>();
    auto providers = std::make_shared<Rigel::Persistence::ProviderRegistry>();
    providers->add(
        Rigel::Persistence::kBlockRegistryProviderId,
        std::make_shared<Rigel::Persistence::BlockRegistryProvider>(&registry));

    Rigel::Persistence::PersistenceContext context;
    context.rootPath = root.string();
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
        std::filesystem::remove_all(root);
        return;
    }
    Rigel::Persistence::ChunkData payload = decodedRegion.chunks.front().data;

    ChunkStreamer streamer;
    WorldGenConfig::StreamConfig stream;
    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 0;
    stream.genQueueLimit = 0;
    stream.meshQueueLimit = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 0;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    streamer.bind(&manager, &meshStore, &registry, nullptr, generator);

    streamer.setChunkLoader([&](ChunkCoord request) {
        if (request != coord) {
            return false;
        }
        Chunk& target = manager.getOrCreateChunk(request);
        Rigel::Persistence::applyChunkData(payload, target, registry);
        target.setWorldGenVersion(generator->config().world.version);
        target.clearPersistDirty();
        return true;
    });

    streamer.update(coord.toWorldCenter());
    streamer.processCompletions();

    Chunk* loaded = manager.getChunk(coord);
    CHECK(loaded != nullptr);
    if (!loaded) {
        return;
    }
    verifyPayloadMatches(*loaded, payload);

    std::filesystem::remove_all(root);
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

    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("rigel_chunk_payload_cr_random_test_" + std::to_string(now));
    std::filesystem::create_directories(root);

    auto storage = std::make_shared<Rigel::Persistence::FilesystemBackend>();
    auto providers = std::make_shared<Rigel::Persistence::ProviderRegistry>();
    providers->add(
        Rigel::Persistence::kBlockRegistryProviderId,
        std::make_shared<Rigel::Persistence::BlockRegistryProvider>(&registry));

    Rigel::Persistence::PersistenceContext context;
    context.rootPath = root.string();
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
        std::filesystem::remove_all(root);
        return;
    }
    Rigel::Persistence::ChunkData payload = decodedRegion.chunks.front().data;

    ChunkStreamer streamer;
    WorldGenConfig::StreamConfig stream;
    stream.viewDistanceChunks = 0;
    stream.unloadDistanceChunks = 0;
    stream.genQueueLimit = 0;
    stream.meshQueueLimit = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 0;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    streamer.bind(&manager, &meshStore, &registry, nullptr, generator);

    streamer.setChunkLoader([&](ChunkCoord request) {
        if (request != coord) {
            return false;
        }
        Chunk& target = manager.getOrCreateChunk(request);
        Rigel::Persistence::applyChunkData(payload, target, registry);
        target.setWorldGenVersion(generator->config().world.version);
        target.clearPersistDirty();
        return true;
    });

    streamer.update(coord.toWorldCenter());
    streamer.processCompletions();

    Chunk* loaded = manager.getChunk(coord);
    CHECK(loaded != nullptr);
    if (!loaded) {
        return;
    }
    verifyPayloadMatches(*loaded, payload);

    std::filesystem::remove_all(root);
}

TEST_CASE(ChunkStreamer_WorkMetrics_CountGenerationAndSchedulerInspection) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);

    ChunkStreamer streamer;
    WorldGenConfig::StreamConfig stream;
    stream.viewDistanceChunks = 1;
    stream.unloadDistanceChunks = 1;
    stream.genQueueLimit = 0;
    stream.meshQueueLimit = 0;
    stream.updateBudgetPerFrame = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 0;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    streamer.bind(&manager, &meshStore, &registry, nullptr, generator);

    streamer.update(glm::vec3(0.0f));

    const auto& metrics = streamer.workMetrics();
    CHECK_EQ(metrics.generationJobsStarted, static_cast<uint64_t>(7));
    CHECK_EQ(metrics.chunkLoadRequestsStarted, static_cast<uint64_t>(0));
    CHECK_EQ(metrics.meshJobsStarted, static_cast<uint64_t>(0));
    CHECK_EQ(metrics.desiredBuildCoordinatesInspected, static_cast<uint64_t>(27));
    CHECK_EQ(metrics.schedulerCoordinatesInspected, static_cast<uint64_t>(14));
    CHECK_EQ(metrics.lastUpdateDesiredBuildCoordinatesInspected, static_cast<size_t>(27));
    CHECK_EQ(metrics.lastUpdateSchedulerCoordinatesInspected, static_cast<size_t>(14));
}

TEST_CASE(ChunkStreamer_WorkMetrics_CoalescePendingLoadRequests) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);

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
    streamer.bind(&manager, &meshStore, &registry, nullptr, generator);

    size_t callbackCount = 0;
    streamer.setChunkLoader([&](ChunkCoord) {
        ++callbackCount;
        return true;
    });

    streamer.update(glm::vec3(0.0f));
    streamer.update(glm::vec3(0.0f));

    const auto& metrics = streamer.workMetrics();
    CHECK_EQ(callbackCount, static_cast<size_t>(2));
    CHECK_EQ(metrics.chunkLoadRequestsStarted, static_cast<uint64_t>(1));
    CHECK_EQ(metrics.desiredBuildCoordinatesInspected, static_cast<uint64_t>(1));
    CHECK_EQ(metrics.schedulerCoordinatesInspected, static_cast<uint64_t>(4));
    CHECK_EQ(metrics.lastUpdateDesiredBuildCoordinatesInspected, static_cast<size_t>(0));
    CHECK_EQ(metrics.lastUpdateSchedulerCoordinatesInspected, static_cast<size_t>(2));
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
    streamer.bind(&manager, &meshStore, &registry, nullptr, generator);

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

TEST_CASE(ChunkStreamer_DependencyChangeRejectsInFlightMesh) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    BlockID solid = registerTestBlock(registry, "rigel:dependency_solid");

    Chunk& chunk = manager.getOrCreateChunk({0, 0, 0});
    chunk.setBlock(Chunk::SIZE - 1, 0, 0, BlockState{solid}, registry);
    chunk.setWorldGenVersion(generator->config().world.version);
    chunk.setLoadedFromDisk(true);

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
    streamer.bind(&manager, &meshStore, &registry, nullptr, generator);

    streamer.update(glm::vec3(0.0f));
    const uint32_t queuedRevision = chunk.meshRevision();

    manager.setBlock(Chunk::SIZE, 0, 0, BlockState{solid});
    CHECK(chunk.meshRevision() != queuedRevision);

    streamer.processCompletions();
    const auto& metrics = streamer.workMetrics();
    CHECK_EQ(metrics.meshJobsCompleted, static_cast<uint64_t>(1));
    CHECK_EQ(metrics.meshJobsAccepted, static_cast<uint64_t>(0));
    CHECK_EQ(metrics.meshJobsRejectedStale, static_cast<uint64_t>(1));
    CHECK_EQ(metrics.meshRequestsCoalesced, static_cast<uint64_t>(1));
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
    streamer.bind(&manager, &meshStore, &registry, nullptr, generator);

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

TEST_CASE(ChunkStreamer_NeighborArrivalInvalidationsCoalesceAcrossOrder) {
    struct Result {
        uint32_t revisionDelta = 0;
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
        const uint32_t initialRevision = center.meshRevision();

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
            center.invalidateMesh();
        }

        meshStore.set(centerCoord, ChunkMesh{});

        ChunkStreamer streamer;
        WorldGenConfig::StreamConfig stream;
        stream.viewDistanceChunks = 1;
        stream.unloadDistanceChunks = 1;
        stream.genQueueLimit = 0;
        stream.meshQueueLimit = 0;
        stream.updateBudgetPerFrame = 0;
        stream.applyBudgetPerFrame = 0;
        stream.workerThreads = 0;
        stream.maxResidentChunks = 0;
        streamer.setConfig(stream);
        streamer.bind(&manager, &meshStore, &registry, nullptr, generator);

        streamer.update(glm::vec3(0.0f));
        streamer.processCompletions();

        Result result;
        result.revisionDelta = center.meshRevision() - initialRevision;
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

    CHECK_EQ(forwardResult.revisionDelta, static_cast<uint32_t>(1));
    CHECK_EQ(reverseResult.revisionDelta, static_cast<uint32_t>(1));
    CHECK_EQ(forwardResult.metrics.meshInvalidations, static_cast<uint64_t>(1));
    CHECK_EQ(reverseResult.metrics.meshInvalidations, static_cast<uint64_t>(1));
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

    ChunkStreamer streamer;
    WorldGenConfig::StreamConfig stream;
    stream.viewDistanceChunks = 1;
    stream.unloadDistanceChunks = 1;
    stream.genQueueLimit = 0;
    stream.meshQueueLimit = 0;
    stream.updateBudgetPerFrame = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = 0;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    streamer.bind(&manager, &meshStore, &registry, nullptr, generator);

    size_t loadAttempts = 0;
    streamer.setChunkLoader([&](ChunkCoord coord) {
        ++loadAttempts;
        if (coord != ChunkCoord{0, 0, 0}) {
            return false;
        }
        Chunk& chunk = manager.getOrCreateChunk(coord);
        chunk.setBlock(0, 0, 0, BlockState{solid}, registry);
        chunk.setWorldGenVersion(generator->config().world.version);
        chunk.setLoadedFromDisk(true);
        return true;
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
    streamer.update(glm::vec3(0.0f));
    CHECK_EQ(quiescent.meshJobsStarted, settled.meshJobsStarted + 1);
    CHECK(quiescent.lastUpdateSchedulerCoordinatesInspected > 0);
    streamer.processCompletions();

    streamer.update(glm::vec3(0.0f));
    streamer.processCompletions();
    streamer.update(glm::vec3(0.0f));
    CHECK_EQ(quiescent.lastUpdateSchedulerCoordinatesInspected, static_cast<uint64_t>(0));
}

TEST_CASE(ChunkStreamer_SettledWorld_RegeneratesAfterVersionChange) {
    ChunkManager manager;
    BlockRegistry registry;
    WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);

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
    streamer.bind(&manager, &meshStore, &registry, nullptr, generator);

    for (int update = 0; update < 4; ++update) {
        streamer.update(glm::vec3(0.0f));
        streamer.processCompletions();
    }

    streamer.update(glm::vec3(0.0f));
    const ChunkStreamer::WorkMetrics settled = streamer.workMetrics();
    CHECK_EQ(settled.lastUpdateSchedulerCoordinatesInspected, static_cast<uint64_t>(0));

    WorldGenConfig changedConfig = generator->config();
    ++changedConfig.world.version;
    generator->setConfig(std::move(changedConfig));

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
