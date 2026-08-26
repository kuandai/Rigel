#pragma once

#include "Rigel/Persistence/Types.h"
#include "Rigel/Voxel/WorldGenConfig.h"

#include <cstdint>
#include <string>

namespace Rigel::Persistence {

inline constexpr uint32_t kWorldSettingsSchemaVersion = 1;

struct GeneratorProvenance {
    std::string sourceId;
    uint32_t sourceRevision = 0;
    uint32_t definitionSchemaVersion = 0;
    uint32_t semanticsVersion = 0;

    bool operator==(const GeneratorProvenance&) const = default;
};

struct WorldSettings {
    uint32_t schemaVersion = kWorldSettingsSchemaVersion;
    std::string displayName;
    uint32_t seed = 0;
    GeneratorProvenance generator;

    bool operator==(const WorldSettings&) const = default;
};

struct SavedWorldGeneration {
    WorldSettings settings;
    Voxel::WorldGenConfig definition;
};

enum class SavedWorldGenerationPresence {
    Missing,
    Published,
    LegacyOrIncomplete
};

SavedWorldGenerationPresence inspectSavedWorldGeneration(
    const PersistenceContext& context);

void publishNewWorldGeneration(const WorldSettings& settings,
                               const Voxel::WorldGenConfig& definition,
                               const PersistenceContext& context);

SavedWorldGeneration loadSavedWorldGeneration(
    const PersistenceContext& context);

} // namespace Rigel::Persistence
