#pragma once

#include "Rigel/Persistence/Types.h"
#include "Rigel/Voxel/ChunkCoord.h"

namespace Rigel::Voxel {
class World;
}

namespace Rigel::Persistence {
class PersistenceFormat;

namespace detail {

void saveChunkToPublishedWorld(
    const Voxel::World& world,
    PersistenceFormat& format,
    const PersistenceContext& context,
    const Voxel::ChunkCoord& coord);

} // namespace detail
} // namespace Rigel::Persistence
