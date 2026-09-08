#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_set>

namespace Rigel::Entity {

class EntityTagList {
public:
    bool has(std::string_view tag) const {
        return m_tags.find(std::string(tag)) != m_tags.end();
    }

    void add(std::string_view tag) {
        m_tags.insert(std::string(tag));
    }

    void remove(std::string_view tag) {
        m_tags.erase(std::string(tag));
    }

    void clear() { m_tags.clear(); }
    size_t size() const { return m_tags.size(); }
    bool retainedStringsFit(size_t byteLimit) const {
        size_t retained = 0;
        for (const std::string& tag : m_tags) {
            if (tag.capacity() > byteLimit - retained) {
                return false;
            }
            retained += tag.capacity();
        }
        return true;
    }

private:
    std::unordered_set<std::string> m_tags;
};

namespace EntityTags {
inline constexpr std::string_view Mob = "mob";
inline constexpr std::string_view Ally = "ally";
inline constexpr std::string_view Passive = "passive";
inline constexpr std::string_view ProjectileImmune = "projectile_immune";
inline constexpr std::string_view FireImmune = "fire_immune";
inline constexpr std::string_view NoDespawn = "no_despawn";
inline constexpr std::string_view NoEntityPush = "no_entity_push";
inline constexpr std::string_view NoBuoyancy = "no_buoyancy";
inline constexpr std::string_view NoSaveInChunks = "no_save_in_chunks";
inline constexpr std::string_view NoClip = "noclip";
inline constexpr std::string_view Sneaking = "sneaking";
inline constexpr std::string_view UsingJetpack = "using_jetpack";
} // namespace EntityTags

} // namespace Rigel::Entity
