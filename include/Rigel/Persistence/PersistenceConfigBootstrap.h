#pragma once

#include "Rigel/Persistence/PersistenceConfigProvider.h"
#include "Rigel/Voxel/WorldId.h"

namespace Rigel {
namespace Asset { class AssetManager; }

namespace Persistence {

PersistenceConfigProvider makePersistenceConfigProvider(
    Asset::AssetManager& assets,
    Voxel::WorldId worldId);

} // namespace Persistence
} // namespace Rigel
