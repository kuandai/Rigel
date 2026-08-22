#pragma once

#include <ryml.hpp>
#include <ryml_std.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

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

inline int readIntInRange(ryml::ConstNodeRef node,
                          const char* key,
                          int fallback,
                          int minimum,
                          int maximum,
                          const char* sourceName,
                          std::string_view path) {
    if (!node.readable() || !node.has_child(ryml::to_csubstr(key))) {
        return fallback;
    }

    const ryml::csubstr scalar = node[ryml::to_csubstr(key)].val();
    const std::string declared(scalar.data(), scalar.size());
    int64_t value = 0;
    const char* begin = declared.data();
    const char* end = begin + declared.size();
    const auto result = std::from_chars(begin, end, value);
    const std::string fullPath = path.empty()
        ? std::string(key)
        : std::string(path) + "." + key;
    if (result.ec != std::errc{} || result.ptr != end ||
        value < minimum || value > maximum) {
        throw std::invalid_argument(
            "Invalid configuration value '" + fullPath + "' in '" +
            sourceName + "': expected integer in [" +
            std::to_string(minimum) + ", " + std::to_string(maximum) +
            "], got '" + declared + "'"
        );
    }
    return static_cast<int>(value);
}

inline int readIntWithMaximum(ryml::ConstNodeRef node,
                              const char* key,
                              int fallback,
                              int minimum,
                              int maximum,
                              const char* sourceName,
                              std::string_view path) {
    if (!node.readable() || !node.has_child(ryml::to_csubstr(key))) {
        return fallback;
    }

    const ryml::csubstr scalar = node[ryml::to_csubstr(key)].val();
    const std::string declared(scalar.data(), scalar.size());
    int64_t value = 0;
    const char* begin = declared.data();
    const char* end = begin + declared.size();
    const auto result = std::from_chars(begin, end, value);
    const std::string fullPath = path.empty()
        ? std::string(key)
        : std::string(path) + "." + key;
    if (result.ec != std::errc{} || result.ptr != end || value > maximum) {
        throw std::invalid_argument(
            "Invalid configuration value '" + fullPath + "' in '" +
            sourceName + "': expected integer no greater than " +
            std::to_string(maximum) + ", got '" + declared + "'"
        );
    }
    return static_cast<int>(std::max<int64_t>(minimum, value));
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
