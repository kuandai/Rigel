#pragma once

#include "BlockRegistry.h"

#include <Rigel/Asset/AssetManager.h>

namespace Rigel::Voxel {

class WorldResources {
public:
    void initialize(Asset::AssetManager& assets);

    BlockRegistry& registry() { return m_registry; }
    const BlockRegistry& registry() const { return m_registry; }

    bool initialized() const { return m_initialized; }

private:
    // Block registrations transitively retain their immutable shared models.
    BlockRegistry m_registry;
    bool m_initialized = false;
};

} // namespace Rigel::Voxel
