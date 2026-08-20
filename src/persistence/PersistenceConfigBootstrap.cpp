#include "Rigel/Persistence/PersistenceConfigBootstrap.h"

namespace Rigel::Persistence {

PersistenceConfigProvider makePersistenceConfigProvider(
    Asset::AssetManager& assets,
    Voxel::WorldId worldId) {
    PersistenceConfigProvider provider;
    provider.addSource(
        std::make_unique<Config::EmbeddedConfigSource>(
            assets, "raw/persistence_config")
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

} // namespace Rigel::Persistence
