#pragma once

#include "BlockRegistry.h"
#include "TextureAtlas.h"

#include <Rigel/Asset/AssetManager.h>

namespace Rigel::Voxel {

class WorldResources {
public:
    void initialize(Asset::AssetManager& assets);

    BlockRegistry& registry() { return m_registry; }
    const BlockRegistry& registry() const { return m_registry; }

    TextureAtlas& textureAtlas() { return m_textureAtlas; }
    const TextureAtlas& textureAtlas() const { return m_textureAtlas; }

    bool initialized() const { return m_initialized; }

    void releaseRenderResources();

private:
    BlockRegistry m_registry;
    TextureAtlas m_textureAtlas;
    bool m_initialized = false;
};

} // namespace Rigel::Voxel
