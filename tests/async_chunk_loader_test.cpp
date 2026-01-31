#include "TestFramework.h"

#include "Rigel/Persistence/AsyncChunkLoader.h"
#include "Rigel/Persistence/Backends/Memory/MemoryFormat.h"
#include "Rigel/Persistence/ChunkSerializer.h"
#include "Rigel/Persistence/PersistenceService.h"
#include "Rigel/Persistence/Storage.h"
#include "Rigel/Voxel/WorldResources.h"
#include "Rigel/Voxel/World.h"

#include <chrono>
#include <filesystem>
#include <optional>
#include <random>
#include <vector>

using namespace Rigel::Voxel;
using namespace Rigel::Persistence;

namespace {
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

    CHECK(loader.request(coord));
    CHECK(loader.isPending(coord));

    loader.drainCompletions(1);

    Chunk* loaded = world.chunkManager().getChunk(coord);
    CHECK(loaded != nullptr);
    if (loaded) {
        verifyPayloadMatches(*loaded, payload);
    }
    CHECK(!loader.isPending(coord));
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

    CHECK(loader.request(coord));
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

    CHECK(loader.request(coordA));
    CHECK(loader.request(coordB));

    loader.drainCompletions(1);

    size_t loadedCount = world.chunkManager().loadedChunkCount();
    CHECK_EQ(loadedCount, static_cast<size_t>(1));

    loader.drainCompletions(4);
    loadedCount = world.chunkManager().loadedChunkCount();
    CHECK_EQ(loadedCount, static_cast<size_t>(2));
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

    CHECK(loader.request(coord));
    loader.cancel(coord);
    CHECK(!loader.isPending(coord));

    loader.drainCompletions(2);

    Chunk* loaded = world.chunkManager().getChunk(coord);
    CHECK(loaded == nullptr);
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

    CHECK(loader.request(coord));
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
