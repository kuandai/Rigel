#pragma once

#include "StreamingConfig.h"
#include "WorldGenConfig.h"

#include "Rigel/Config/ConfigSource.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Rigel::Voxel {

struct GeneratorDefinitionSource {
    std::string id;
    uint32_t revision = 0;

    bool operator==(const GeneratorDefinitionSource&) const = default;
};

struct WorldConfiguration {
    GeneratorDefinitionSource generatorSource;
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
