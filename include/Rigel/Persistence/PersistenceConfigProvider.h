#pragma once

#include "Rigel/Config/ConfigSource.h"
#include "Rigel/Persistence/PersistenceConfig.h"

#include <memory>
#include <vector>

namespace Rigel::Persistence {

class PersistenceConfigProvider {
public:
    void addSource(std::unique_ptr<Config::IConfigSource> source);
    PersistenceConfig load() const;

private:
    std::vector<std::unique_ptr<Config::IConfigSource>> m_sources;
};

} // namespace Rigel::Persistence
