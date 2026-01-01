#pragma once

#include <Rigel/Asset/AssetLoader.h>

namespace Rigel::Entity {

class EntityModelLoader : public Asset::IAssetLoader {
public:
    std::string_view category() const override { return "entity_models"; }
    std::shared_ptr<Asset::AssetBase> load(const Asset::LoadContext& ctx) override;
};

class EntityAnimationSetLoader : public Asset::IAssetLoader {
public:
    std::string_view category() const override { return "entity_anims"; }
    std::shared_ptr<Asset::AssetBase> load(const Asset::LoadContext& ctx) override;
};

} // namespace Rigel::Entity
