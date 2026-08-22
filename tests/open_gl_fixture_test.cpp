#include "TestFramework.h"

#include "OpenGLFixture.h"

using Rigel::Test::detail::OpenGLContextFailure;

TEST_CASE(OpenGLFixture_OnlyMissingDisplayCanSkip) {
    CHECK(Rigel::Test::detail::canSkipOpenGLContextFailure(
        OpenGLContextFailure::MissingDisplay));
    CHECK(!Rigel::Test::detail::canSkipOpenGLContextFailure(
        OpenGLContextFailure::None));
    CHECK(!Rigel::Test::detail::canSkipOpenGLContextFailure(
        OpenGLContextFailure::GlfwInitialization));
    CHECK(!Rigel::Test::detail::canSkipOpenGLContextFailure(
        OpenGLContextFailure::ContextCreation));
    CHECK(!Rigel::Test::detail::canSkipOpenGLContextFailure(
        OpenGLContextFailure::GlewInitialization));
}

TEST_CASE(OpenGLFixture_RecognizesOnlyExplicitMissingDisplayErrors) {
    CHECK(Rigel::Test::detail::isMissingDisplayError(
        GLFW_PLATFORM_ERROR, "X11: Failed to open display localhost:10.0"));
    CHECK(Rigel::Test::detail::isMissingDisplayError(
        GLFW_PLATFORM_ERROR,
        "X11: The DISPLAY environment variable is missing"));
    CHECK(Rigel::Test::detail::isMissingDisplayError(
        GLFW_PLATFORM_ERROR, "Wayland: Failed to connect to display"));
    CHECK(!Rigel::Test::detail::isMissingDisplayError(
        GLFW_PLATFORM_ERROR, "X11: Failed to load Xlib"));
    CHECK(!Rigel::Test::detail::isMissingDisplayError(
        GLFW_VERSION_UNAVAILABLE, "X11: Failed to open display :0"));
}

TEST_CASE(OpenGLFixture_MissingDisplayRequiresBothVariablesAbsent) {
    CHECK(!Rigel::Test::detail::hasDisplayEnvironment(nullptr, nullptr));
    CHECK(!Rigel::Test::detail::hasDisplayEnvironment("", ""));
    CHECK(Rigel::Test::detail::hasDisplayEnvironment(":0", nullptr));
    CHECK(Rigel::Test::detail::hasDisplayEnvironment(nullptr, "wayland-0"));
}
