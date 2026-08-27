#include "Rigel/Voxel/GeneratorDefinitionLoader.h"

#include "Rigel/Asset/AssetLoader.h"
#include "Rigel/Asset/AssetManager.h"
#include "Rigel/Util/Ryml.h"
#include "Rigel/Voxel/BlockRegistry.h"

#include <ryml.hpp>

#include <algorithm>
#include <functional>
#include <stdexcept>
#include <string>

namespace Rigel::Voxel {

struct GeneratorDefinitionAssetTransactionAccess {
    static bool hasPending(const Asset::AssetManager& assets) {
        return assets.hasPendingGeneratorDefinitions();
    }

    static void forEachCandidate(
        const Asset::AssetManager& assets,
        const std::function<void(
            const std::string&,
            const Asset::AssetManager::AssetEntry&)>& visitor) {
        assets.forEachGeneratorDefinitionCandidate(visitor);
    }

    static void rethrowDeclarationError(
        const Asset::AssetManager& assets) {
        assets.rethrowPendingGeneratorDefinitionError();
    }

    static void commit(Asset::AssetManager& assets) {
        assets.commitPendingGeneratorDefinitions();
    }

    static void discard(Asset::AssetManager& assets) {
        assets.discardPendingGeneratorDefinitions();
    }
};

namespace {

struct GeneratorDefinitionAsset final : Asset::AssetBase {
    GeneratorDefinition definition;
};

std::vector<GeneratorDefinition> validateAndOrderGeneratorDefinitions(
    std::vector<GeneratorDefinition> definitions,
    const BlockRegistry& registry);

std::string readDefinitionPath(const Asset::LoadContext& context) {
    if (!context.config.readable() || !context.config.is_map()) {
        throw Asset::AssetLoadError(
            context.id, "generator definition declaration must be a mapping");
    }
    bool foundPath = false;
    std::string path;
    for (const ryml::ConstNodeRef field : context.config.children()) {
        const std::string key = Util::toStdString(field.key());
        if (key != "path") {
            throw Asset::AssetLoadError(
                context.id,
                "unknown generator definition declaration field '" + key +
                    "'");
        }
        if (foundPath) {
            throw Asset::AssetLoadError(
                context.id,
                "duplicate generator definition declaration field 'path'");
        }
        if (!field.has_val() || field.has_children()) {
            throw Asset::AssetLoadError(
                context.id,
                "generator definition declaration path must be a scalar");
        }
        path = Util::toStdString(field.val());
        foundPath = true;
    }
    if (!foundPath || path.empty()) {
        throw Asset::AssetLoadError(
            context.id,
            "generator definition declaration requires a non-empty path");
    }
    return path;
}

class GeneratorDefinitionAssetLoader final : public Asset::IAssetLoader {
public:
    std::string_view category() const override {
        return "generator_definitions";
    }

    std::shared_ptr<Asset::AssetBase> load(
        const Asset::LoadContext& context) override {
        try {
            const std::string path = readDefinitionPath(context);
            const std::span<const char> source = context.loadResource(path);
            auto result = std::make_shared<GeneratorDefinitionAsset>();
            result->definition = parseGeneratorDefinition(
                std::string_view(source.data(), source.size()), path);
            return result;
        } catch (const Asset::AssetLoadError&) {
            throw;
        } catch (const std::exception& error) {
            throw Asset::AssetLoadError(context.id, error.what());
        }
    }
};

std::vector<GeneratorDefinition> loadAndValidateGeneratorDefinitionCandidate(
    Asset::AssetManager& assets,
    const BlockRegistry& registry) {
    GeneratorDefinitionAssetTransactionAccess::rethrowDeclarationError(
        assets);
    std::vector<std::pair<
        std::string, const Asset::AssetManager::AssetEntry*>> declarations;
    GeneratorDefinitionAssetTransactionAccess::forEachCandidate(
        assets,
        [&](const std::string& name,
            const Asset::AssetManager::AssetEntry& entry) {
            declarations.emplace_back(name, &entry);
        });
    std::sort(declarations.begin(), declarations.end(),
              [](const auto& left, const auto& right) {
                  return left.first < right.first;
              });

    GeneratorDefinitionAssetLoader loader;
    std::vector<GeneratorDefinition> definitions;
    definitions.reserve(declarations.size());
    for (const auto& [name, entry] : declarations) {
        const std::string id = "generator_definitions/" + name;
        Asset::LoadContext context{id, entry->config, assets};
        const auto loaded =
            std::dynamic_pointer_cast<GeneratorDefinitionAsset>(
                loader.load(context));
        if (!loaded) {
            throw Asset::AssetLoadError(
                id,
                "Generator definition loader returned an incompatible asset");
        }
        definitions.push_back(loaded->definition);
    }
    return validateAndOrderGeneratorDefinitions(
        std::move(definitions), registry);
}

std::vector<GeneratorDefinition> validateAndOrderGeneratorDefinitions(
    std::vector<GeneratorDefinition> definitions,
    const BlockRegistry& registry) {
    if (definitions.empty()) {
        throw std::invalid_argument(
            "At least one generator definition must be declared");
    }
    std::sort(definitions.begin(), definitions.end(),
              [](const auto& left, const auto& right) {
                  if (left.id != right.id) {
                      return left.id < right.id;
                  }
                  if (left.sourceRevision != right.sourceRevision) {
                      return left.sourceRevision < right.sourceRevision;
                  }
                  return left.label < right.label;
              });

    for (size_t index = 1; index < definitions.size(); ++index) {
        if (definitions[index - 1].id == definitions[index].id) {
            throw std::invalid_argument(
                "Duplicate generator definition ID '" +
                definitions[index].id + "' at revisions " +
                std::to_string(definitions[index - 1].sourceRevision) +
                " and " +
                std::to_string(definitions[index].sourceRevision));
        }
    }
    for (const auto& definition : definitions) {
        static_cast<void>(prepareGeneratorDefinitionSnapshot(
            definition, registry));
    }
    return definitions;
}

} // namespace

PreparedGeneratorDefinitionSnapshot loadPreparedGeneratorDefinitionSnapshot(
    Asset::AssetManager& assets,
    const BlockRegistry& registry,
    std::string_view selectedId) {
    const bool candidatePending =
        GeneratorDefinitionAssetTransactionAccess::hasPending(assets);
    try {
        const std::vector<GeneratorDefinition> definitions =
            loadAndValidateGeneratorDefinitionCandidate(
                assets, registry);
        const auto selected = std::find_if(
            definitions.begin(), definitions.end(),
            [&](const auto& definition) {
                return definition.id == selectedId;
            });
        if (selected == definitions.end()) {
            throw Asset::AssetLoadError(
                "generator_definitions",
                "Selected generator definition is not declared: " +
                    std::string(selectedId));
        }
        auto prepared = prepareGeneratorDefinitionSnapshot(
            *selected, registry);
        if (candidatePending) {
            GeneratorDefinitionAssetTransactionAccess::commit(assets);
        }
        return prepared;
    } catch (const Asset::AssetLoadError&) {
        if (candidatePending) {
            GeneratorDefinitionAssetTransactionAccess::discard(assets);
        }
        throw;
    } catch (const std::exception& error) {
        if (candidatePending) {
            GeneratorDefinitionAssetTransactionAccess::discard(assets);
        }
        throw Asset::AssetLoadError(
            "generator_definitions", error.what());
    }
}

} // namespace Rigel::Voxel
