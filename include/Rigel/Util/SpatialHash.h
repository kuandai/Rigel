#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace Rigel::Util {

inline std::size_t spatialHash3D(int32_t x, int32_t y, int32_t z) {
    return std::hash<int64_t>{}(
        (static_cast<int64_t>(x) * 73856093) ^
        (static_cast<int64_t>(y) * 19349663) ^
        (static_cast<int64_t>(z) * 83492791)
    );
}

} // namespace Rigel::Util
