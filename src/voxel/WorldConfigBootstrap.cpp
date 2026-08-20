#include "Rigel/Voxel/WorldConfigBootstrap.h"

namespace Rigel::Voxel {

WorldConfigProvider makeWorldConfigProvider(Asset::AssetManager& assets,
                                            WorldId worldId) {
    WorldConfigProvider provider;
    provider.addSource(
        std::make_unique<Config::EmbeddedConfigSource>(assets, "raw/world_config")
    );
    provider.addSource(
        std::make_unique<Config::FileConfigSource>("config/world_generation.yaml")
    );
    provider.addSource(
        std::make_unique<Config::FileConfigSource>("world_generation.yaml")
    );
    provider.addSource(
        std::make_unique<Config::FileConfigSource>(
            "config/worlds/" + std::to_string(worldId) + "/world_generation.yaml")
    );
    return provider;
}

PersistenceConfigProvider makePersistenceConfigProvider(
    Asset::AssetManager& assets,
    WorldId worldId) {
    PersistenceConfigProvider provider;
    provider.addSource(
        std::make_unique<Config::EmbeddedConfigSource>(assets, "raw/persistence_config")
    );
    provider.addSource(
        std::make_unique<Config::FileConfigSource>("config/persistence.yaml")
    );
    provider.addSource(
        std::make_unique<Config::FileConfigSource>("persistence.yaml")
    );
    provider.addSource(
        std::make_unique<Config::FileConfigSource>(
            "config/worlds/" + std::to_string(worldId) + "/persistence.yaml")
    );
    return provider;
}

} // namespace Rigel::Voxel
