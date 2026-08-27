#include "DeveloperDiagnostics.h"

#include <array>
#include <cstdlib>
#include <string>
#include <system_error>
#include <string_view>

#include <spdlog/spdlog.h>

namespace Rigel::detail {
namespace {

bool pathEntryExists(const std::filesystem::path& path) {
    std::error_code error;
    const std::filesystem::file_status status =
        std::filesystem::symlink_status(path, error);
    return !error && status.type() != std::filesystem::file_type::not_found;
}

void warnAboutObsoletePath(const std::filesystem::path& launchDirectory,
                           const std::filesystem::path& relativePath,
                           bool overlay) {
    if (!pathEntryExists(launchDirectory / relativePath)) {
        return;
    }

    if (overlay) {
        spdlog::warn(
            "Obsolete conditional generation overlay '{}' is ignored; "
            "declare a complete generator definition variant in "
            "assets/manifest.yaml and remove this file",
            relativePath.generic_string());
        return;
    }

    spdlog::warn(
        "Obsolete generation configuration '{}' is ignored; move generation "
        "content to a complete definition declared in assets/manifest.yaml "
        "and remove legacy flags and overlay declarations",
        relativePath.generic_string());
}

} // namespace

bool profilerEnabledFromEnvironmentValue(const char* value) noexcept {
    return value != nullptr && std::string_view(value) == "1";
}

bool profilerEnabledFromEnvironment() noexcept {
    return profilerEnabledFromEnvironmentValue(std::getenv("RIGEL_PROFILE"));
}

void warnAboutObsoleteGenerationConfiguration(
    const std::filesystem::path& launchDirectory,
    std::uint32_t activeWorldId) {
    const std::filesystem::path worldDirectory =
        std::filesystem::path("config/worlds") /
        std::to_string(activeWorldId);
    const std::array<std::filesystem::path, 4> legacyDocuments = {
        "assets/config/world_generation.yaml",
        "config/world_generation.yaml",
        "world_generation.yaml",
        worldDirectory / "world_generation.yaml",
    };
    for (const auto& path : legacyDocuments) {
        warnAboutObsoletePath(launchDirectory, path, false);
    }

    const std::array<std::filesystem::path, 6> legacyOverlays = {
        "assets/config/worldgen_overlays/no_carvers.yaml",
        "config/assets/config/worldgen_overlays/no_carvers.yaml",
        "config/worldgen_overlays/no_carvers.yaml",
        "worldgen_overlays/no_carvers.yaml",
        worldDirectory / "assets/config/worldgen_overlays/no_carvers.yaml",
        worldDirectory / "worldgen_overlays/no_carvers.yaml",
    };
    for (const auto& path : legacyOverlays) {
        warnAboutObsoletePath(launchDirectory, path, true);
    }
}

} // namespace Rigel::detail
