#pragma once

#include "Rigel/Persistence/Types.h"
#include "Rigel/Voxel/Chunk.h"
#include "Rigel/Voxel/BlockRegistry.h"

namespace Rigel::Persistence {

ChunkData serializeChunk(const Voxel::Chunk& chunk);
ChunkData serializeChunkSpan(const Voxel::Chunk& chunk, const ChunkSpan& span);
void applyChunkData(const ChunkData& data, Voxel::Chunk& chunk, const Voxel::BlockRegistry& registry);

} // namespace Rigel::Persistence
