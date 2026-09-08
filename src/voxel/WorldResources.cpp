#include "Rigel/Voxel/WorldResources.h"

#include "Rigel/Voxel/BlockLoader.h"

#include <spdlog/spdlog.h>

#include <sstream>
#include <stdexcept>

namespace Rigel::Voxel {

namespace {

std::string unusableBlockAssetsMessage(const BlockLoadReport& report) {
    std::ostringstream message;
    message << "Block assets are unusable: "
            << report.modelsLoaded << " models loaded, "
            << report.modelsFailed << " model definitions failed; "
            << report.loaded << " definitions loaded, "
            << report.failed << " failed, "
            << report.skipped << " skipped ("
            << report.discovered << " discovered). "
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

    BlockModelRegistry models;
    BlockRegistry registry;
    BlockLoader loader;
    BlockLoadReport report = loader.loadFromManifest(
        assets, models, registry);
    if (report.modelsFailed != 0 || report.failed != 0 || report.loaded == 0 ||
        registry.size() <= 1) {
        throw std::runtime_error(unusableBlockAssetsMessage(report));
    }

    registry.freeze();
    spdlog::info(
        "world.resources models.loaded={} blocks.loaded={} blocks.failed={} "
        "blocks.skipped={} blocks.discovered={}",
        report.modelsLoaded, report.loaded,
        report.failed,
        report.skipped,
        report.discovered
    );
    m_registry.swap(registry);
    m_initialized = true;
}

} // namespace Rigel::Voxel
