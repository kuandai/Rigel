#pragma once

#include "EntityAnimation.h"

#include <Rigel/Asset/AssetLoader.h>
#include <Rigel/Asset/Handle.h>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Rigel::Asset {
class AssetManager;
struct ShaderAsset;
}

namespace Rigel::Entity {

class IEntityModelInstance;

class IEntityModel {
public:
    virtual ~IEntityModel() = default;

    virtual std::unique_ptr<IEntityModelInstance> createInstance(
        Asset::AssetManager& assets,
        const Asset::Handle<Asset::ShaderAsset>& shader) const = 0;
};

struct EntityModelCube {
    glm::vec3 origin{0.0f};
    glm::vec3 size{1.0f};
    glm::vec2 uv{0.0f};
    glm::vec3 pivot{0.0f};
    glm::vec3 rotation{0.0f};
    float inflate = 0.0f;
    bool mirror = false;
};

struct EntityBone {
    std::string name;
    int parentIndex = -1;
    glm::vec3 pivot{0.0f};
    glm::vec3 rotation{0.0f};
    glm::vec3 scale{1.0f};
    std::vector<EntityModelCube> cubes;
};

struct EntityAnimationSetAsset : public Asset::AssetBase {
    EntityAnimationSet set;
};

struct EntityModelAsset : public Asset::AssetBase,
                          public IEntityModel,
                          public std::enable_shared_from_this<EntityModelAsset> {
    float texWidth = 16.0f;
    float texHeight = 16.0f;
    float modelScale = 1.0f;
    std::unordered_map<std::string, std::string> textures;
    std::vector<EntityBone> bones;
    std::unordered_map<std::string, size_t> boneLookup;
    Asset::Handle<EntityAnimationSetAsset> animationSet;
    std::string defaultAnimation;

    const EntityBone* findBone(std::string_view name) const;

    std::unique_ptr<IEntityModelInstance> createInstance(
        Asset::AssetManager& assets,
        const Asset::Handle<Asset::ShaderAsset>& shader) const override;
};

} // namespace Rigel::Entity
