#pragma once

#include <cstdint>
#include <string>

#include "Rigel/Persistence/Types.h"
#include "Rigel/Voxel/WorldSet.h"

namespace Rigel {
namespace Asset { class AssetManager; }
namespace Voxel { class World; }

namespace Persistence {
class PersistenceService;

std::string mainWorldRootPath(Voxel::WorldId id);

void loadWorldFromDisk(Voxel::World& world,
                       Asset::AssetManager& assets,
                       PersistenceService& service,
                       PersistenceContext context,
                       uint32_t worldGenVersion,
                       SaveScope scope = SaveScope::All);

void saveWorldToDisk(const Voxel::World& world,
                     PersistenceService& service,
                     PersistenceContext context);

bool loadChunkFromDisk(Voxel::World& world,
                       PersistenceService& service,
                       PersistenceContext context,
                       const Voxel::ChunkCoord& coord,
                       uint32_t worldGenVersion);

} // namespace Persistence
} // namespace Rigel
