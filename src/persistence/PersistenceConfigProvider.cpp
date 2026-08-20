#include "Rigel/Persistence/PersistenceConfigProvider.h"

namespace Rigel::Persistence {

void PersistenceConfigProvider::addSource(
    std::unique_ptr<Config::IConfigSource> source) {
    m_sources.push_back(std::move(source));
}

PersistenceConfig PersistenceConfigProvider::load() const {
    PersistenceConfig config;
    for (const auto& source : m_sources) {
        auto yaml = source->load();
        if (yaml) {
            config.applyYaml(source->name().c_str(), *yaml);
        }
    }
    return config;
}

} // namespace Rigel::Persistence
