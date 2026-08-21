#pragma once

#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>

namespace Rigel::Persistence::detail {

inline bool parseCanonicalCoordinate(std::string_view text, int32_t& value) {
    if (text.empty()) {
        return false;
    }

    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size() &&
        text == std::to_string(value);
}

inline bool parseCanonicalRegionFilename(std::string_view name,
                                         std::string_view prefix,
                                         std::string_view suffix,
                                         int32_t& x,
                                         int32_t& y,
                                         int32_t& z) {
    if (!name.starts_with(prefix) || !name.ends_with(suffix) ||
        name.size() < prefix.size() + suffix.size()) {
        return false;
    }

    const auto coordinates = name.substr(
        prefix.size(), name.size() - prefix.size() - suffix.size());
    const auto firstSeparator = coordinates.find('_');
    if (firstSeparator == std::string_view::npos) {
        return false;
    }
    const auto secondSeparator = coordinates.find('_', firstSeparator + 1);
    if (secondSeparator == std::string_view::npos) {
        return false;
    }

    return parseCanonicalCoordinate(coordinates.substr(0, firstSeparator), x) &&
        parseCanonicalCoordinate(
            coordinates.substr(
                firstSeparator + 1, secondSeparator - firstSeparator - 1),
            y) &&
        parseCanonicalCoordinate(coordinates.substr(secondSeparator + 1), z);
}

} // namespace Rigel::Persistence::detail
