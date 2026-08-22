#pragma once

#include "Rigel/Persistence/Types.h"
#include "Rigel/Voxel/BlockRegistry.h"

#include <cstddef>

namespace Rigel::Persistence::detail {

size_t validateChunkSpan(const ChunkSpan& span);
size_t validateChunkBlockCount(const ChunkSpan& span, size_t blockCount);
void validateChunkBlockIds(const ChunkData& data, const Voxel::BlockRegistry& registry);
void validateChunkData(const ChunkData& data, const Voxel::BlockRegistry& registry);

} // namespace Rigel::Persistence::detail
