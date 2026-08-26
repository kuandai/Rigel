#include "Rigel/Voxel/WorldConfigProvider.h"

#include <iterator>
#include <stdexcept>
#include <unordered_set>

namespace Rigel::Voxel {

void WorldConfigProvider::addSource(
    std::unique_ptr<Config::IConfigSource> source) {
    m_sources.push_back(std::move(source));
}

WorldConfiguration WorldConfigProvider::loadConfig() const {
    WorldConfiguration config;
    auto applyOverlays = [](
        WorldConfiguration& target,
        const Config::IConfigSource& source,
        std::vector<WorldGenConfig::OverlayConfig> pending) {
        std::unordered_set<std::string> appliedPaths;
        size_t overlayIndex = 0;
        while (overlayIndex < pending.size()) {
            WorldGenConfig::OverlayConfig overlay = std::move(pending[overlayIndex]);
            ++overlayIndex;
            if (!overlay.when.empty() &&
                !target.generation.isFlagEnabled(overlay.when)) {
                continue;
            }
            if (!appliedPaths.insert(overlay.path).second) {
                continue;
            }

            auto overlayData = source.loadPath(overlay.path);
            if (!overlayData) {
                throw std::runtime_error(
                    "Missing configuration overlay '" + overlay.path +
                    "' declared by '" + source.name() + "'");
            }

            auto nestedOverlays = target.generation.applyYamlWithOverlays(
                overlayData->name.c_str(),
                overlayData->content
            );
            target.streaming.applyYaml(
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
        WorldConfiguration candidate = config;
        auto overlays = candidate.generation.applyYamlWithOverlays(
            source->name().c_str(), *yaml);
        candidate.streaming.applyYaml(source->name().c_str(), *yaml);
        applyOverlays(candidate, *source, std::move(overlays));
        config = std::move(candidate);
    }

    config.generation.validate("merged world configuration");
    config.streaming.validate("merged world configuration");

    return config;
}

StreamingConfig WorldConfigProvider::loadStreamingConfig() const {
    StreamingConfig config;
    for (const auto& source : m_sources) {
        auto yaml = source->load();
        if (!yaml) {
            continue;
        }
        StreamingConfig candidate = config;
        candidate.applyYaml(source->name().c_str(), *yaml);
        config = std::move(candidate);
    }
    config.validate("merged streaming configuration");
    return config;
}

} // namespace Rigel::Voxel
