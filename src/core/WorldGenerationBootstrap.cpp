#include "WorldGenerationBootstrap.h"

#include <stdexcept>
#include <utility>

namespace Rigel::detail {
namespace {

void validateRuntimeTargets(
    Voxel::WorldSet& worldSet,
    Voxel::WorldId worldId,
    Voxel::World& world,
    Voxel::WorldView& worldView) {
    if (!worldSet.hasWorld(worldId)
        || &worldSet.world(worldId) != &world
        || world.id() != worldId) {
        throw std::invalid_argument(
            "World generation target is not owned by the world set");
    }
    if (world.generator()) {
        throw std::invalid_argument(
            "World generation target already owns a generator");
    }
    if (&worldView.world() != &world || worldView.generator()) {
        throw std::invalid_argument(
            "World generation view target does not match the world");
    }
}

} // namespace

ApplicationWorldGenerationBootstrapResult bootstrapApplicationWorldGeneration(
    Voxel::WorldSet& worldSet,
    Voxel::WorldId worldId,
    Voxel::World& world,
    Voxel::WorldView& worldView,
    const Persistence::NewWorldGenerationFactory& creationFactory,
    const Persistence::PersistenceContext& context) {
    validateRuntimeTargets(worldSet, worldId, world, worldView);

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

} // namespace Rigel::detail
