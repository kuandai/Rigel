#pragma once

#include "Entity.h"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace Rigel::Entity {

class EntityFactory {
public:
    using Creator = std::function<std::unique_ptr<Entity>()>;

    static EntityFactory& instance();

    void registerType(const std::string& typeId, Creator creator);
    std::unique_ptr<Entity> create(const std::string& typeId) const;
    bool hasType(const std::string& typeId) const;

private:
    std::unordered_map<std::string, Creator> m_creators;
};

} // namespace Rigel::Entity
