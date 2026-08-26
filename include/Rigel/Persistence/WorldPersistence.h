#pragma once

#include <string>

#include "Rigel/Persistence/Types.h"
#include "Rigel/Persistence/WorldSettings.h"
#include "Rigel/Voxel/WorldSet.h"

namespace Rigel {
namespace Asset { class AssetManager; }
namespace Voxel { class World; }

namespace Persistence {
class PersistenceService;

std::string mainWorldRootPath(Voxel::WorldId id);

void loadBootstrapEntities(Voxel::World& world,
                           Asset::AssetManager& assets,
                           PersistenceService& service,
                           PersistenceContext context);

void saveWorldToDisk(const Voxel::World& world,
                     const WorldSettings& settings,
                     PersistenceService& service,
                     PersistenceContext context);

void saveChunkToDisk(const Voxel::World& world,
                     PersistenceService& service,
                     PersistenceContext context,
                     const Voxel::ChunkCoord& coord);

} // namespace Persistence
} // namespace Rigel
