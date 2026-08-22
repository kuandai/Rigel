#pragma once

struct GLFWmonitor;
struct GLFWwindow;

namespace Rigel {

class GlfwRuntime {
public:
    struct Api {
        int (*initialize)();
        void (*terminate)();
        void (*windowHint)(int, int);
        GLFWwindow* (*createWindow)(int, int, const char*, GLFWmonitor*, GLFWwindow*);
        void (*destroyWindow)(GLFWwindow*);
        void (*makeContextCurrent)(GLFWwindow*);
    };

    GlfwRuntime();
    explicit GlfwRuntime(Api api);
    ~GlfwRuntime();

    GlfwRuntime(const GlfwRuntime&) = delete;
    GlfwRuntime& operator=(const GlfwRuntime&) = delete;

    bool initialize();
    void windowHint(int hint, int value) const;
    GLFWwindow* createWindow(int width, int height, const char* title);
    GLFWwindow* window() const { return m_window; }
    void makeContextCurrent() const;
    void shutdown() noexcept;

private:
    Api m_api;
    GLFWwindow* m_window = nullptr;
    bool m_initialized = false;
};

} // namespace Rigel
