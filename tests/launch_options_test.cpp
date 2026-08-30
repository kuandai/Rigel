#include "Rigel/LaunchOptions.h"
#include "TestFramework.h"

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

Rigel::EnvironmentLookup noEnvironment() {
    return [](std::string_view) -> std::optional<std::string> {
        return std::nullopt;
    };
}

std::string decodeFailure(std::vector<const char*> arguments) {
    try {
        (void)Rigel::decodeLaunchOptions(
            static_cast<int>(arguments.size()),
            arguments.data(),
            noEnvironment());
    } catch (const std::invalid_argument& error) {
        return error.what();
    }
    throw Rigel::Test::TestFailure(
        "Expected launch option decoding to fail");
}

} // namespace

TEST_CASE(LaunchOptions_DefaultsToNormal) {
    const char* arguments[] = {"Rigel"};
    int environmentLookups = 0;
    const Rigel::LaunchOptions options = Rigel::decodeLaunchOptions(
        1,
        arguments,
        [&](std::string_view name) -> std::optional<std::string> {
            ++environmentLookups;
            CHECK_EQ(name, std::string_view("RIGEL_WORLD_MODE"));
            return std::nullopt;
        });

    CHECK_EQ(options.worldMode, Rigel::WorldMode::Normal);
    CHECK_EQ(environmentLookups, 1);
}

TEST_CASE(LaunchOptions_AcceptsBothCommandLineForms) {
    int environmentLookups = 0;
    const Rigel::EnvironmentLookup environment =
        [&](std::string_view) -> std::optional<std::string> {
            ++environmentLookups;
            return std::string("block-gallery");
        };

    const char* separatedArguments[] = {
        "Rigel", "--world-mode", "block-gallery"};
    const Rigel::LaunchOptions separated = Rigel::decodeLaunchOptions(
        3, separatedArguments, environment);
    CHECK_EQ(separated.worldMode, Rigel::WorldMode::BlockGallery);

    const char* joinedArguments[] = {"Rigel", "--world-mode=normal"};
    const Rigel::LaunchOptions joined = Rigel::decodeLaunchOptions(
        2, joinedArguments, environment);
    CHECK_EQ(joined.worldMode, Rigel::WorldMode::Normal);

    CHECK_EQ(environmentLookups, 0);
}

TEST_CASE(LaunchOptions_UsesEnvironmentFallback) {
    const char* arguments[] = {"Rigel"};
    const Rigel::LaunchOptions options = Rigel::decodeLaunchOptions(
        1,
        arguments,
        [](std::string_view name) -> std::optional<std::string> {
            CHECK_EQ(name, std::string_view("RIGEL_WORLD_MODE"));
            return std::string("block-gallery");
        });

    CHECK_EQ(options.worldMode, Rigel::WorldMode::BlockGallery);
}

TEST_CASE(LaunchOptions_RejectsMalformedUnknownAndDuplicateOptions) {
    const std::string missing = decodeFailure({"Rigel", "--world-mode"});
    CHECK(missing.find("requires a value") != std::string::npos);

    const std::string empty = decodeFailure({"Rigel", "--world-mode="});
    CHECK(empty.find("requires a value") != std::string::npos);

    const std::string invalid =
        decodeFailure({"Rigel", "--world-mode", "gallery"});
    CHECK(invalid.find("Invalid value 'gallery'") != std::string::npos);
    CHECK(invalid.find("normal") != std::string::npos);
    CHECK(invalid.find("block-gallery") != std::string::npos);

    const std::string unknown = decodeFailure({"Rigel", "--fullscreen"});
    CHECK(unknown.find("Unknown launch option '--fullscreen'") !=
          std::string::npos);
    CHECK(unknown.find("--world-mode") != std::string::npos);

    const std::string duplicate = decodeFailure({
        "Rigel",
        "--world-mode=normal",
        "--world-mode",
        "block-gallery",
    });
    CHECK(duplicate.find("provided more than once") != std::string::npos);
}

TEST_CASE(LaunchOptions_RejectsInvalidEnvironmentFallback) {
    const char* arguments[] = {"Rigel"};

    try {
        (void)Rigel::decodeLaunchOptions(
            1,
            arguments,
            [](std::string_view) -> std::optional<std::string> {
                return std::string("gallery");
            });
    } catch (const std::invalid_argument& error) {
        const std::string message = error.what();
        CHECK(message.find("RIGEL_WORLD_MODE") != std::string::npos);
        CHECK(message.find("Invalid value 'gallery'") != std::string::npos);
        return;
    }

    throw Rigel::Test::TestFailure(
        "Expected an invalid environment fallback to fail");
}
