#pragma once

#include "Rigel/Persistence/Types.h"
#include "Rigel/Voxel/BlockRegistry.h"
#include "Rigel/Voxel/Chunk.h"

#include <cstdint>
#include <functional>
#include <span>

namespace Rigel::Persistence {

struct ChunkSpanMergeResult {
    bool loadedFromDisk = false;
    bool fullSpan = false;
    uint8_t subchunkMask = 0;
    bool appliedBase = false;
};

using ChunkBaseFillFn = std::function<void(Voxel::Chunk&, const Voxel::BlockRegistry&)>;

ChunkSpanMergeResult mergeChunkSpans(
    Voxel::Chunk& chunk,
    const Voxel::BlockRegistry& registry,
    std::span<const ChunkSnapshot* const> spans,
    const ChunkBaseFillFn& baseFill);

} // namespace Rigel::Persistence
