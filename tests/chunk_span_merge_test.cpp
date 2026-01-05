#include "TestFramework.h"

#include "Rigel/Persistence/ChunkSpanMerge.h"
#include "Rigel/Persistence/ChunkSerializer.h"
#include "Rigel/Voxel/BlockRegistry.h"
#include "Rigel/Voxel/BlockType.h"

using namespace Rigel::Voxel;
using namespace Rigel::Persistence;

namespace {
BlockID registerBlock(BlockRegistry& registry, const std::string& id) {
    BlockType block;
    block.identifier = id;
    block.isOpaque = true;
    block.isSolid = true;
    return registry.registerBlock(id, std::move(block));
}

ChunkSnapshot makeSnapshot(const ChunkCoord& coord,
                           const ChunkSpan& span,
                           const BlockRegistry& registry,
                           BlockID fillId) {
    Chunk chunk(coord);
    BlockState state;
    state.id = fillId;
    chunk.fill(state, registry);
    ChunkSnapshot snapshot;
    snapshot.key = ChunkKey{"test", coord.x, coord.y, coord.z};
    snapshot.data = serializeChunkSpan(chunk, span);
    return snapshot;
}

ChunkSnapshot makeFullSnapshot(const ChunkCoord& coord,
                               const BlockRegistry& registry,
                               BlockID fillId) {
    Chunk chunk(coord);
    BlockState state;
    state.id = fillId;
    chunk.fill(state, registry);
    ChunkSnapshot snapshot;
    snapshot.key = ChunkKey{"test", coord.x, coord.y, coord.z};
    snapshot.data = serializeChunk(chunk);
    return snapshot;
}
}

TEST_CASE(ChunkSpanMerge_FullSpanSkipsBaseFill) {
    BlockRegistry registry;
    BlockID baseId = registerBlock(registry, "test:base");
    BlockID diskId = registerBlock(registry, "test:disk");

    ChunkCoord coord{0, 0, 0};
    ChunkSnapshot snapshot = makeFullSnapshot(coord, registry, diskId);
    std::vector<const ChunkSnapshot*> spans = {&snapshot};

    bool baseCalled = false;
    Chunk chunk(coord);
    auto baseFill = [&](Chunk& target, const BlockRegistry& reg) {
        baseCalled = true;
        BlockState state;
        state.id = baseId;
        target.fill(state, reg);
    };

    ChunkSpanMergeResult result =
        mergeChunkSpans(chunk, registry, spans, baseFill);

    CHECK(result.loadedFromDisk);
    CHECK(result.fullSpan);
    CHECK(!baseCalled);

    ChunkData expected = snapshot.data;
    ChunkData actual = serializeChunk(chunk);
    CHECK_EQ(actual.span, expected.span);
    CHECK_EQ(actual.blocks, expected.blocks);
}

TEST_CASE(ChunkSpanMerge_PartialSpanUsesBaseFill) {
    BlockRegistry registry;
    BlockID baseId = registerBlock(registry, "test:base");
    BlockID diskId = registerBlock(registry, "test:disk");

    ChunkCoord coord{0, 0, 0};
    ChunkSpan span;
    span.chunkX = coord.x;
    span.chunkY = coord.y;
    span.chunkZ = coord.z;
    span.offsetX = 0;
    span.offsetY = 0;
    span.offsetZ = 0;
    span.sizeX = Chunk::SUBCHUNK_SIZE;
    span.sizeY = Chunk::SUBCHUNK_SIZE;
    span.sizeZ = Chunk::SUBCHUNK_SIZE;

    ChunkSnapshot snapshot = makeSnapshot(coord, span, registry, diskId);
    std::vector<const ChunkSnapshot*> spans = {&snapshot};

    bool baseCalled = false;
    Chunk chunk(coord);
    auto baseFill = [&](Chunk& target, const BlockRegistry& reg) {
        baseCalled = true;
        BlockState state;
        state.id = baseId;
        target.fill(state, reg);
    };

    ChunkSpanMergeResult result =
        mergeChunkSpans(chunk, registry, spans, baseFill);

    CHECK(result.loadedFromDisk);
    CHECK(!result.fullSpan);
    CHECK(baseCalled);

    Chunk expected(coord);
    BlockState baseState;
    baseState.id = baseId;
    expected.fill(baseState, registry);
    applyChunkData(snapshot.data, expected, registry);

    ChunkData actual = serializeChunk(chunk);
    ChunkData expectedData = serializeChunk(expected);
    CHECK_EQ(actual.blocks, expectedData.blocks);
}

TEST_CASE(ChunkSpanMerge_EmptySpansNoOp) {
    BlockRegistry registry;
    BlockID baseId = registerBlock(registry, "test:base");

    ChunkCoord coord{0, 0, 0};
    Chunk chunk(coord);
    BlockState state;
    state.id = baseId;
    chunk.fill(state, registry);

    bool baseCalled = false;
    auto baseFill = [&](Chunk&, const BlockRegistry&) {
        baseCalled = true;
    };

    std::vector<const ChunkSnapshot*> spans;
    ChunkSpanMergeResult result =
        mergeChunkSpans(chunk, registry, spans, baseFill);

    CHECK(!result.loadedFromDisk);
    CHECK(!baseCalled);
}
