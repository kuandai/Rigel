#pragma once

#include "Rigel/Asset/AssetLoader.h"

#include <string_view>

namespace Rigel {

class InputBindingsLoader : public Asset::IAssetLoader {
public:
    std::string_view category() const override { return "input"; }
    std::shared_ptr<Asset::AssetBase> load(const Asset::LoadContext& ctx) override;
};

} // namespace Rigel
