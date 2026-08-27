#include "GlfwRuntime.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cstdint>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Rigel {
namespace {

bool supportsSwapInterval(int interval) {
#ifdef __APPLE__
    static_cast<void>(interval);
    return true;
#elif defined(_WIN32)
    static_cast<void>(interval);
    if (glfwExtensionSupported("WGL_EXT_swap_control") == GLFW_TRUE) {
        return true;
    }

    // Both supported display modes remain windowed. GLFW implements their
    // interval through DwmFlush when desktop composition is active.
    const HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (!dwm) {
        return false;
    }
    using DwmIsCompositionEnabled = HRESULT(WINAPI*)(BOOL*);
    const auto isCompositionEnabled =
        reinterpret_cast<DwmIsCompositionEnabled>(
            GetProcAddress(dwm, "DwmIsCompositionEnabled"));
    BOOL enabled = FALSE;
    const bool supported =
        isCompositionEnabled &&
        SUCCEEDED(isCompositionEnabled(&enabled)) && enabled;
    FreeLibrary(dwm);
    return supported;
#else
    const bool ext =
        glfwExtensionSupported("GLX_EXT_swap_control") == GLFW_TRUE;
    const bool mesa =
        glfwExtensionSupported("GLX_MESA_swap_control") == GLFW_TRUE;
    if (interval == 0) {
        return ext || mesa;
    }
    return ext || mesa ||
        glfwExtensionSupported("GLX_SGI_swap_control") == GLFW_TRUE;
#endif
}

} // namespace

GlfwRuntime::GlfwRuntime()
    : GlfwRuntime(Api{
          &glfwInit,
          &glfwTerminate,
          &glfwWindowHint,
          &glfwCreateWindow,
          &glfwDestroyWindow,
          &glfwMakeContextCurrent,
          &glfwGetMonitors,
          &glfwGetPrimaryMonitor,
          &glfwGetVideoMode,
          &glfwGetMonitorPos,
          &glfwGetWindowPos,
          &glfwGetWindowSize,
          &glfwGetFramebufferSize,
          &glfwGetWindowAttrib,
          &glfwSetWindowAttrib,
          &glfwSetWindowMonitor,
          &glfwSetWindowPos,
          &supportsSwapInterval,
          &glfwSwapInterval,
          &glfwGetError,
          &glfwSetWindowSizeCallback,
          &glfwSetFramebufferSizeCallback,
          &glfwGetTime,
          &glfwWindowShouldClose,
          &glfwPollEvents,
          &glfwSwapBuffers,
      }) {
}

GlfwRuntime::GlfwRuntime(Api api)
    : m_api(api) {
}

GlfwRuntime::~GlfwRuntime() {
    shutdown();
}

double GlfwRuntime::time() const {
    return m_api.getTime ? m_api.getTime() : 0.0;
}

bool GlfwRuntime::windowShouldClose(GLFWwindow* window) const {
    return !m_api.windowShouldClose || m_api.windowShouldClose(window) != 0;
}

void GlfwRuntime::pollEvents() const {
    if (m_api.pollEvents) {
        m_api.pollEvents();
    }
}

void GlfwRuntime::swapBuffers(GLFWwindow* window) const {
    if (m_api.swapBuffers) {
        m_api.swapBuffers(window);
    }
}

bool GlfwRuntime::initialize() {
    if (m_initialized) {
        return true;
    }
    m_initialized = m_api.initialize() != 0;
    return m_initialized;
}

void GlfwRuntime::windowHint(int hint, int value) const {
    if (m_initialized) {
        m_api.windowHint(hint, value);
    }
}

GLFWwindow* GlfwRuntime::createWindow(int width, int height, const char* title) {
    return createWindow(width, height, title, true, std::nullopt);
}

