#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace Rigel {

enum class WorldMode {
    Normal,
    BlockGallery,
};

struct LaunchOptions {
    WorldMode worldMode = WorldMode::Normal;
};

using EnvironmentLookup =
    std::function<std::optional<std::string>(std::string_view)>;

LaunchOptions decodeLaunchOptions(
    int argc,
    const char* const* argv,
    const EnvironmentLookup& environmentLookup);
LaunchOptions decodeLaunchOptions(int argc, const char* const* argv);

} // namespace Rigel
