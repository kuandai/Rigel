#include "TestFramework.h"

#include "Rigel/Asset/AssetManager.h"
#include "Rigel/Entity/Entity.h"
#include "Rigel/Entity/EntityModel.h"
#include "Rigel/Entity/EntityModelLoader.h"
#include "Rigel/Voxel/BlockLoader.h"
#include "Rigel/Voxel/GeneratorDefinitionLoader.h"
#include "Rigel/Voxel/World.h"
#include "Rigel/Voxel/WorldGenerator.h"
#include "Rigel/Voxel/WorldResources.h"

#include <array>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#if defined(__gl_h_) || defined(__glew_h__) || defined(_glfw3_h_) || \
    defined(__egl_h_)
#error "CPU semantic public headers must not include graphics APIs"
#endif

namespace {

using namespace Rigel;

Voxel::BlockModelDefinitionSource modelDefinition(
    std::string_view path,
    std::string_view source
) {
    return {path, std::span<const char>(source.data(), source.size())};
}

Voxel::BlockDefinitionSource blockDefinition(
    std::string_view path,
    std::string_view source
) {
    return {path, std::span<const char>(source.data(), source.size())};
}

} // namespace

TEST_CASE(CpuSemanticLifecycle_LoadGenerateTickAndDestroy) {
    constexpr std::string_view modelYaml = R"(
id: semantic_column
texture_slots: [surface]
cuboids:
  - bounds: [0.25, 0, 0.25, 0.75, 1, 0.75]
    faces:
      pos_x: {texture: surface}
      neg_x: {texture: surface}
)";
    const std::array materialIds = {
        std::string_view{"stone_shale"},
        std::string_view{"water[type=source]"},
        std::string_view{"grass"},
        std::string_view{"dirt"},
        std::string_view{"sand"},
    };
    std::vector<std::string> blockDocuments;
    std::vector<Voxel::BlockDefinitionSource> blockDefinitions;
    blockDocuments.reserve(materialIds.size());
    blockDefinitions.reserve(materialIds.size());
    for (const std::string_view id : materialIds) {
        blockDocuments.push_back(
            "id: " + std::string(id) +
            "\nmodel: semantic_column\ncollision: full\n"
            "textures: {surface: textures/synthetic/semantic.png}\n");
    }
    for (size_t index = 0; index < materialIds.size(); ++index) {
        blockDefinitions.push_back(blockDefinition(
            materialIds[index], blockDocuments[index]));
    }

    Asset::AssetManager assets;
    assets.registerLoader(
        "entity_models", std::make_unique<Entity::EntityModelLoader>());
    assets.registerLoader(
        "entity_anims", std::make_unique<Entity::EntityAnimationSetLoader>());
    assets.loadManifest("manifest.yaml");

    Voxel::WorldResources resources;
    Voxel::BlockModelRegistry models;
    const std::array modelsToLoad = {
        modelDefinition("models/blocks/semantic_column.yaml", modelYaml)};
    const Voxel::BlockLoadReport blockReport =
        Voxel::BlockLoader{}.loadDefinitions(
            "base", modelsToLoad, blockDefinitions,
            models, resources.registry());
    CHECK_EQ(blockReport.modelsLoaded, static_cast<size_t>(1));
    CHECK_EQ(blockReport.loaded, materialIds.size());
    CHECK_EQ(blockReport.failed, static_cast<size_t>(0));
    const Voxel::BlockType& stone = resources.registry().getType(
        *resources.registry().findByIdentifier("base:stone_shale"));
    CHECK_EQ(stone.model->identifier(), std::string("base:semantic_column"));
    CHECK_EQ(
        stone.textures.named().at("surface"),
        std::string("textures/synthetic/semantic.png"));
    resources.registry().freeze();
    CHECK(resources.registry().frozen());

    const auto prepared = Voxel::loadPreparedGeneratorDefinitionSnapshot(
        assets, resources.registry(), "rigel:default");
    auto generator = std::make_shared<const Voxel::WorldGenerator>(
        resources.registry(), prepared.data, 1337u);
    Voxel::ChunkBuffer generated;
    generator->generate({0, 0, 0}, generated);

    {
        Voxel::World world(resources);
        world.setGenerator(generator);
        Voxel::Chunk& chunk = world.chunkManager().getOrCreateChunk({0, 0, 0});
        chunk.copyFrom(generated.blocks, resources.registry());

        const auto model = assets.get<Entity::EntityModelAsset>(
            "entity_models/demo_cube");
        auto entity = std::make_unique<Entity::Entity>("base:test_entity");
        entity->setModel(model);
        entity->addTag(Entity::EntityTags::NoClip);
        entity->setVelocity({2.0f, 0.0f, 0.0f});
        const Entity::EntityId id = world.entities().spawn(std::move(entity));
        CHECK(!id.isNull());
        world.tickEntities(0.25f);
        CHECK_NEAR(world.entities().get(id)->position().x, 0.5f, 0.00001f);
        CHECK_EQ(world.entities().get(id)->modelIdentifier(), model.id());
    }
}
