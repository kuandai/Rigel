#include "Rigel/Entity/EntityFactory.h"

namespace Rigel::Entity {

EntityFactory& EntityFactory::instance() {
    static EntityFactory factory;
    return factory;
}

void EntityFactory::registerType(const std::string& typeId, Creator creator) {
    m_creators[typeId] = std::move(creator);
}

std::unique_ptr<Entity> EntityFactory::create(const std::string& typeId) const {
    auto it = m_creators.find(typeId);
    if (it == m_creators.end()) {
        return nullptr;
    }
    return it->second();
}

bool EntityFactory::hasType(const std::string& typeId) const {
    return m_creators.find(typeId) != m_creators.end();
}

} // namespace Rigel::Entity
