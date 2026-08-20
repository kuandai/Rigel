#pragma once

#include <ryml.hpp>
#include <ryml_std.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <initializer_list>
#include <string>
#include <string_view>

namespace Rigel::Util {

inline void warnUnknownKeys(ryml::ConstNodeRef node,
                            const char* sourceName,
                            std::string_view path,
                            std::initializer_list<std::string_view> knownKeys) {
    if (!node.readable() || !node.is_map()) {
        return;
    }

    for (ryml::ConstNodeRef child : node.children()) {
        const std::string key(child.key().data(), child.key().size());
        if (std::find(knownKeys.begin(), knownKeys.end(), key) != knownKeys.end()) {
            continue;
        }

        const std::string fullPath = path.empty()
            ? key
            : std::string(path) + "." + key;
        spdlog::warn(
            "Unknown configuration key '{}' in '{}'",
            fullPath,
            sourceName
        );
    }
}

inline bool readBool(ryml::ConstNodeRef node, const char* key, bool fallback) {
    if (!node.readable() || !node.has_child(ryml::to_csubstr(key))) {
        return fallback;
    }
    std::string value;
    node[ryml::to_csubstr(key)] >> value;
    return value == "true" || value == "yes" || value == "1";
}

inline int readInt(ryml::ConstNodeRef node, const char* key, int fallback) {
    if (!node.readable() || !node.has_child(ryml::to_csubstr(key))) {
        return fallback;
    }
    int value = fallback;
    node[ryml::to_csubstr(key)] >> value;
    return value;
}

inline float readFloat(ryml::ConstNodeRef node, const char* key, float fallback) {
    if (!node.readable() || !node.has_child(ryml::to_csubstr(key))) {
        return fallback;
    }
    float value = fallback;
    node[ryml::to_csubstr(key)] >> value;
    return value;
}

inline std::string readString(ryml::ConstNodeRef node,
                              const char* key,
                              const std::string& fallback) {
    if (!node.readable() || !node.has_child(ryml::to_csubstr(key))) {
        return fallback;
    }
    std::string value;
    node[ryml::to_csubstr(key)] >> value;
    return value;
}

} // namespace Rigel::Util
