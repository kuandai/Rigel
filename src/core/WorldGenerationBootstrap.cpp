#include "WorldGenerationBootstrap.h"

#include "Rigel/Voxel/GeneratorSnapshot.h"

#include <utility>

namespace Rigel::detail {

ApplicationWorldGenerationBootstrapResult bootstrapApplicationWorldGeneration(
    Voxel::WorldSet& worldSet,
    Voxel::WorldId worldId,
    Voxel::World& world,
    Voxel::WorldView& worldView,
    const std::optional<Persistence::NewWorldGeneration>& creation,
    const Persistence::PersistenceContext& context) {
    const Voxel::BlockRegistry& registry = worldSet.resources().registry();
    Persistence::BootstrappedWorldGeneration bootstrapped =
        Persistence::bootstrapWorldGeneration(
            creation, worldSet.persistenceService(), registry, context);
    Voxel::validateGeneratorSnapshotContent(
        bootstrapped.generation.definition, registry);
    auto generator = std::make_shared<const Voxel::WorldGenerator>(
        registry, std::move(bootstrapped.generation.definition));

    worldSet.setPersistenceActiveFormat(
        worldId, bootstrapped.persistenceFormat);
    world.setGenerator(generator);
    worldView.setGenerator(generator);

    ApplicationWorldGenerationBootstrapResult result;
    result.generator = std::move(generator);
    result.settings = std::move(bootstrapped.generation.settings);
    result.persistenceFormat = std::move(bootstrapped.persistenceFormat);
    return result;
}

} // namespace Rigel::detail
