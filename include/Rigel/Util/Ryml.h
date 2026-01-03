#pragma once

#include <ryml.hpp>
#include <ryml_std.hpp>

#include <string>

namespace Rigel::Util {

inline std::string toStdString(ryml::csubstr value) {
    return std::string(value.data(), value.size());
}

} // namespace Rigel::Util
