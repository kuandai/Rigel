#include "Rigel/LaunchOptions.h"

#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>

namespace Rigel {

namespace {

constexpr std::string_view kWorldModeOption = "--world-mode";
constexpr std::string_view kWorldModeEnvironment = "RIGEL_WORLD_MODE";

WorldMode parseWorldMode(
    std::string_view value,
    std::string_view source) {
    if (value == "normal") {
        return WorldMode::Normal;
    }
    if (value == "block-gallery") {
        return WorldMode::BlockGallery;
    }

    throw std::invalid_argument(
        "Invalid value '" + std::string(value) + "' for " +
        std::string(source) +
        "; expected 'normal' or 'block-gallery'.");
}

std::optional<std::string> processEnvironment(std::string_view name) {
    const std::string ownedName(name);
    const char* value = std::getenv(ownedName.c_str());
    if (!value) {
        return std::nullopt;
    }
    return std::string(value);
}

} // namespace

LaunchOptions decodeLaunchOptions(
    int argc,
    const char* const* argv,
    const EnvironmentLookup& environmentLookup
) {
    if (argc < 0) {
        throw std::invalid_argument("Launch argument count cannot be negative.");
    }
    if (argc > 0 && !argv) {
        throw std::invalid_argument("Launch arguments are unavailable.");
    }
    if (!environmentLookup) {
        throw std::invalid_argument("Launch environment lookup is unavailable.");
    }

    std::optional<std::string_view> commandLineWorldMode;
    for (int index = 1; index < argc; ++index) {
        if (!argv[index]) {
            throw std::invalid_argument(
                "Launch argument " + std::to_string(index) + " is unavailable.");
        }

        const std::string_view argument(argv[index]);
        std::optional<std::string_view> value;
        if (argument == kWorldModeOption) {
            if (index + 1 >= argc || !argv[index + 1] ||
                std::string_view(argv[index + 1]).starts_with("--")) {
                throw std::invalid_argument(
                    "Option '--world-mode' requires a value: "
                    "'normal' or 'block-gallery'.");
            }
            value = argv[++index];
        } else if (argument.starts_with("--world-mode=")) {
            value = argument.substr(kWorldModeOption.size() + 1);
            if (value->empty()) {
                throw std::invalid_argument(
                    "Option '--world-mode' requires a value: "
                    "'normal' or 'block-gallery'.");
            }
        } else {
            throw std::invalid_argument(
                "Unknown launch option '" + std::string(argument) +
                "'. Supported option: --world-mode <normal|block-gallery>.");
        }

        if (commandLineWorldMode) {
            throw std::invalid_argument(
                "Option '--world-mode' was provided more than once.");
        }
        commandLineWorldMode = *value;
    }

    LaunchOptions options;
    if (commandLineWorldMode) {
        options.worldMode = parseWorldMode(
            *commandLineWorldMode,
            "command-line option '--world-mode'");
        return options;
    }

    if (const auto environmentWorldMode =
            environmentLookup(kWorldModeEnvironment)) {
        options.worldMode = parseWorldMode(
            *environmentWorldMode,
            "environment variable RIGEL_WORLD_MODE");
    }
    return options;
}

LaunchOptions decodeLaunchOptions(int argc, const char* const* argv) {
    return decodeLaunchOptions(argc, argv, &processEnvironment);
}

} // namespace Rigel
