#pragma once

#include "AssetLoader.h"

#include <string_view>
#include <vector>

namespace Rigel::Asset {

/** CPU-owned byte resource used by semantic asset loaders. */
struct RawAsset : AssetBase {
    std::vector<char> data;

    std::string_view str() const {
        return std::string_view(data.data(), data.size());
    }
};

} // namespace Rigel::Asset
