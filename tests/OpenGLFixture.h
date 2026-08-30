#pragma once

#include "TestFramework.h"
#include "Rigel/Render/OpenGLRuntime.h"

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#ifdef RIGEL_TEST_HAS_EGL
#include <EGL/egl.h>
#include <EGL/eglext.h>
#endif

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
    EglInitialization,
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
    explicit HiddenOpenGLContext(int width = 64, int height = 64) {
        if (width <= 0 || height <= 0) {
            m_failure = detail::OpenGLContextFailure::ContextCreation;
            m_error = "OpenGL test surface dimensions must be positive";
            return;
        }
#if (defined(__unix__) || defined(__linux__)) && !defined(__APPLE__)
        if (!detail::hasDisplayEnvironment(
                std::getenv("DISPLAY"), std::getenv("WAYLAND_DISPLAY"))) {
#ifdef RIGEL_TEST_HAS_EGL
            initializeEgl(width, height);
            return;
#else
            m_failure = detail::OpenGLContextFailure::MissingDisplay;
            m_error = "No X11 or Wayland display is available";
            return;
#endif
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
#ifdef RIGEL_TEST_HAS_EGL
            if (m_failure == detail::OpenGLContextFailure::MissingDisplay) {
                initializeEgl(width, height);
            }
#endif
            return;
        }
        m_glfwInitialized = true;

        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, Render::kOpenGLContextMajorVersion);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, Render::kOpenGLContextMinorVersion);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

        m_window = glfwCreateWindow(
            width, height, "Rigel test", nullptr, nullptr);
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
#ifdef RIGEL_TEST_HAS_EGL
        if (m_eglDisplay != EGL_NO_DISPLAY) {
            eglMakeCurrent(
                m_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (m_eglContext != EGL_NO_CONTEXT) {
                eglDestroyContext(m_eglDisplay, m_eglContext);
            }
            if (m_eglSurface != EGL_NO_SURFACE) {
                eglDestroySurface(m_eglDisplay, m_eglSurface);
            }
            eglTerminate(m_eglDisplay);
        }
#endif
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
#ifdef RIGEL_TEST_HAS_EGL
    void setEglError(std::string prefix) {
        m_error = std::move(prefix) + " (EGL error " +
            std::to_string(eglGetError()) + ")";
        m_failure = detail::OpenGLContextFailure::EglInitialization;
    }

    void initializeEgl(int width, int height) {
        const auto getPlatformDisplay =
            reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
                eglGetProcAddress("eglGetPlatformDisplayEXT"));
        if (!getPlatformDisplay) {
            setEglError("EGL surfaceless display entry point is unavailable");
            return;
        }
        m_eglDisplay = getPlatformDisplay(
            EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
        EGLint major = 0;
        EGLint minor = 0;
        if (m_eglDisplay == EGL_NO_DISPLAY ||
            !eglInitialize(m_eglDisplay, &major, &minor)) {
            setEglError("EGL surfaceless display initialization failed");
            return;
        }
        if (!eglBindAPI(EGL_OPENGL_API)) {
            setEglError("EGL could not bind the OpenGL API");
            return;
        }

        constexpr EGLint configAttributes[] = {
            EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
            EGL_RED_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE, 8,
            EGL_ALPHA_SIZE, 8,
            EGL_DEPTH_SIZE, 24,
            EGL_NONE,
        };
        EGLConfig config = nullptr;
        EGLint configCount = 0;
        if (!eglChooseConfig(
                m_eglDisplay, configAttributes, &config, 1, &configCount) ||
            configCount != 1) {
            setEglError("EGL could not select an OpenGL pbuffer configuration");
            return;
        }

        const EGLint surfaceAttributes[] = {
            EGL_WIDTH, width,
            EGL_HEIGHT, height,
            EGL_NONE,
        };
        m_eglSurface = eglCreatePbufferSurface(
            m_eglDisplay, config, surfaceAttributes);
        if (m_eglSurface == EGL_NO_SURFACE) {
            setEglError("EGL pbuffer creation failed");
            return;
        }

        constexpr EGLint contextAttributes[] = {
            EGL_CONTEXT_MAJOR_VERSION_KHR,
            Render::kOpenGLContextMajorVersion,
            EGL_CONTEXT_MINOR_VERSION_KHR,
            Render::kOpenGLContextMinorVersion,
            EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR,
            EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR,
            EGL_NONE,
        };
        m_eglContext = eglCreateContext(
            m_eglDisplay, config, EGL_NO_CONTEXT, contextAttributes);
        if (m_eglContext == EGL_NO_CONTEXT ||
            !eglMakeCurrent(
                m_eglDisplay, m_eglSurface, m_eglSurface, m_eglContext)) {
            setEglError("EGL OpenGL context creation failed");
            return;
        }

        glewExperimental = GL_TRUE;
        const GLenum glewStatus = glewInit();
        if (glewStatus != GLEW_OK &&
            glewStatus != GLEW_ERROR_NO_GLX_DISPLAY) {
            m_failure = detail::OpenGLContextFailure::GlewInitialization;
            m_error = "GLEW initialization failed: ";
            m_error += reinterpret_cast<const char*>(
                glewGetErrorString(glewStatus));
            return;
        }
        glGetError();
        m_failure = detail::OpenGLContextFailure::None;
    }
#endif

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
#ifdef RIGEL_TEST_HAS_EGL
    EGLDisplay m_eglDisplay = EGL_NO_DISPLAY;
    EGLContext m_eglContext = EGL_NO_CONTEXT;
    EGLSurface m_eglSurface = EGL_NO_SURFACE;
#endif
};

} // namespace Rigel::Test
