#include "WorldGenerationBootstrap.h"

#include <utility>

namespace Rigel::detail {

ApplicationWorldGenerationBootstrapResult bootstrapApplicationWorldGeneration(
    Voxel::WorldSet& worldSet,
    Voxel::WorldId worldId,
    Voxel::World& world,
    Voxel::WorldView& worldView,
    const Persistence::NewWorldGenerationFactory& creationFactory,
    const Persistence::PersistenceContext& context) {
    const Voxel::BlockRegistry& registry = worldSet.resources().registry();
    Persistence::BootstrappedWorldGeneration bootstrapped =
        Persistence::bootstrapWorldGeneration(
            creationFactory,
            worldSet.persistenceService(),
            registry,
            context);
    Voxel::validateGeneratorDefinitionContent(
        bootstrapped.generation.definition,
        registry,
        bootstrapped.generation.settings.generator.sourceId);
    auto generator = std::make_shared<const Voxel::WorldGenerator>(
        registry,
        std::move(bootstrapped.generation.definition),
        bootstrapped.generation.settings.seed,
        bootstrapped.generation.settings.generator.semanticsVersion);

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

ApplicationWorldGenerationBootstrapResult bootstrapApplicationWorldGeneration(
    Voxel::WorldSet& worldSet,
    Voxel::WorldId worldId,
    Voxel::World& world,
    Voxel::WorldView& worldView,
    const std::optional<Persistence::NewWorldGeneration>& preparedCreation,
    const Persistence::PersistenceContext& context) {
    Persistence::NewWorldGenerationFactory factory;
    if (preparedCreation) {
        factory = [creation = *preparedCreation] { return creation; };
    }
    return bootstrapApplicationWorldGeneration(
        worldSet, worldId, world, worldView, factory, context);
}

} // namespace Rigel::detail
