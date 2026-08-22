#include "GlfwRuntime.h"

#include <GLFW/glfw3.h>

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
    if (!m_initialized || m_window) {
        return m_window;
    }
    m_window = m_api.createWindow(width, height, title, nullptr, nullptr);
    return m_window;
}

void GlfwRuntime::makeContextCurrent() const {
    if (m_window) {
        m_api.makeContextCurrent(m_window);
    }
}

void GlfwRuntime::shutdown() noexcept {
    GLFWwindow* window = std::exchange(m_window, nullptr);
    if (window) {
        m_api.makeContextCurrent(nullptr);
        m_api.destroyWindow(window);
    }
    if (std::exchange(m_initialized, false)) {
        m_api.terminate();
    }
}

} // namespace Rigel
