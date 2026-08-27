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
    const BlockRegistry& registry);

std::vector<GeneratorDefinition> loadDeclaredGeneratorDefinitions(
    Asset::AssetManager& assets,
    const BlockRegistry& registry);

} // namespace Rigel::Voxel
