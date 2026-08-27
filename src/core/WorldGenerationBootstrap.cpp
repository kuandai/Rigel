#include "WorldGenerationBootstrap.h"

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
    const Persistence::SavedWorldGenerationPresence presence =
        Persistence::inspectSavedWorldGeneration(context);
    std::shared_ptr<const Voxel::WorldGenerator> preparedGenerator;
    if (presence == Persistence::SavedWorldGenerationPresence::Missing &&
        creation) {
        preparedGenerator = std::make_shared<const Voxel::WorldGenerator>(
            registry,
            creation->definition.data,
            creation->seed,
            Voxel::kGeneratorSemanticsVersion);
    }
    Persistence::BootstrappedWorldGeneration bootstrapped =
        Persistence::bootstrapWorldGeneration(
            creation, worldSet.persistenceService(), registry, context);
    Voxel::validateGeneratorDefinitionContent(
        bootstrapped.generation.definition,
        registry,
        bootstrapped.generation.settings.generator.sourceId);
    const bool preparedMatchesPublished = preparedGenerator &&
        preparedGenerator->seed() == bootstrapped.generation.settings.seed &&
        preparedGenerator->semanticsVersion() ==
            bootstrapped.generation.settings.generator.semanticsVersion &&
        preparedGenerator->definition() == bootstrapped.generation.definition;
    auto generator = preparedMatchesPublished
        ? std::move(preparedGenerator)
        : std::make_shared<const Voxel::WorldGenerator>(
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
