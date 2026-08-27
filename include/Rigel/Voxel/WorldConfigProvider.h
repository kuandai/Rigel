#pragma once

#include "StreamingConfig.h"

#include "Rigel/Config/ConfigSource.h"

#include <memory>
#include <vector>

namespace Rigel::Voxel {

class WorldConfigProvider {
public:
    void addSource(std::unique_ptr<Config::IConfigSource> source);
    StreamingConfig loadStreamingConfig() const;

private:
    std::vector<std::unique_ptr<Config::IConfigSource>> m_sources;
};

} // namespace Rigel::Voxel