GLFWwindow* GlfwRuntime::createWindow(
    int width,
    int height,
    const char* title,
    bool decorated,
    std::optional<std::pair<int, int>> position) {
    if (!m_initialized || m_window) {
        return m_window;
    }
    windowHint(GLFW_DECORATED, decorated ? GLFW_TRUE : GLFW_FALSE);
    clearError();
    m_window = m_api.createWindow(width, height, title, nullptr, nullptr);
    if (m_window && position) {
        m_api.setWindowPos(m_window, position->first, position->second);
    }
    if (captureError("window creation")) {
        destroyWindow();
        return nullptr;
    }
    return m_window;
}

bool GlfwRuntime::makeContextCurrent() const {
    if (!m_window) {
        m_lastError = "cannot acquire a context without a window";
        return false;
    }
    clearError();
    m_api.makeContextCurrent(m_window);
    return !captureError("context acquisition");
}

void GlfwRuntime::destroyWindow() noexcept {
    GLFWwindow* window = std::exchange(m_window, nullptr);
    if (!window) {
        return;
    }
    m_api.makeContextCurrent(nullptr);
    m_api.destroyWindow(window);
}

std::optional<GlfwRuntime::Rectangle> GlfwRuntime::currentDesktopBounds() const {
    int monitorCount = 0;
    GLFWmonitor* primaryFallback = nullptr;
    clearError();
    GLFWmonitor** monitors = m_api.getMonitors(&monitorCount);
    if (captureError("monitor enumeration")) {
        return std::nullopt;
    }
    if (!monitors || monitorCount <= 0) {
        clearError();
        primaryFallback = m_api.getPrimaryMonitor();
        if (captureError("primary monitor query")) {
            return std::nullopt;
        }
        if (!primaryFallback) {
            m_lastError = "no current or default monitor is available";
            return std::nullopt;
        }
        monitors = &primaryFallback;
        monitorCount = 1;
    }

    Rectangle window;
    if (m_window) {
        const auto bounds = windowBounds();
        if (!bounds) {
            return std::nullopt;
        }
        window = *bounds;
    }
    clearError();
    GLFWmonitor* selected = m_api.getPrimaryMonitor();
    if (captureError("primary monitor query")) {
        return std::nullopt;
    }
    std::int64_t selectedOverlap = -1;
    if (m_window) {
        for (int index = 0; index < monitorCount; ++index) {
            GLFWmonitor* monitor = monitors[index];
            clearError();
            const GLFWvidmode* mode = m_api.getVideoMode(monitor);
            if (captureError("monitor desktop mode query")) {
                return std::nullopt;
            }
            if (!mode) {
                continue;
            }
            int x = 0;
            int y = 0;
            clearError();
            m_api.getMonitorPos(monitor, &x, &y);
            if (captureError("monitor position query")) {
                return std::nullopt;
            }
            const int overlapWidth = std::max(
                0,
                std::min(window.x + window.width, x + mode->width) -
                    std::max(window.x, x));
            const int overlapHeight = std::max(
                0,
                std::min(window.y + window.height, y + mode->height) -
                    std::max(window.y, y));
            const std::int64_t overlap =
                static_cast<std::int64_t>(overlapWidth) * overlapHeight;
            if (overlap > selectedOverlap) {
                selected = monitor;
                selectedOverlap = overlap;
            }
        }
    }

    if (!selected) {
        selected = monitors[0];
    }
    clearError();
    const GLFWvidmode* mode = m_api.getVideoMode(selected);
    if (captureError("monitor desktop mode query")) {
        return std::nullopt;
    }
    if (!mode || mode->width <= 0 || mode->height <= 0) {
        m_lastError = "the current or default monitor has no desktop mode";
        return std::nullopt;
    }
    Rectangle result;
    clearError();
    m_api.getMonitorPos(selected, &result.x, &result.y);
    if (captureError("monitor position query")) {
        return std::nullopt;
    }
    result.width = mode->width;
    result.height = mode->height;
    return result;
}

std::optional<GlfwRuntime::Rectangle> GlfwRuntime::windowBounds() const {
    Rectangle result;
    if (!m_window) {
        m_lastError = "cannot query bounds without a window";
        return std::nullopt;
    }
    clearError();
    m_api.getWindowPos(m_window, &result.x, &result.y);
    if (captureError("window position query")) {
        return std::nullopt;
    }
    clearError();
    m_api.getWindowSize(m_window, &result.width, &result.height);
    if (captureError("window size query")) {
        return std::nullopt;
    }
    if (result.width <= 0 || result.height <= 0) {
        m_lastError = "window size query returned empty bounds";
        return std::nullopt;
    }
    return result;
}

