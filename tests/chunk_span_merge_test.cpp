#include "TestFramework.h"

#include "Rigel/Persistence/ChunkSpanMerge.h"
#include "Rigel/Persistence/ChunkSerializer.h"
#include "Rigel/Voxel/BlockRegistry.h"
#include "Rigel/Voxel/BlockType.h"

#include <limits>
#include <string>

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

template <typename Fn>
void checkRuntimeError(Fn&& fn, const std::string& expected) {
    try {
        fn();
    } catch (const std::runtime_error& error) {
        CHECK_EQ(std::string(error.what()), expected);
        return;
    }
    throw Rigel::Test::TestFailure("Expected std::runtime_error");
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

TEST_CASE(ChunkSerializer_RejectsInvalidSpanBounds) {
    BlockRegistry registry;
    Chunk chunk({0, 0, 0});

    ChunkData negativeSize;
    negativeSize.span.sizeX = -1;
    negativeSize.span.sizeY = 1;
    negativeSize.span.sizeZ = 1;
    checkRuntimeError(
        [&]() { applyChunkData(negativeSize, chunk, registry); },
        "ChunkSerializer: span size must be positive");

    ChunkData negativeOffset;
    negativeOffset.span.offsetX = -1;
    negativeOffset.span.sizeX = 1;
    negativeOffset.span.sizeY = 1;
    negativeOffset.span.sizeZ = 1;
    checkRuntimeError(
        [&]() { applyChunkData(negativeOffset, chunk, registry); },
        "ChunkSerializer: span offset must be non-negative");

    ChunkData overflowing;
    overflowing.span.offsetX = std::numeric_limits<int32_t>::max();
    overflowing.span.sizeX = std::numeric_limits<int32_t>::max();
    overflowing.span.sizeY = 1;
    overflowing.span.sizeZ = 1;
    checkRuntimeError(
        [&]() { applyChunkData(overflowing, chunk, registry); },
        "ChunkSerializer: span out of chunk bounds");
}

TEST_CASE(ChunkSerializer_RejectsMismatchedBlockCount) {
    BlockRegistry registry;
    Chunk chunk({0, 0, 0});
    ChunkData data;
    data.span.sizeX = 2;
    data.span.sizeY = 1;
    data.span.sizeZ = 1;
    data.blocks.resize(1);

    checkRuntimeError(
        [&]() { applyChunkData(data, chunk, registry); },
        "ChunkSerializer: block data size mismatch");
}

TEST_CASE(ChunkSerializer_InvalidBlockIdDoesNotPartiallyApply) {
    BlockRegistry registry;
    const BlockID originalId = registerBlock(registry, "test:original");
    const BlockID replacementId = registerBlock(registry, "test:replacement");

    Chunk chunk({0, 0, 0});
    BlockState original;
    original.id = originalId;
    chunk.fill(original, registry);
    chunk.clearDirty();
    chunk.clearPersistDirty();
    const uint32_t revision = chunk.meshRevision();

    ChunkData data;
    data.span.sizeX = 2;
    data.span.sizeY = 1;
    data.span.sizeZ = 1;
    data.blocks.resize(2);
    data.blocks[0].id = replacementId;
    data.blocks[1].id.type = std::numeric_limits<uint16_t>::max();

    checkRuntimeError(
        [&]() { applyChunkData(data, chunk, registry); },
        "ChunkSerializer: invalid block ID 65535");

    CHECK_EQ(chunk.getBlock(0, 0, 0).id, originalId);
    CHECK_EQ(chunk.getBlock(1, 0, 0).id, originalId);
    CHECK(!chunk.isDirty());
    CHECK(!chunk.isPersistDirty());
    CHECK_EQ(chunk.meshRevision(), revision);
}

TEST_CASE(ChunkSpanMerge_ValidatesAllSpansBeforeBaseFill) {
    BlockRegistry registry;
    const BlockID originalId = registerBlock(registry, "test:original");
    const BlockID replacementId = registerBlock(registry, "test:replacement");

    ChunkSpan firstSpan;
    firstSpan.sizeX = 1;
    firstSpan.sizeY = 1;
    firstSpan.sizeZ = 1;
    ChunkSnapshot first;
    first.data.span = firstSpan;
    first.data.blocks.push_back(BlockState{replacementId, 0, 0});

    ChunkSpan secondSpan = firstSpan;
    secondSpan.offsetX = 1;
    ChunkSnapshot second;
    second.data.span = secondSpan;
    second.data.blocks.push_back(
        BlockState{BlockID{std::numeric_limits<uint16_t>::max()}, 0, 0});
    const std::vector<const ChunkSnapshot*> spans = {&first, &second};

    Chunk chunk({0, 0, 0});
    BlockState original;
    original.id = originalId;
    chunk.fill(original, registry);
    chunk.clearDirty();
    chunk.clearPersistDirty();
    bool baseCalled = false;
    const auto baseFill = [&](Chunk&, const BlockRegistry&) {
        baseCalled = true;
    };

    checkRuntimeError(
        [&]() { mergeChunkSpans(chunk, registry, spans, baseFill); },
        "ChunkSerializer: invalid block ID 65535");

    CHECK(!baseCalled);
    CHECK_EQ(chunk.getBlock(0, 0, 0).id, originalId);
    CHECK_EQ(chunk.getBlock(1, 0, 0).id, originalId);
    CHECK(!chunk.isDirty());
    CHECK(!chunk.isPersistDirty());
}
