#pragma once

#include "GeneratorDefinition.h"

#include <vector>

namespace Rigel::Asset {
class AssetManager;
}

namespace Rigel::Voxel {

class BlockRegistry;

std::vector<GeneratorDefinition> validateAndOrderGeneratorDefinitions(
    std::vector<GeneratorDefinition> definitions,
    const BlockRegistry& registry,
    GeneratorDefinitionOrigin origin);

std::vector<GeneratorDefinition> loadDeclaredGeneratorDefinitions(
    Asset::AssetManager& assets,
    const BlockRegistry& registry,
    GeneratorDefinitionOrigin origin);

PreparedGeneratorDefinitionSnapshot loadPreparedGeneratorDefinitionSnapshot(
    Asset::AssetManager& assets,
    const BlockRegistry& registry,
    std::string_view selectedId,
    GeneratorDefinitionOrigin origin);

} // namespace Rigel::Voxel
