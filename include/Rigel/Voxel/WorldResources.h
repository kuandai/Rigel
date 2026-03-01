#pragma once

#include "BlockRegistry.h"
#include "TextureAtlas.h"

#include <Rigel/Asset/DefinitionRegistry.h>
#include <Rigel/Asset/AssetManager.h>

namespace Rigel::Voxel {

class WorldResources {
public:
    void initialize(Asset::AssetManager& assets);

    BlockRegistry& registry() { return m_registry; }
    const BlockRegistry& registry() const { return m_registry; }

    TextureAtlas& textureAtlas() { return m_textureAtlas; }
    const TextureAtlas& textureAtlas() const { return m_textureAtlas; }

    Asset::EntityTypeRegistry& entityTypes() { return m_entityTypes; }
    const Asset::EntityTypeRegistry& entityTypes() const { return m_entityTypes; }

    Asset::ItemDefinitionRegistry& itemDefinitions() { return m_itemDefinitions; }
    const Asset::ItemDefinitionRegistry& itemDefinitions() const { return m_itemDefinitions; }

    bool initialized() const { return m_initialized; }

    void releaseRenderResources();

private:
    BlockRegistry m_registry;
    TextureAtlas m_textureAtlas;
    Asset::EntityTypeRegistry m_entityTypes;
    Asset::ItemDefinitionRegistry m_itemDefinitions;
    bool m_initialized = false;
};

} // namespace Rigel::Voxel
