#pragma once

#include "BlockModel.h"
#include "BlockRegistry.h"
#include "TextureAtlas.h"

#include <Rigel/Asset/AssetManager.h>

namespace Rigel::Voxel {

class WorldResources {
public:
    void initialize(Asset::AssetManager& assets);

    BlockRegistry& registry() { return m_registry; }
    const BlockRegistry& registry() const { return m_registry; }

    BlockModelRegistry& modelRegistry() { return m_models; }
    const BlockModelRegistry& modelRegistry() const { return m_models; }

    TextureAtlas& textureAtlas() { return m_textureAtlas; }
    const TextureAtlas& textureAtlas() const { return m_textureAtlas; }

    bool initialized() const { return m_initialized; }

    void releaseRenderResources();

private:
    BlockModelRegistry m_models;
    BlockRegistry m_registry;
    TextureAtlas m_textureAtlas;
    bool m_initialized = false;
};

} // namespace Rigel::Voxel
