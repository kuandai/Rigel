#pragma once

#include "Rigel/Voxel/WorldConfigProvider.h"
#include "Rigel/Voxel/WorldSet.h"

namespace Rigel {
namespace Asset { class AssetManager; }

namespace Voxel {

WorldConfigProvider makeWorldConfigProvider(Asset::AssetManager& assets,
                                            WorldId worldId);

} // namespace Voxel
} // namespace Rigel
