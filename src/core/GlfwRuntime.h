#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>

struct GLFWmonitor;
struct GLFWwindow;
struct GLFWvidmode;

namespace Rigel {

class GlfwRuntime {
public:
    using WindowSizeCallback = void (*)(GLFWwindow*, int, int);

    struct Rectangle {
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;

        bool operator==(const Rectangle&) const = default;
    };

    struct Api {
        int (*initialize)();
        void (*terminate)();
        void (*windowHint)(int, int);
        GLFWwindow* (*createWindow)(int, int, const char*, GLFWmonitor*, GLFWwindow*);
        void (*destroyWindow)(GLFWwindow*);
        void (*makeContextCurrent)(GLFWwindow*);
        GLFWmonitor** (*getMonitors)(int*);
        GLFWmonitor* (*getPrimaryMonitor)();
        const GLFWvidmode* (*getVideoMode)(GLFWmonitor*);
        void (*getMonitorPos)(GLFWmonitor*, int*, int*);
        void (*getWindowPos)(GLFWwindow*, int*, int*);
        void (*getWindowSize)(GLFWwindow*, int*, int*);
        void (*getFramebufferSize)(GLFWwindow*, int*, int*);
        int (*getWindowAttrib)(GLFWwindow*, int);
        void (*setWindowAttrib)(GLFWwindow*, int, int);
        void (*setWindowMonitor)(
            GLFWwindow*, GLFWmonitor*, int, int, int, int, int);
        void (*setWindowPos)(GLFWwindow*, int, int);
        void (*swapInterval)(int);
        int (*getError)(const char**);
        WindowSizeCallback (*setWindowSizeCallback)(
            GLFWwindow*, WindowSizeCallback);
        WindowSizeCallback (*setFramebufferSizeCallback)(
            GLFWwindow*, WindowSizeCallback);
    };

    GlfwRuntime();
    explicit GlfwRuntime(Api api);
    ~GlfwRuntime();

    GlfwRuntime(const GlfwRuntime&) = delete;
    GlfwRuntime& operator=(const GlfwRuntime&) = delete;

    bool initialize();
    void windowHint(int hint, int value) const;
    GLFWwindow* createWindow(int width, int height, const char* title);
    GLFWwindow* createWindow(int width,
                             int height,
                             const char* title,
                             bool decorated,
                             std::optional<std::pair<int, int>> position);
    GLFWwindow* window() const { return m_window; }
    bool makeContextCurrent() const;
    void destroyWindow() noexcept;

    std::optional<Rectangle> currentDesktopBounds() const;
    Rectangle windowBounds() const;
    std::pair<int, int> framebufferSize() const;
    bool windowDecorated() const;
    bool applyWindowConfiguration(const Rectangle& bounds, bool decorated);
    bool setSwapInterval(int interval);
    void setWindowSizeCallback(WindowSizeCallback callback) const;
    void setFramebufferSizeCallback(WindowSizeCallback callback) const;
    const std::string& lastError() const { return m_lastError; }

    void shutdown() noexcept;

private:
    void clearError() const;
    bool captureError(std::string_view operation) const;

    Api m_api;
    GLFWwindow* m_window = nullptr;
    bool m_initialized = false;
    mutable std::string m_lastError;
};

} // namespace Rigel
