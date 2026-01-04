#pragma once

#include "Rigel/Voxel/WorldConfigProvider.h"
#include "Rigel/Voxel/WorldSet.h"

namespace Rigel {
namespace Asset { class AssetManager; }

namespace Voxel {

ConfigProvider makeWorldConfigProvider(Asset::AssetManager& assets,
                                       WorldId worldId);

ConfigProvider makeRenderConfigProvider(Asset::AssetManager& assets,
                                        WorldId worldId);

} // namespace Voxel
} // namespace Rigel
