#pragma once

#include <ryml.hpp>
#include <ryml_std.hpp>

#include <string>

namespace Rigel::Util {

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
