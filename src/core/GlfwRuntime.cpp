#include "GlfwRuntime.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>

namespace Rigel {

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
          &glfwSwapInterval,
          &glfwGetError,
          &glfwSetWindowSizeCallback,
          &glfwSetFramebufferSizeCallback,
      }) {
}

GlfwRuntime::GlfwRuntime(Api api)
    : m_api(api) {
}

GlfwRuntime::~GlfwRuntime() {
    shutdown();
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
    GLFWmonitor** monitors = m_api.getMonitors(&monitorCount);
    if (!monitors || monitorCount <= 0) {
        GLFWmonitor* primary = m_api.getPrimaryMonitor();
        if (!primary) {
            m_lastError = "no current or default monitor is available";
            return std::nullopt;
        }
        monitors = &primary;
        monitorCount = 1;
    }

    const Rectangle window = windowBounds();
    GLFWmonitor* selected = m_api.getPrimaryMonitor();
    std::int64_t selectedOverlap = -1;
    if (m_window) {
        for (int index = 0; index < monitorCount; ++index) {
            GLFWmonitor* monitor = monitors[index];
            const GLFWvidmode* mode = m_api.getVideoMode(monitor);
            if (!mode) {
                continue;
            }
            int x = 0;
            int y = 0;
            m_api.getMonitorPos(monitor, &x, &y);
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
    const GLFWvidmode* mode = m_api.getVideoMode(selected);
    if (!mode || mode->width <= 0 || mode->height <= 0) {
        m_lastError = "the current or default monitor has no desktop mode";
        return std::nullopt;
    }
    Rectangle result;
    m_api.getMonitorPos(selected, &result.x, &result.y);
    result.width = mode->width;
    result.height = mode->height;
    return result;
}

GlfwRuntime::Rectangle GlfwRuntime::windowBounds() const {
    Rectangle result;
    if (!m_window) {
        return result;
    }
    m_api.getWindowPos(m_window, &result.x, &result.y);
    m_api.getWindowSize(m_window, &result.width, &result.height);
    return result;
}

std::pair<int, int> GlfwRuntime::framebufferSize() const {
    int width = 0;
    int height = 0;
    if (m_window) {
        m_api.getFramebufferSize(m_window, &width, &height);
    }
    return {width, height};
}

bool GlfwRuntime::windowDecorated() const {
    return m_window &&
           m_api.getWindowAttrib(m_window, GLFW_DECORATED) == GLFW_TRUE;
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
    const Rectangle actual = windowBounds();
    if (actual != bounds || windowDecorated() != decorated) {
        m_lastError = "window manager did not apply the requested bounds and decoration";
        return false;
    }
    return true;
}

bool GlfwRuntime::setSwapInterval(int interval) {
    if (!m_window) {
        m_lastError = "cannot set swap interval without a window";
        return false;
    }
    clearError();
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
