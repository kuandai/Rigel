#include "Rigel/Voxel/WorldConfigProvider.h"

#include <iterator>
#include <unordered_set>

namespace Rigel::Voxel {

void WorldConfigProvider::addSource(
    std::unique_ptr<Config::IConfigSource> source) {
    m_sources.push_back(std::move(source));
}

WorldConfiguration WorldConfigProvider::loadConfig() const {
    WorldConfiguration config;
    auto applyOverlays = [&config](
        const Config::IConfigSource& source,
        std::vector<WorldGenConfig::OverlayConfig> pending) {
        std::unordered_set<std::string> appliedPaths;
        size_t overlayIndex = 0;
        while (overlayIndex < pending.size()) {
            WorldGenConfig::OverlayConfig overlay = std::move(pending[overlayIndex]);
            ++overlayIndex;
            if (!overlay.when.empty() &&
                !config.generation.isFlagEnabled(overlay.when)) {
                continue;
            }
            if (!appliedPaths.insert(overlay.path).second) {
                continue;
            }

            auto overlayData = source.loadPath(overlay.path);
            if (!overlayData) {
                continue;
            }

            auto nestedOverlays = config.generation.applyYamlWithOverlays(
                overlayData->name.c_str(),
                overlayData->content
            );
            config.streaming.applyYaml(
                overlayData->name.c_str(), overlayData->content);
            pending.insert(
                pending.end(),
                std::make_move_iterator(nestedOverlays.begin()),
                std::make_move_iterator(nestedOverlays.end())
            );
        }
    };

    for (const auto& source : m_sources) {
        auto yaml = source->load();
        if (!yaml) {
            continue;
        }
        auto overlays = config.generation.applyYamlWithOverlays(
            source->name().c_str(), *yaml);
        config.streaming.applyYaml(source->name().c_str(), *yaml);
        applyOverlays(*source, std::move(overlays));
    }

    return config;
}

} // namespace Rigel::Voxel
