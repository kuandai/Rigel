#pragma once

#include "StreamingConfig.h"
#include "WorldGenConfig.h"

#include "Rigel/Config/ConfigSource.h"
#include "Rigel/Persistence/PersistenceConfig.h"

#include <memory>
#include <vector>

namespace Rigel::Voxel {

struct WorldConfiguration {
    WorldGenConfig generation;
    StreamingConfig streaming;
};

class WorldConfigProvider {
public:
    void addSource(std::unique_ptr<Config::IConfigSource> source);
    WorldConfiguration loadConfig() const;

private:
    std::vector<std::unique_ptr<Config::IConfigSource>> m_sources;
};

class PersistenceConfigProvider {
public:
    void addSource(std::unique_ptr<Config::IConfigSource> source);
    Persistence::PersistenceConfig loadPersistenceConfig() const;

private:
    std::vector<std::unique_ptr<Config::IConfigSource>> m_sources;
};

} // namespace Rigel::Voxel
