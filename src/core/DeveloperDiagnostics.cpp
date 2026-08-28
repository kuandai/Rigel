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
                           std::string_view replacement) {
    if (!pathEntryExists(launchDirectory / relativePath)) {
        return;
    }

    spdlog::warn(
        "Obsolete configuration '{}' is ignored; {}",
        relativePath.generic_string(),
        replacement);
}

void warnAboutObsoleteDocument(
    const std::filesystem::path& launchDirectory,
    const std::filesystem::path& worldDirectory,
    const std::filesystem::path& filename,
    std::string_view replacement) {
    const std::array<std::filesystem::path, 4> paths = {
        std::filesystem::path("assets/config") / filename,
        std::filesystem::path("config") / filename,
        filename,
        worldDirectory / filename,
    };
    for (const auto& path : paths) {
        warnAboutObsoletePath(launchDirectory, path, replacement);
    }
}

} // namespace

bool profilerEnabledFromEnvironmentValue(const char* value) noexcept {
    return value != nullptr && std::string_view(value) == "1";
}

bool profilerEnabledFromEnvironment() noexcept {
    return profilerEnabledFromEnvironmentValue(std::getenv("RIGEL_PROFILE"));
}

void warnAboutObsoleteConfiguration(
    const std::filesystem::path& launchDirectory,
    std::uint32_t activeWorldId) {
    const std::filesystem::path worldDirectory =
        std::filesystem::path("config/worlds") /
        std::to_string(activeWorldId);
    warnAboutObsoleteDocument(
        launchDirectory,
        worldDirectory,
        "world_generation.yaml",
        "declare a complete generator definition in assets/manifest.yaml "
        "and remove legacy flags and overlays");
    warnAboutObsoleteDocument(
        launchDirectory,
        worldDirectory,
        "streaming.yaml",
        "put View Distance in user-preferences.yaml; streaming scheduler "
        "policy is internal");
    warnAboutObsoleteDocument(
        launchDirectory,
        worldDirectory,
        "render.yaml",
        "put View Distance and Shadows in user-preferences.yaml; renderer "
        "tuning is internal");
    warnAboutObsoleteDocument(
        launchDirectory,
        worldDirectory,
        "persistence.yaml",
        "new saves use the installed CR policy and existing saves retain "
        "their authoritative saved format");

    const std::array<std::filesystem::path, 6> legacyOverlays = {
        "assets/config/worldgen_overlays/no_carvers.yaml",
        "config/assets/config/worldgen_overlays/no_carvers.yaml",
        "config/worldgen_overlays/no_carvers.yaml",
        "worldgen_overlays/no_carvers.yaml",
        worldDirectory / "assets/config/worldgen_overlays/no_carvers.yaml",
        worldDirectory / "worldgen_overlays/no_carvers.yaml",
    };
    for (const auto& path : legacyOverlays) {
        warnAboutObsoletePath(
            launchDirectory,
            path,
            "declare a complete generator definition variant in "
            "assets/manifest.yaml and remove this conditional overlay");
    }
}

} // namespace Rigel::detail
