#pragma once

#include "Rigel/Persistence/Types.h"
#include "Rigel/Voxel/WorldGenConfig.h"

#include <cstdint>
#include <optional>
#include <string>

namespace Rigel::Voxel {
class BlockRegistry;
}

namespace Rigel::Persistence {

class PersistenceService;

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

struct NewWorldGeneration {
    WorldSettings settings;
    Voxel::WorldGenConfig definition;
};

struct BootstrappedWorldGeneration {
    SavedWorldGeneration generation;
    std::string persistenceFormat;
};

enum class SavedWorldGenerationPresence {
    Missing,
    Published,
    LegacyOrIncomplete
};

SavedWorldGenerationPresence inspectSavedWorldGeneration(
    const PersistenceContext& context);

// Cleans deletion-authorized abandoned staging. Non-deleting publication
// handoffs are preserved here and completed by bootstrapWorldGeneration(),
// which can validate their saved identity and persistence format.
void recoverAbandonedWorldGenerationStaging(
    const PersistenceContext& context);

// Selects and durably publishes the persistence backend identity in the same
// atomic directory publication as the save-owned settings and definition.
// Returns the selected format ID for subsequent operations on this world.
std::string publishNewWorldGeneration(
    const WorldSettings& settings,
    const Voxel::WorldGenConfig& definition,
    PersistenceService& persistence,
    const PersistenceContext& context);

// Opens a saved generation identity and its authoritative persistence format
// under the per-world bootstrap lock. When the root is still missing, the
// optional creation input is published atomically. Older identity-only roots
// are claimed by exactly one preferred backend while the same lock is held.
BootstrappedWorldGeneration bootstrapWorldGeneration(
    const std::optional<NewWorldGeneration>& creation,
    PersistenceService& persistence,
    const Voxel::BlockRegistry& registry,
    const PersistenceContext& context);

SavedWorldGeneration loadSavedWorldGeneration(
    const PersistenceContext& context);

} // namespace Rigel::Persistence
