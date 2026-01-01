#include "TestFramework.h"

#include "Rigel/Asset/AssetManager.h"
#include "Rigel/Entity/EntityModel.h"
#include "Rigel/Entity/EntityModelLoader.h"
#include "Rigel/Entity/EntityAnimation.h"

using namespace Rigel::Asset;
using namespace Rigel::Entity;

TEST_CASE(EntityAnimation_Advances) {
    AssetManager assets;
    assets.loadManifest("manifest.yaml");
    assets.registerLoader("entity_anims", std::make_unique<EntityAnimationSetLoader>());

    auto animSet = assets.get<EntityAnimationSetAsset>("entity_anims/demo_spin");
    CHECK(animSet);

    const EntityAnimation* anim = animSet->set.find("spin");
    CHECK(anim != nullptr);
    CHECK(anim->duration > 0.0f);

    const EntityBoneAnimation* boneAnim = anim->findBone("root");
    CHECK(boneAnim != nullptr);

    glm::vec3 rot = boneAnim->rotation.sample(0.5f, anim->loop, anim->duration, glm::vec3(0.0f));
    CHECK(rot.y > 100.0f);
    CHECK(rot.y < 260.0f);
}
