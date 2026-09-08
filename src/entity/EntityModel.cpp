#include "Rigel/Entity/EntityModel.h"

namespace Rigel::Entity {

const EntityBone* EntityModelAsset::findBone(std::string_view name) const {
    auto it = boneLookup.find(std::string(name));
    if (it == boneLookup.end()) {
        return nullptr;
    }
    return &bones[it->second];
}

} // namespace Rigel::Entity
