#include "Rigel/Render/RenderConfigBootstrap.h"

#include <utility>

namespace Rigel::Render {

RenderConfigProvider makeRenderConfigProvider(Asset::AssetManager& assets,
                                              Voxel::WorldId worldId) {
    RenderConfigProvider provider;
    for (auto& source : Config::makeStandardConfigSources(
             assets, "raw/render_config", "render.yaml", worldId)) {
        provider.addSource(std::move(source));
    }
    return provider;
}

} // namespace Rigel::Render
