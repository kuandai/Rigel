#pragma once

#include "StreamingConfig.h"
#include "WorldGenConfig.h"

#include "Rigel/Config/ConfigSource.h"

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
    StreamingConfig loadStreamingConfig() const;

private:
    std::vector<std::unique_ptr<Config::IConfigSource>> m_sources;
};

} // namespace Rigel::Voxel
