#pragma once

#include "WorldGenConfig.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace Rigel::Voxel {

class BlockRegistry;

inline constexpr uint32_t kGeneratorDefinitionSchemaVersion = 1;
inline constexpr uint32_t kGeneratorSemanticsVersion = 1;

std::string serializeGeneratorSnapshot(const WorldGenConfig& definition);

void validateGeneratorSnapshotContent(const WorldGenConfig& definition,
                                      const BlockRegistry& registry);

WorldGenConfig parseGeneratorSnapshot(std::string_view snapshot,
                                      uint32_t definitionSchemaVersion,
                                      uint32_t seed,
                                      uint32_t runtimeGenerationVersion);

} // namespace Rigel::Voxel
