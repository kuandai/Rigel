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
                       uint32_t worldGenVersion);

void saveWorldToDisk(const Voxel::World& world,
                     PersistenceService& service,
                     PersistenceContext context);

} // namespace Persistence
} // namespace Rigel
