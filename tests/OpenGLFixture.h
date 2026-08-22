#pragma once

#include "TestFramework.h"
#include "Rigel/Render/OpenGLRuntime.h"

#include <GL/glew.h>
#include <GLFW/glfw3.h>

#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>

namespace Rigel::Test {

namespace detail {

enum class OpenGLContextFailure {
    None,
    MissingDisplay,
    GlfwInitialization,
    ContextCreation,
    GlewInitialization,
};

constexpr bool canSkipOpenGLContextFailure(OpenGLContextFailure failure) {
    return failure == OpenGLContextFailure::MissingDisplay;
}

constexpr bool isMissingDisplayError(
    int errorCode,
    std::string_view description
) {
    return errorCode == GLFW_PLATFORM_ERROR &&
           (description.starts_with("X11: Failed to open display") ||
            description == "X11: The DISPLAY environment variable is missing" ||
            description == "Wayland: Failed to connect to display");
}

constexpr bool hasDisplayEnvironment(
    const char* x11Display,
    const char* waylandDisplay
) {
    return (x11Display && x11Display[0] != '\0') ||
           (waylandDisplay && waylandDisplay[0] != '\0');
}

} // namespace detail

class HiddenOpenGLContext {
public:
    HiddenOpenGLContext() {
#if (defined(__unix__) || defined(__linux__)) && !defined(__APPLE__)
        if (!detail::hasDisplayEnvironment(
                std::getenv("DISPLAY"), std::getenv("WAYLAND_DISPLAY"))) {
            m_failure = detail::OpenGLContextFailure::MissingDisplay;
            m_error = "No X11 or Wayland display is available";
            return;
        }
#endif

        if (!glfwInit()) {
            const char* description = nullptr;
            const int code = glfwGetError(&description);
            m_failure = detail::isMissingDisplayError(
                            code, description ? description : "")
                ? detail::OpenGLContextFailure::MissingDisplay
                : detail::OpenGLContextFailure::GlfwInitialization;
            setGlfwError("GLFW initialization failed", code, description);
            return;
        }
        m_glfwInitialized = true;

        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, Render::kOpenGLContextMajorVersion);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, Render::kOpenGLContextMinorVersion);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

        m_window = glfwCreateWindow(64, 64, "Rigel test", nullptr, nullptr);
        if (!m_window) {
            m_failure = detail::OpenGLContextFailure::ContextCreation;
            const char* description = nullptr;
            const int code = glfwGetError(&description);
            setGlfwError("OpenGL context creation failed", code, description);
            return;
        }

        glfwMakeContextCurrent(m_window);
        glewExperimental = GL_TRUE;
        const GLenum glewStatus = glewInit();
        if (glewStatus != GLEW_OK) {
            m_failure = detail::OpenGLContextFailure::GlewInitialization;
            m_error = "GLEW initialization failed: ";
            m_error += reinterpret_cast<const char*>(glewGetErrorString(glewStatus));
            return;
        }

        // GLEW can leave GL_INVALID_ENUM set after probing a core context.
        glGetError();
        m_failure = detail::OpenGLContextFailure::None;
    }

    ~HiddenOpenGLContext() {
        if (m_window) {
            glfwMakeContextCurrent(nullptr);
            glfwDestroyWindow(m_window);
        }
        if (m_glfwInitialized) {
            glfwTerminate();
        }
    }

    void require() const {
        if (m_failure == detail::OpenGLContextFailure::None) {
            return;
        }
        if (detail::canSkipOpenGLContextFailure(m_failure)) {
            throw TestSkip(m_error);
        }
        throw TestFailure(m_error);
    }

private:
    void setGlfwError(
        std::string prefix,
        int code,
        const char* description
    ) {
        m_error = std::move(prefix) + " (GLFW error " + std::to_string(code);
        if (description) {
            m_error += ": ";
            m_error += description;
        }
        m_error += ")";
    }

    GLFWwindow* m_window = nullptr;
    bool m_glfwInitialized = false;
    detail::OpenGLContextFailure m_failure =
        detail::OpenGLContextFailure::GlfwInitialization;
    std::string m_error;
};

} // namespace Rigel::Test
