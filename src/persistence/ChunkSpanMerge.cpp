#include "Rigel/Persistence/ChunkSpanMerge.h"

#include "Rigel/Persistence/ChunkSerializer.h"

namespace Rigel::Persistence {

namespace {
bool isFullSpan(const ChunkSpan& span) {
    return span.offsetX == 0 &&
        span.offsetY == 0 &&
        span.offsetZ == 0 &&
        span.sizeX == Voxel::Chunk::SIZE &&
        span.sizeY == Voxel::Chunk::SIZE &&
        span.sizeZ == Voxel::Chunk::SIZE;
}

bool isSubchunkSpan(const ChunkSpan& span) {
    return span.sizeX == Voxel::Chunk::SUBCHUNK_SIZE &&
        span.sizeY == Voxel::Chunk::SUBCHUNK_SIZE &&
        span.sizeZ == Voxel::Chunk::SUBCHUNK_SIZE &&
        span.offsetX % Voxel::Chunk::SUBCHUNK_SIZE == 0 &&
        span.offsetY % Voxel::Chunk::SUBCHUNK_SIZE == 0 &&
        span.offsetZ % Voxel::Chunk::SUBCHUNK_SIZE == 0;
}
}

ChunkSpanMergeResult mergeChunkSpans(
    Voxel::Chunk& chunk,
    const Voxel::BlockRegistry& registry,
    std::span<const ChunkSnapshot* const> spans,
    const ChunkBaseFillFn& baseFill) {
    ChunkSpanMergeResult result;
    if (spans.empty()) {
        return result;
    }

    result.loadedFromDisk = true;
    for (const ChunkSnapshot* snapshot : spans) {
        if (!snapshot) {
            continue;
        }
        const ChunkSpan& span = snapshot->data.span;
        if (isFullSpan(span)) {
            result.fullSpan = true;
        }
        if (isSubchunkSpan(span)) {
            int sx = span.offsetX / Voxel::Chunk::SUBCHUNK_SIZE;
            int sy = span.offsetY / Voxel::Chunk::SUBCHUNK_SIZE;
            int sz = span.offsetZ / Voxel::Chunk::SUBCHUNK_SIZE;
            if (sx >= 0 && sx < 2 && sy >= 0 && sy < 2 && sz >= 0 && sz < 2) {
                int index = sx + (sy << 1) + (sz << 2);
                result.subchunkMask = static_cast<uint8_t>(result.subchunkMask | (1u << index));
            }
        }
    }

    if (result.fullSpan) {
        result.subchunkMask = 0xFF;
    }

    if (!result.fullSpan &&
        result.subchunkMask != 0xFF &&
        baseFill) {
        baseFill(chunk, registry);
        result.appliedBase = true;
    }

    for (const ChunkSnapshot* snapshot : spans) {
        if (!snapshot) {
            continue;
        }
        applyChunkData(snapshot->data, chunk, registry);
    }

    return result;
}

} // namespace Rigel::Persistence
