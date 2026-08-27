#include "Rigel/Voxel/WorldConfigBootstrap.h"

#include <utility>

namespace Rigel::Voxel {

WorldConfigProvider makeWorldConfigProvider(Asset::AssetManager& assets,
                                            WorldId worldId) {
    WorldConfigProvider provider;
    for (auto& source : Config::makeStandardConfigSources(
             assets, "raw/streaming_config", "streaming.yaml", worldId)) {
        provider.addSource(std::move(source));
    }
    return provider;
}

} // namespace Rigel::Voxel
