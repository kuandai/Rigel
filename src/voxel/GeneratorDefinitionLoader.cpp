#include "Rigel/Voxel/GeneratorDefinitionLoader.h"

#include "Rigel/Asset/AssetLoader.h"
#include "Rigel/Asset/AssetManager.h"
#include "Rigel/Util/Ryml.h"
#include "Rigel/Voxel/BlockRegistry.h"

#include <ryml.hpp>

#include <algorithm>
#include <stdexcept>
#include <string>

namespace Rigel::Voxel {
namespace {

struct GeneratorDefinitionAsset final : Asset::AssetBase {
    GeneratorDefinition definition;
};

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
        const std::string path = readDefinitionPath(context);
        const std::span<const char> source = context.loadResource(path);
        try {
            auto result = std::make_shared<GeneratorDefinitionAsset>();
            result->definition = parseGeneratorDefinition(
                std::string_view(source.data(), source.size()), path);
            return result;
        } catch (const std::exception& error) {
            throw Asset::AssetLoadError(context.id, error.what());
        }
    }
};

} // namespace

std::vector<GeneratorDefinition> validateAndOrderGeneratorDefinitions(
    std::vector<GeneratorDefinition> definitions,
    const BlockRegistry& registry,
    GeneratorDefinitionOrigin origin) {
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
        if (definitions[index - 1].id == definitions[index].id &&
            definitions[index - 1].sourceRevision ==
                definitions[index].sourceRevision) {
            throw std::invalid_argument(
                "Duplicate generator definition identity '" +
                definitions[index].id + "@" +
                std::to_string(definitions[index].sourceRevision) + "'");
        }
    }
    for (const auto& definition : definitions) {
        static_cast<void>(prepareGeneratorDefinitionSnapshot(
            definition, registry, origin));
    }
    return definitions;
}

std::vector<GeneratorDefinition> loadDeclaredGeneratorDefinitions(
    Asset::AssetManager& assets,
    const BlockRegistry& registry,
    GeneratorDefinitionOrigin origin) {
    assets.registerLoader(
        "generator_definitions",
        std::make_unique<GeneratorDefinitionAssetLoader>());

    std::vector<std::string> declarationNames;
    assets.forEachInCategory(
        "generator_definitions",
        [&](const std::string& name, const Asset::AssetManager::AssetEntry&) {
            declarationNames.push_back(name);
        });
    std::sort(declarationNames.begin(), declarationNames.end());

    std::vector<GeneratorDefinition> definitions;
    definitions.reserve(declarationNames.size());
    for (const auto& name : declarationNames) {
        definitions.push_back(
            assets.get<GeneratorDefinitionAsset>(
                "generator_definitions/" + name)->definition);
    }
    return validateAndOrderGeneratorDefinitions(
        std::move(definitions), registry, origin);
}

} // namespace Rigel::Voxel
