#pragma once

#include <algorithm>
#include <algorithm>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

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
    std::vector<std::string> sorted() const {
        std::vector<std::string> result(m_tags.begin(), m_tags.end());
        std::sort(result.begin(), result.end());
        return result;
    }
    std::optional<size_t> retainedStorageBytes() const {
        if (m_tags.bucket_count() >
            std::numeric_limits<size_t>::max() / sizeof(void*)) {
            return std::nullopt;
        }
        size_t retained = m_tags.bucket_count() * sizeof(void*);
        if (m_tags.size() >
            (std::numeric_limits<size_t>::max() - retained) /
                sizeof(std::string)) {
            return std::nullopt;
        }
        retained += m_tags.size() * sizeof(std::string);
        for (const std::string& tag : m_tags) {
            if (tag.capacity() >
                std::numeric_limits<size_t>::max() - retained) {
                return std::nullopt;
            }
            retained += tag.capacity();
        }
        return retained;
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
