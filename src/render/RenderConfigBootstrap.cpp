#include "Rigel/Render/RenderConfigBootstrap.h"

namespace Rigel::Render {

RenderConfigProvider makeRenderConfigProvider(Asset::AssetManager& assets,
                                              Voxel::WorldId worldId) {
    RenderConfigProvider provider;
    provider.addSource(
        std::make_unique<Config::EmbeddedConfigSource>(assets, "raw/render_config")
    );
    provider.addSource(
        std::make_unique<Config::FileConfigSource>("config/render.yaml")
    );
    provider.addSource(
        std::make_unique<Config::FileConfigSource>("render.yaml")
    );
    provider.addSource(
        std::make_unique<Config::FileConfigSource>(
            "config/worlds/" + std::to_string(worldId) + "/render.yaml")
    );
    return provider;
}

} // namespace Rigel::Render
