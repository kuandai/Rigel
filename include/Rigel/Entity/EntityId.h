#pragma once

#include <cstdint>
#include <functional>

namespace Rigel::Entity {

struct EntityId {
    uint64_t time = 0;
    uint32_t random = 0;
    uint32_t counter = 0;

    bool operator==(const EntityId&) const = default;

    bool isNull() const { return time == 0 && random == 0 && counter == 0; }

    static EntityId Null() { return {}; }
    static EntityId New();
};

struct EntityIdHash {
    size_t operator()(const EntityId& id) const noexcept {
        size_t h1 = std::hash<uint64_t>{}(id.time);
        size_t h2 = std::hash<uint32_t>{}(id.random);
        size_t h3 = std::hash<uint32_t>{}(id.counter);
        return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2))
                 ^ (h3 + 0x9e3779b97f4a7c15ULL + (h2 << 6) + (h2 >> 2));
    }
};

} // namespace Rigel::Entity
