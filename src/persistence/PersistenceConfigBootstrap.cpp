#include "Rigel/Persistence/PersistenceConfigBootstrap.h"

#include <utility>

namespace Rigel::Persistence {

PersistenceConfigProvider makePersistenceConfigProvider(
    Asset::AssetManager& assets,
    Voxel::WorldId worldId) {
    PersistenceConfigProvider provider;
    for (auto& source : Config::makeStandardConfigSources(
             assets, "raw/persistence_config", "persistence.yaml", worldId)) {
        provider.addSource(std::move(source));
    }
    return provider;
}

} // namespace Rigel::Persistence
