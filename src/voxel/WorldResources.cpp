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
        Asset::IR::AssetGraphIR graph;
        size_t loaded = loader.loadFromManifest(assets, m_registry, m_textureAtlas, &graph);
        std::vector<Asset::IR::ValidationIssue> registryIssues;
        Asset::buildDefinitionRegistriesFromAssetGraph(
            graph,
            m_entityTypes,
            m_itemDefinitions,
            &registryIssues);
        const auto registrySummary = Asset::IR::summarizeValidationIssues(registryIssues, 12);
        if (registrySummary.errorCount > 0 || registrySummary.warningCount > 0) {
            spdlog::warn("Definition registry build reported {} errors and {} warnings (showing up to {} samples)",
                         registrySummary.errorCount,
                         registrySummary.warningCount,
                         registrySummary.samples.size());
        }
        for (const auto& issue : registrySummary.samples) {
            if (issue.severity == Asset::IR::ValidationSeverity::Error) {
                spdlog::error("Definition registry error [{}] {} (id='{}', field='{}')",
                              issue.sourcePath,
                              issue.message,
                              issue.identifier,
                              issue.field);
            } else {
                spdlog::warn("Definition registry warning [{}] {} (id='{}', field='{}')",
                             issue.sourcePath,
                             issue.message,
                             issue.identifier,
                             issue.field);
            }
        }
        if (registryIssues.size() > registrySummary.samples.size()) {
            spdlog::warn("Definition registry build: {} additional diagnostics omitted",
                         registryIssues.size() - registrySummary.samples.size());
        }
        if (m_textureAtlas.textureCount() > 0) {
            m_textureAtlas.upload();
        }
        spdlog::info("Loaded {} block types (registry size {})", loaded, m_registry.size());
        spdlog::info("Block registry snapshot hash: {:016x}", m_registry.snapshotHash());
        spdlog::info("Loaded {} entity definitions (snapshot {:016x})",
                     m_entityTypes.size(),
                     m_entityTypes.snapshotHash());
        spdlog::info("Loaded {} item definitions (snapshot {:016x})",
                     m_itemDefinitions.size(),
                     m_itemDefinitions.snapshotHash());
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
