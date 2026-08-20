#pragma once

#include "Rigel/Config/ConfigSource.h"
#include "Rigel/Voxel/RenderConfig.h"

#include <memory>
#include <vector>

namespace Rigel::Render {

class RenderConfigProvider {
public:
    void addSource(std::unique_ptr<Config::IConfigSource> source);
    Voxel::WorldRenderConfig load() const;

private:
    std::vector<std::unique_ptr<Config::IConfigSource>> m_sources;
};

} // namespace Rigel::Render
