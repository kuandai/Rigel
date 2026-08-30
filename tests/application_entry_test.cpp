#include "ApplicationEntry.h"
#include "LogCapture.h"
#include "TestFramework.h"

#include <cstdlib>
#include <optional>
#include <string>

namespace {

std::optional<Rigel::LaunchOptions> g_observedLaunchOptions;

void observeLaunchOptions(const Rigel::LaunchOptions& options) {
    g_observedLaunchOptions = options;
}

} // namespace

TEST_CASE(ApplicationEntry_ForwardsDecodedLaunchOptions) {
    g_observedLaunchOptions.reset();
    const char* arguments[] = {
        "Rigel", "--world-mode", "block-gallery"};

    const int result = Rigel::runApplication(
        3, arguments, &observeLaunchOptions);

    CHECK_EQ(result, EXIT_SUCCESS);
    CHECK(g_observedLaunchOptions.has_value());
    CHECK_EQ(
        g_observedLaunchOptions->worldMode,
        Rigel::WorldMode::BlockGallery);
}

TEST_CASE(ApplicationEntry_RejectsOptionsBeforeLaunching) {
    g_observedLaunchOptions.reset();
    const char* arguments[] = {"Rigel", "--world-mode=gallery"};
    Rigel::Test::LogCapture logs("application-entry-options");

    const int result = Rigel::runApplication(
        2, arguments, &observeLaunchOptions);

    CHECK_EQ(result, EXIT_FAILURE);
    CHECK(!g_observedLaunchOptions.has_value());
    CHECK(logs.output().find("Invalid value 'gallery'") != std::string::npos);
    CHECK(logs.output().find("normal") != std::string::npos);
    CHECK(logs.output().find("block-gallery") != std::string::npos);
}
