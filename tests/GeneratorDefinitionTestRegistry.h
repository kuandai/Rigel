#pragma once

#include "Rigel/Voxel/BlockRegistry.h"
#include "Rigel/Voxel/WorldGenConfig.h"

#include <utility>

namespace Rigel::Test {

inline void registerGeneratorDefinitionBlocks(
    const Voxel::WorldGenConfig& definition,
    Voxel::BlockRegistry& registry) {
    auto registerBlock = [&registry](const std::string& identifier) {
        if (identifier.empty() || registry.hasIdentifier(identifier)) {
            return;
        }
        Voxel::BlockType block;
        block.identifier = identifier;
        registry.registerBlock(identifier, std::move(block));
    };

    registerBlock(definition.solidBlock);
    registerBlock(definition.surfaceBlock);
    registerBlock(definition.waterBlock);
    registerBlock(definition.shoreBlock);
    for (const auto& biome : definition.biomes.entries) {
        for (const auto& layer : biome.surface) {
            registerBlock(layer.block);
        }
    }
    for (const auto& feature : definition.structures.features) {
        registerBlock(feature.block);
    }
}

} // namespace Rigel::Test
