#pragma once

#include "Rigel/Persistence/Types.h"
#include "Rigel/Voxel/ChunkManager.h"
#include "Rigel/Voxel/BlockRegistry.h"
#include "Rigel/Voxel/ChunkCoord.h"

#include <optional>
#include <string>
#include <vector>

namespace Rigel::Persistence::Backends::CR {

std::vector<ChunkSnapshot> encodeRigelChunk(const Voxel::Chunk& chunk,
                                            const Voxel::BlockRegistry& registry,
                                            const Voxel::ChunkCoord& coord,
                                            const std::string& zoneId);

void decodeChunkSnapshot(const ChunkSnapshot& snapshot,
                         Voxel::ChunkManager& manager,
                         const Voxel::BlockRegistry& registry,
                         std::optional<uint32_t> worldGenVersion = std::nullopt,
                         bool markPersistClean = true);

} // namespace Rigel::Persistence::Backends::CR
