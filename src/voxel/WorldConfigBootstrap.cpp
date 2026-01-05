#include "Rigel/Voxel/WorldConfigBootstrap.h"

namespace Rigel::Voxel {

ConfigProvider makeWorldConfigProvider(Asset::AssetManager& assets,
                                       WorldId worldId) {
    ConfigProvider provider;
    provider.addSource(
        std::make_unique<EmbeddedConfigSource>(assets, "raw/world_config")
    );
    provider.addSource(
        std::make_unique<FileConfigSource>("config/world_generation.yaml")
    );
    provider.addSource(
        std::make_unique<FileConfigSource>("world_generation.yaml")
    );
    provider.addSource(
        std::make_unique<FileConfigSource>(
            "config/worlds/" + std::to_string(worldId) + "/world_generation.yaml")
    );
    return provider;
}

ConfigProvider makeRenderConfigProvider(Asset::AssetManager& assets,
                                        WorldId worldId) {
    ConfigProvider provider;
    provider.addSource(
        std::make_unique<EmbeddedConfigSource>(assets, "raw/render_config")
    );
    provider.addSource(
        std::make_unique<FileConfigSource>("config/render.yaml")
    );
    provider.addSource(
        std::make_unique<FileConfigSource>("render.yaml")
    );
    provider.addSource(
        std::make_unique<FileConfigSource>(
            "config/worlds/" + std::to_string(worldId) + "/render.yaml")
    );
    return provider;
}

ConfigProvider makePersistenceConfigProvider(Asset::AssetManager& assets,
                                             WorldId worldId) {
    ConfigProvider provider;
    provider.addSource(
        std::make_unique<EmbeddedConfigSource>(assets, "raw/persistence_config")
    );
    provider.addSource(
        std::make_unique<FileConfigSource>("config/persistence.yaml")
    );
    provider.addSource(
        std::make_unique<FileConfigSource>("persistence.yaml")
    );
    provider.addSource(
        std::make_unique<FileConfigSource>(
            "config/worlds/" + std::to_string(worldId) + "/persistence.yaml")
    );
    return provider;
}

} // namespace Rigel::Voxel
