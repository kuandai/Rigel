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

// Classifies save-local generation authority without mutation. Published means
// both documents are canonical, supported, and structurally valid; persistence
// format identity and runtime content references are validated when opening.
SavedWorldGenerationPresence inspectSavedWorldGeneration(
    const PersistenceContext& context);

// Completes a durable publication handoff before creation inputs are resolved.
// The saved snapshot is validated against the runtime block registry before
// any handoff mutation. This never invents a missing backend identity or
// consults installed generator-definition content.
void recoverWorldGenerationPublication(
    PersistenceService& persistence,
    const Voxel::BlockRegistry& registry,
    const PersistenceContext& context);

// Opens a saved generation identity and its authoritative persistence format
// under the per-world bootstrap lock. When the root is still missing, the
// optional creation input is block-registry validated and published atomically.
// This is the only supported world-generation creation lifecycle. Published
// roots without an authoritative backend marker are rejected without mutation.
BootstrappedWorldGeneration bootstrapWorldGeneration(
    const std::optional<NewWorldGeneration>& creation,
    PersistenceService& persistence,
    const Voxel::BlockRegistry& registry,
    const PersistenceContext& context);

SavedWorldGeneration loadSavedWorldGeneration(
    const PersistenceContext& context);

// Validates an already published save-local generation identity and resolves
// its authoritative persistence format without creating or recovering a root.
BootstrappedWorldGeneration loadPublishedWorldGeneration(
    PersistenceService& persistence,
    const Voxel::BlockRegistry& registry,
    const PersistenceContext& context);

} // namespace Rigel::Persistence
