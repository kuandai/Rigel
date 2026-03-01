#include "Rigel/Voxel/WorldResources.h"

#include "Rigel/Voxel/BlockLoader.h"

#include <spdlog/spdlog.h>

namespace Rigel::Voxel {

void WorldResources::initialize(Asset::AssetManager& assets) {
    if (m_initialized) {
        spdlog::warn("WorldResources::initialize called multiple times");
        return;
    }

    try {
        BlockLoader loader;
        size_t loaded = loader.loadFromManifest(assets, m_registry, m_textureAtlas);
        if (m_textureAtlas.textureCount() > 0) {
            m_textureAtlas.upload();
        }
        spdlog::info("Loaded {} block types (registry size {})", loaded, m_registry.size());
        spdlog::info("Block registry snapshot hash: {:016x}", m_registry.snapshotHash());
        spdlog::info("Texture atlas entries: {}", m_textureAtlas.textureCount());
    } catch (const std::exception& e) {
        spdlog::error("Failed to load blocks: {}", e.what());
        throw;
    }

    m_initialized = true;
}

void WorldResources::releaseRenderResources() {
    m_textureAtlas.releaseGPU();
}

} // namespace Rigel::Voxel
