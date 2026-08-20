#pragma once

#include "Rigel/Render/RenderConfigProvider.h"
#include "Rigel/Voxel/WorldId.h"

namespace Rigel {
namespace Asset { class AssetManager; }

namespace Render {

RenderConfigProvider makeRenderConfigProvider(Asset::AssetManager& assets,
                                              Voxel::WorldId worldId);

} // namespace Render
} // namespace Rigel
