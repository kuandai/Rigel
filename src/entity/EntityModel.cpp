#include "Rigel/Entity/EntityModel.h"
#include "Rigel/Entity/EntityModelInstance.h"

#include "Rigel/Asset/AssetManager.h"
#include "Rigel/Asset/Types.h"

#include <spdlog/spdlog.h>

namespace Rigel::Entity {

const EntityBone* EntityModelAsset::findBone(std::string_view name) const {
    auto it = boneLookup.find(std::string(name));
    if (it == boneLookup.end()) {
        return nullptr;
    }
    return &bones[it->second];
}

std::unique_ptr<IEntityModelInstance> EntityModelAsset::createInstance(
    Asset::AssetManager& assets,
    const Asset::Handle<Asset::ShaderAsset>& shader) const {
    if (!shader) {
        spdlog::warn("EntityModelAsset: shader handle missing when creating instance");
        return nullptr;
    }

    std::unordered_map<std::string, Asset::Handle<Asset::TextureAsset>> resolved;
    resolved.reserve(textures.size());

    for (const auto& entry : textures) {
        if (!assets.exists(entry.second)) {
            spdlog::warn("EntityModelAsset: texture '{}' not found in manifest", entry.second);
            continue;
        }
        resolved.emplace(entry.first, assets.get<Asset::TextureAsset>(entry.second));
    }

    std::shared_ptr<const EntityModelAsset> constShared = shared_from_this();
    return std::make_unique<EntityModelInstance>(
        std::move(constShared),
        shader,
        std::move(resolved)
    );
}

} // namespace Rigel::Entity
