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

struct CreationLayer {
    std::optional<GeneratorDefinitionSource> generatorSource;
    bool changesDefinition = false;
};

bool isGeneratorDefinitionField(std::string_view field) {
    return field == "solid_block" || field == "surface_block" ||
        field == "water_block" || field == "shore_block" ||
        field == "world" || field == "terrain" || field == "climate" ||
        field == "biomes" || field == "density_graph" || field == "caves" ||
        field == "structures" || field == "generation";
}

GeneratorDefinitionSource parseGeneratorSource(
    const char* sourceName,
    ryml::ConstNodeRef generator) {
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

CreationLayer inspectCreationLayer(const char* sourceName,
                                   const std::string& yaml) {
    CreationLayer result;
    if (yaml.empty()) {
        return result;
    }
    const ryml::Tree tree = ryml::parse_in_arena(
        ryml::to_csubstr(sourceName), ryml::to_csubstr(yaml));
    const ryml::ConstNodeRef root = tree.rootref();
    if (!root.is_map()) {
        return result;
    }
    for (const ryml::ConstNodeRef field : root.children()) {
        result.changesDefinition = result.changesDefinition ||
            isGeneratorDefinitionField(Util::toStdString(field.key()));
    }
    if (root.has_child("generator")) {
        result.generatorSource =
            parseGeneratorSource(sourceName, root["generator"]);
    }
    return result;
}

void validateSourceIdentityCoupling(const char* sourceName,
                                    const CreationLayer& source,
                                    bool changesDefinition) {
    if (changesDefinition && !source.generatorSource) {
        throw std::invalid_argument(
            "Generator definition fields in '" + std::string(sourceName) +
            "' require a generator source ID and revision in the same source");
    }
    if (!changesDefinition && source.generatorSource) {
        throw std::invalid_argument(
            "Generator source identity in '" + std::string(sourceName) +
            "' requires generator definition fields in the same source");
    }
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
        bool changesDefinition = false;
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

            const CreationLayer overlayLayer = inspectCreationLayer(
                overlayData->name.c_str(), overlayData->content);
            if (overlayLayer.generatorSource) {
                throw std::invalid_argument(
                    "Generator source identity must be declared by '" +
                    source.name() + "', not by overlay '" +
                    overlayData->name + "'");
            }
            changesDefinition =
                changesDefinition || overlayLayer.changesDefinition;
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
        return changesDefinition;
    };

    for (const auto& source : m_sources) {
        auto yaml = source->load();
        if (!yaml) {
            continue;
        }
        const CreationLayer sourceLayer =
            inspectCreationLayer(source->name().c_str(), *yaml);
        WorldConfiguration candidate = config;
        auto overlays = candidate.generation.applyCreationYamlWithOverlays(
            source->name().c_str(), *yaml);
        candidate.streaming.applyYaml(source->name().c_str(), *yaml);
        const bool overlayChangesDefinition =
            applyOverlays(candidate, *source, std::move(overlays));
        validateSourceIdentityCoupling(
            source->name().c_str(), sourceLayer,
            sourceLayer.changesDefinition || overlayChangesDefinition);
        if (sourceLayer.generatorSource) {
            candidate.generatorSource = *sourceLayer.generatorSource;
        }
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
