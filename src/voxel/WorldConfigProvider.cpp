#include "Rigel/Voxel/WorldConfigProvider.h"

namespace Rigel::Voxel {

void WorldConfigProvider::addSource(
    std::unique_ptr<Config::IConfigSource> source) {
    m_sources.push_back(std::move(source));
}

StreamingConfig WorldConfigProvider::loadStreamingConfig() const {
    StreamingConfig config;
    for (const auto& source : m_sources) {
        const auto yaml = source->load();
        if (!yaml) {
            continue;
        }
        StreamingConfig candidate = config;
        candidate.applyYaml(source->name().c_str(), *yaml);
        candidate.validate(source->name().c_str());
        config = std::move(candidate);
    }
    config.validate("merged streaming configuration");
    return config;
}

} // namespace Rigel::Voxel