std::optional<std::pair<int, int>> GlfwRuntime::framebufferSize() const {
    int width = 0;
    int height = 0;
    if (!m_window) {
        m_lastError = "cannot query framebuffer size without a window";
        return std::nullopt;
    }
    clearError();
    m_api.getFramebufferSize(m_window, &width, &height);
    if (captureError("framebuffer size query")) {
        return std::nullopt;
    }
    return std::pair{width, height};
}

std::optional<bool> GlfwRuntime::windowDecorated() const {
    if (!m_window) {
        m_lastError = "cannot query decoration without a window";
        return std::nullopt;
    }
    clearError();
    const int decorated = m_api.getWindowAttrib(m_window, GLFW_DECORATED);
    if (captureError("window decoration query")) {
        return std::nullopt;
    }
    return decorated == GLFW_TRUE;
}

bool GlfwRuntime::applyWindowConfiguration(
    const Rectangle& bounds,
    bool decorated) {
    if (!m_window || bounds.width <= 0 || bounds.height <= 0) {
        m_lastError = "cannot configure an absent window or empty bounds";
        return false;
    }
    clearError();
    m_api.setWindowAttrib(
        m_window, GLFW_DECORATED, decorated ? GLFW_TRUE : GLFW_FALSE);
    m_api.setWindowMonitor(
        m_window,
        nullptr,
        bounds.x,
        bounds.y,
        bounds.width,
        bounds.height,
        GLFW_DONT_CARE);
    if (captureError("window configuration")) {
        return false;
    }
    const auto actual = windowBounds();
    if (!actual) {
        return false;
    }
    const auto actualDecorated = windowDecorated();
    if (!actualDecorated) {
        return false;
    }
    if (*actual != bounds || *actualDecorated != decorated) {
        m_lastError = "window manager did not apply the requested bounds and decoration";
        return false;
    }
    return true;
}

bool GlfwRuntime::setSwapInterval(int interval) {
    m_swapIntervalUpdateMayHaveMutated = false;
    if (!m_window) {
        m_lastError = "cannot set swap interval without a window";
        return false;
    }
    if (interval != 0 && interval != 1) {
        m_lastError = "only swap intervals zero and one are supported";
        return false;
    }
    clearError();
    const bool supported = m_api.supportsSwapInterval(interval);
    if (captureError("swap interval capability query")) {
        return false;
    }
    if (!supported) {
        m_lastError = interval == 0
            ? "the current context cannot disable vertical synchronization"
            : "the current context cannot enable vertical synchronization";
        return false;
    }
    clearError();
    m_swapIntervalUpdateMayHaveMutated = true;
    m_api.swapInterval(interval);
    return !captureError("swap interval update");
}

void GlfwRuntime::setWindowSizeCallback(WindowSizeCallback callback) const {
    if (m_window) {
        m_api.setWindowSizeCallback(m_window, callback);
    }
}

void GlfwRuntime::setFramebufferSizeCallback(WindowSizeCallback callback) const {
    if (m_window) {
        m_api.setFramebufferSizeCallback(m_window, callback);
    }
}

void GlfwRuntime::clearError() const {
    m_lastError.clear();
    static_cast<void>(m_api.getError(nullptr));
}

bool GlfwRuntime::captureError(std::string_view operation) const {
    const char* description = nullptr;
    const int error = m_api.getError(&description);
    if (error == GLFW_NO_ERROR) {
        return false;
    }
    m_lastError = std::string(operation) + " failed";
    if (description && description[0] != '\0') {
        m_lastError += ": ";
        m_lastError += description;
    }
    return true;
}

void GlfwRuntime::shutdown() noexcept {
    destroyWindow();
    if (std::exchange(m_initialized, false)) {
        m_api.terminate();
    }
}

} // namespace Rigel
