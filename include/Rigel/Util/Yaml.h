#pragma once

#include <ryml.hpp>
#include <ryml_std.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace Rigel::Util {

namespace detail {

struct ParsedConfigurationInteger {
    int64_t value = 0;
    std::string declared;
    std::string fullPath;
};

[[noreturn]] inline void throwInvalidConfigurationInteger(
    const ParsedConfigurationInteger& parsed,
    const char* sourceName,
    const std::string& expectation) {
    throw std::invalid_argument(
        "Invalid configuration value '" + parsed.fullPath + "' in '" +
        sourceName + "': expected " + expectation + ", got '" +
        parsed.declared + "'"
    );
}

inline std::optional<ParsedConfigurationInteger> parseConfigurationInteger(
    ryml::ConstNodeRef node,
    const char* key,
    const char* sourceName,
    std::string_view path,
    const std::string& expectation) {
    if (!node.readable() || !node.has_child(ryml::to_csubstr(key))) {
        return std::nullopt;
    }

    const ryml::csubstr scalar = node[ryml::to_csubstr(key)].val();
    ParsedConfigurationInteger parsed;
    parsed.declared.assign(scalar.data(), scalar.size());
    parsed.fullPath = path.empty()
        ? std::string(key)
        : std::string(path) + "." + key;
    const char* begin = parsed.declared.data();
    const char* end = begin + parsed.declared.size();
    const auto result = std::from_chars(begin, end, parsed.value);
    if (result.ec != std::errc{} || result.ptr != end) {
        throwInvalidConfigurationInteger(parsed, sourceName, expectation);
    }
    return parsed;
}

} // namespace detail

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
    const std::string expectation =
        "integer in [" + std::to_string(minimum) + ", " +
        std::to_string(maximum) + "]";
    const auto parsed = detail::parseConfigurationInteger(
        node, key, sourceName, path, expectation);
    if (!parsed) {
        return fallback;
    }
    if (parsed->value < minimum || parsed->value > maximum) {
        detail::throwInvalidConfigurationInteger(
            *parsed, sourceName, expectation);
    }
    return static_cast<int>(parsed->value);
}

inline int readIntWithMaximum(ryml::ConstNodeRef node,
                              const char* key,
                              int fallback,
                              int minimum,
                              int maximum,
                              const char* sourceName,
                              std::string_view path) {
    const std::string expectation =
        "integer no greater than " + std::to_string(maximum);
    const auto parsed = detail::parseConfigurationInteger(
        node, key, sourceName, path, expectation);
    if (!parsed) {
        return fallback;
    }
    if (parsed->value > maximum) {
        detail::throwInvalidConfigurationInteger(
            *parsed, sourceName, expectation);
    }
    return static_cast<int>(std::max<int64_t>(minimum, parsed->value));
}

[[noreturn]] inline void throwConfigurationConstraint(
    const char* sourceName,
    std::string_view key,
    const std::string& requirement) {
    throw std::invalid_argument(
        "Invalid configuration value '" + std::string(key) + "' in '" +
        sourceName + "': " + requirement
    );
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
