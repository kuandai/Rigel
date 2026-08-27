#pragma once

#include "GeneratorDefinition.h"

namespace Rigel::Asset {
class AssetManager;
}

namespace Rigel::Voxel {

class BlockRegistry;

PreparedGeneratorDefinitionSnapshot loadPreparedGeneratorDefinitionSnapshot(
    Asset::AssetManager& assets,
    const BlockRegistry& registry,
    std::string_view selectedId);

} // namespace Rigel::Voxel
