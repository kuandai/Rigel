#include "Rigel/Voxel/WorldResources.h"

#include "Rigel/Voxel/BlockLoader.h"

#include <spdlog/spdlog.h>

#include <sstream>
#include <stdexcept>

namespace Rigel::Voxel {

namespace {

std::string unusableBlockAssetsMessage(
    const BlockLoadReport& report,
    size_t textureCount
) {
    std::ostringstream message;
    message << "Block assets are unusable: "
            << report.modelsLoaded << " models loaded, "
            << report.modelsFailed << " model definitions failed; "
            << report.loaded << " definitions loaded, "
            << report.failed << " failed, "
            << report.skipped << " skipped ("
            << report.discovered << " discovered); "
            << textureCount << " textures loaded. "
            << "Cosmic Reach runtime assets have not been prepared or are invalid. "
               "Run 'python3 scripts/rigel_assets.py stage /path/to/Cosmic-Reach.jar' "
               "and reconfigure Rigel.";

    if (!report.representativeFailures.empty()) {
        message << " Representative failures: ";
        for (size_t i = 0; i < report.representativeFailures.size(); ++i) {
            if (i != 0) {
                message << "; ";
            }
            const auto& failure = report.representativeFailures[i];
            message << failure.definitionPath << " (" << failure.reason << ')';
        }
    }

    return message.str();
}

} // namespace

void WorldResources::initialize(Asset::AssetManager& assets) {
    if (m_initialized) {
        spdlog::warn("WorldResources::initialize called multiple times");
        return;
    }

    BlockLoader loader;
    BlockLoadReport report = loader.loadFromManifest(
        assets, m_models, m_registry, m_textureAtlas);
    const size_t textureCount = m_textureAtlas.textureCount();
    if (report.modelsFailed != 0 || report.failed != 0 || report.loaded == 0 ||
        m_registry.size() <= 1 ||
        textureCount == 0) {
        throw std::runtime_error(unusableBlockAssetsMessage(report, textureCount));
    }

    m_textureAtlas.upload();
    m_models.freeze();
    m_registry.freeze();
    spdlog::info(
        "world.resources models.loaded={} blocks.loaded={} blocks.failed={} "
        "blocks.skipped={} blocks.discovered={} textures.loaded={}",
        report.modelsLoaded, report.loaded,
        report.failed,
        report.skipped,
        report.discovered,
        textureCount
    );
    m_initialized = true;
}

void WorldResources::releaseRenderResources() {
    m_textureAtlas.releaseGPU();
}

} // namespace Rigel::Voxel
