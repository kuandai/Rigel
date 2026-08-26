#pragma once

#include "Rigel/Persistence/WorldSettings.h"
#include "Rigel/Voxel/WorldSet.h"

#include <memory>
#include <optional>
#include <string>

namespace Rigel::detail {

struct ApplicationWorldGenerationBootstrapResult {
    std::shared_ptr<const Voxel::WorldGenerator> generator;
    Persistence::WorldSettings settings;
    std::string persistenceFormat;
};

ApplicationWorldGenerationBootstrapResult bootstrapApplicationWorldGeneration(
    Voxel::WorldSet& worldSet,
    Voxel::WorldId worldId,
    Voxel::World& world,
    Voxel::WorldView& worldView,
    const std::optional<Persistence::NewWorldGeneration>& creation,
    const Persistence::PersistenceContext& context);

} // namespace Rigel::detail
