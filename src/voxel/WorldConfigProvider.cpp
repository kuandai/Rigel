#include "Rigel/Voxel/WorldConfigProvider.h"

#include "Rigel/Util/Ryml.h"

#include <ryml.hpp>

#include <charconv>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <unordered_set>

namespace Rigel::Voxel {
namespace {

bool validGeneratorSourceId(std::string_view id) {
    const size_t separator = id.find(':');
    if (separator == std::string_view::npos || separator == 0 ||
        separator + 1 == id.size()) {
        return false;
    }
    for (const unsigned char byte : id) {
        if (byte <= 0x20 || byte == 0x7f) {
            return false;
        }
    }
    return true;
}

std::optional<GeneratorDefinitionSource> readGeneratorSource(
    const char* sourceName,
    const std::string& yaml) {
    if (yaml.empty()) {
        return std::nullopt;
    }
    const ryml::Tree tree = ryml::parse_in_arena(
        ryml::to_csubstr(sourceName), ryml::to_csubstr(yaml));
    const ryml::ConstNodeRef root = tree.rootref();
    if (!root.has_child("generator")) {
        return std::nullopt;
    }
    const ryml::ConstNodeRef generator = root["generator"];
    if (!generator.is_map()) {
        throw std::invalid_argument(
            "Generator definition source in '" + std::string(sourceName) +
            "' must be a mapping");
    }
    std::unordered_set<std::string> fields;
    for (const ryml::ConstNodeRef field : generator.children()) {
        const std::string name = Util::toStdString(field.key());
        if (name != "id" && name != "source_revision") {
            throw std::invalid_argument(
                "Unknown generator definition field 'generator." + name +
                "' in '" + sourceName + "'");
        }
        if (!fields.insert(name).second) {
            throw std::invalid_argument(
                "Duplicate generator definition field 'generator." + name +
                "' in '" + sourceName + "'");
        }
    }
    for (const char* required : {"id", "source_revision"}) {
        if (!generator.has_child(required)) {
            throw std::invalid_argument(
                "Missing generator definition field 'generator." +
                std::string(required) + "' in '" + sourceName + "'");
        }
    }

    GeneratorDefinitionSource result;
    generator["id"] >> result.id;
    if (!validGeneratorSourceId(result.id)) {
        throw std::invalid_argument(
            "Generator definition field 'generator.id' in '" +
            std::string(sourceName) + "' must be a non-empty namespaced ID");
    }

    const std::string revision = Util::toStdString(
        generator["source_revision"].val());
    const char* begin = revision.data();
    const char* end = begin + revision.size();
    const auto parsed = std::from_chars(begin, end, result.revision);
    if (parsed.ec != std::errc{} || parsed.ptr != end ||
        result.revision == 0) {
        throw std::invalid_argument(
            "Generator definition field 'generator.source_revision' in '" +
            std::string(sourceName) +
            "' must be an unsigned integer greater than zero");
    }
    return result;
}

void validateResolvedGeneratorSource(
    const GeneratorDefinitionSource& source) {
    if (!validGeneratorSourceId(source.id) || source.revision == 0) {
        throw std::invalid_argument(
            "Merged world configuration requires a generator definition "
            "source ID and revision");
    }
}

} // namespace

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

            if (auto generatorSource = readGeneratorSource(
                    overlayData->name.c_str(), overlayData->content)) {
                target.generatorSource = std::move(*generatorSource);
            }
            auto nestedOverlays =
                target.generation.applyCreationYamlWithOverlays(
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
        if (auto generatorSource = readGeneratorSource(
                source->name().c_str(), *yaml)) {
            candidate.generatorSource = std::move(*generatorSource);
        }
        auto overlays = candidate.generation.applyCreationYamlWithOverlays(
            source->name().c_str(), *yaml);
        candidate.streaming.applyYaml(source->name().c_str(), *yaml);
        applyOverlays(candidate, *source, std::move(overlays));
        config = std::move(candidate);
    }

    config.generation.validate("merged world configuration");
    config.streaming.validate("merged world configuration");
    validateResolvedGeneratorSource(config.generatorSource);

    return config;
}

StreamingConfig WorldConfigProvider::loadStreamingConfig() const {
    StreamingConfig config;
    WorldGenConfig routing;
    auto applyOverlays = [](
        StreamingConfig& target,
        WorldGenConfig& routingState,
        const Config::IConfigSource& source,
        std::vector<WorldGenConfig::OverlayConfig> pending) {
        std::unordered_set<std::string> appliedPaths;
        size_t overlayIndex = 0;
        while (overlayIndex < pending.size()) {
            WorldGenConfig::OverlayConfig overlay =
                std::move(pending[overlayIndex]);
            ++overlayIndex;
            if (!overlay.when.empty() &&
                !routingState.isFlagEnabled(overlay.when)) {
                continue;
            }
            if (!appliedPaths.insert(overlay.path).second) {
                continue;
            }

            std::optional<Config::ConfigSourceResult> overlayData;
            try {
                overlayData = source.loadPath(overlay.path);
            } catch (const Config::ConfigPathNotFound&) {
                continue;
            }
            if (!overlayData) {
                continue;
            }
            auto nestedOverlays = routingState.applyYamlRouting(
                overlayData->name.c_str(), overlayData->content);
            target.applyYaml(
                overlayData->name.c_str(), overlayData->content);
            pending.insert(
                pending.end(),
                std::make_move_iterator(nestedOverlays.begin()),
                std::make_move_iterator(nestedOverlays.end()));
        }
    };

    for (const auto& source : m_sources) {
        auto yaml = source->load();
        if (!yaml) {
            continue;
        }
        StreamingConfig candidate = config;
        WorldGenConfig routingCandidate = routing;
        auto overlays = routingCandidate.applyYamlRouting(
            source->name().c_str(), *yaml);
        candidate.applyYaml(source->name().c_str(), *yaml);
        applyOverlays(
            candidate,
            routingCandidate,
            *source,
            std::move(overlays));
        config = std::move(candidate);
        routing = std::move(routingCandidate);
    }
    config.validate("merged streaming configuration");
    return config;
}

} // namespace Rigel::Voxel
