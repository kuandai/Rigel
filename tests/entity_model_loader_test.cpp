#include "TestFramework.h"

#include "Rigel/Asset/AssetManager.h"
#include "Rigel/Entity/EntityModelLoader.h"
#include "Rigel/Entity/EntityModel.h"

using namespace Rigel::Asset;
using namespace Rigel::Entity;

TEST_CASE(EntityModel_LoadsDefinition) {
    AssetManager assets;
    assets.loadManifest("manifest.yaml");
    assets.registerLoader("entity_models", std::make_unique<EntityModelLoader>());
    assets.registerLoader("entity_anims", std::make_unique<EntityAnimationSetLoader>());

    auto model = assets.get<EntityModelAsset>("entity_models/demo_cube");
    CHECK(model);
    CHECK_EQ(model->bones.size(), static_cast<size_t>(1));
    CHECK_EQ(model->textures.count("diffuse"), static_cast<size_t>(1));
    CHECK_EQ(model->defaultAnimation, std::string("spin"));
}
