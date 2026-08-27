#include "TestFramework.h"
#include "GlfwRuntime.h"

#include <GLFW/glfw3.h>

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct RuntimeCalls {
    std::vector<std::string> events;
    GLFWwindow* window = reinterpret_cast<GLFWwindow*>(0x1);
    GLFWwindow* destroyedWindow = nullptr;
    GLFWmonitor* monitor = reinterpret_cast<GLFWmonitor*>(0x2);
    GLFWmonitor* monitors[1] = {monitor};
    GLFWvidmode videoMode{};
    Rigel::GlfwRuntime::Rectangle bounds{100, 120, 800, 600};
    std::pair<int, int> framebufferSize{1600, 1200};
    int decorated = GLFW_TRUE;
    int nextDecorated = GLFW_TRUE;
    int error = GLFW_NO_ERROR;
    bool swapIntervalZeroSupported = true;
    bool failWindowPositionQuery = false;
    bool failMonitorPositionQuery = false;
    bool failDecorationQuery = false;
    bool failFramebufferSizeQuery = false;
    std::vector<int> swapIntervals;
    Rigel::GlfwRuntime::WindowSizeCallback windowSizeCallback = nullptr;
    Rigel::GlfwRuntime::WindowSizeCallback framebufferSizeCallback = nullptr;
};

RuntimeCalls* g_calls = nullptr;

int initialize() {
    g_calls->events.emplace_back("initialize");
    return 1;
}

void terminate() {
    g_calls->events.emplace_back("terminate");
}

void windowHint(int hint, int value) {
    if (hint == GLFW_DECORATED) {
        g_calls->nextDecorated = value;
    }
}

GLFWwindow* createWindow(
    int width, int height, const char*, GLFWmonitor*, GLFWwindow*) {
    g_calls->events.emplace_back("create window");
    g_calls->bounds.width = width;
    g_calls->bounds.height = height;
    g_calls->decorated = g_calls->nextDecorated;
    return g_calls->window;
}

void destroyWindow(GLFWwindow* window) {
    g_calls->destroyedWindow = window;
    g_calls->events.emplace_back("destroy window");
}

void makeContextCurrent(GLFWwindow* window) {
    g_calls->events.emplace_back(
        window == nullptr ? "clear context" : "make context current");
}

GLFWmonitor** getMonitors(int* count) {
    *count = 1;
    return g_calls->monitors;
}

GLFWmonitor* getPrimaryMonitor() {
    return g_calls->monitor;
}

const GLFWvidmode* getVideoMode(GLFWmonitor*) {
    return &g_calls->videoMode;
}

void getMonitorPos(GLFWmonitor*, int* x, int* y) {
    *x = 0;
    *y = 0;
    if (g_calls->failMonitorPositionQuery) {
        g_calls->failMonitorPositionQuery = false;
        g_calls->error = GLFW_PLATFORM_ERROR;
    }
}

void getWindowPos(GLFWwindow*, int* x, int* y) {
    *x = g_calls->bounds.x;
    *y = g_calls->bounds.y;
    if (g_calls->failWindowPositionQuery) {
        g_calls->failWindowPositionQuery = false;
        g_calls->error = GLFW_PLATFORM_ERROR;
    }
}

void getWindowSize(GLFWwindow*, int* width, int* height) {
    *width = g_calls->bounds.width;
    *height = g_calls->bounds.height;
}

void getFramebufferSize(GLFWwindow*, int* width, int* height) {
    *width = g_calls->framebufferSize.first;
    *height = g_calls->framebufferSize.second;
    if (g_calls->failFramebufferSizeQuery) {
        g_calls->failFramebufferSizeQuery = false;
        g_calls->error = GLFW_PLATFORM_ERROR;
    }
}

int getWindowAttrib(GLFWwindow*, int attribute) {
    if (g_calls->failDecorationQuery) {
        g_calls->failDecorationQuery = false;
        g_calls->error = GLFW_PLATFORM_ERROR;
    }
    return attribute == GLFW_DECORATED ? g_calls->decorated : 0;
}

void setWindowAttrib(GLFWwindow*, int attribute, int value) {
    if (attribute == GLFW_DECORATED) {
        g_calls->decorated = value;
    }
}

void setWindowMonitor(
    GLFWwindow*, GLFWmonitor*, int x, int y, int width, int height, int) {
    g_calls->bounds = {x, y, width, height};
}

void setWindowPos(GLFWwindow*, int x, int y) {
    g_calls->bounds.x = x;
    g_calls->bounds.y = y;
}

bool supportsSwapInterval(int interval) {
    return interval != 0 || g_calls->swapIntervalZeroSupported;
}

void swapInterval(int interval) {
    g_calls->swapIntervals.push_back(interval);
}

int getError(const char** description) {
    if (description) {
        *description = nullptr;
    }
    return std::exchange(g_calls->error, GLFW_NO_ERROR);
}

Rigel::GlfwRuntime::WindowSizeCallback setWindowSizeCallback(
    GLFWwindow*, Rigel::GlfwRuntime::WindowSizeCallback callback) {
    return std::exchange(g_calls->windowSizeCallback, callback);
}

Rigel::GlfwRuntime::WindowSizeCallback setFramebufferSizeCallback(
    GLFWwindow*, Rigel::GlfwRuntime::WindowSizeCallback callback) {
    return std::exchange(g_calls->framebufferSizeCallback, callback);
}

Rigel::GlfwRuntime::Api fakeApi() {
    return {
        &initialize,
        &terminate,
        &windowHint,
        &createWindow,
        &destroyWindow,
        &makeContextCurrent,
        &getMonitors,
        &getPrimaryMonitor,
        &getVideoMode,
        &getMonitorPos,
        &getWindowPos,
        &getWindowSize,
        &getFramebufferSize,
        &getWindowAttrib,
        &setWindowAttrib,
        &setWindowMonitor,
        &setWindowPos,
        &supportsSwapInterval,
        &swapInterval,
        &getError,
        &setWindowSizeCallback,
        &setFramebufferSizeCallback,
    };
}

} // namespace

TEST_CASE(GlfwRuntime_PartialConstructionCleanupRunsOnce) {
    RuntimeCalls calls;
    calls.videoMode.width = 1920;
    calls.videoMode.height = 1080;
    g_calls = &calls;

    CHECK_THROWS(([&] {
        Rigel::GlfwRuntime runtime(fakeApi());
        CHECK(runtime.initialize());
        CHECK_EQ(runtime.createWindow(800, 600, "Rigel"), calls.window);

        try {
            throw std::runtime_error("construction failed");
        } catch (...) {
            runtime.makeContextCurrent();
            runtime.shutdown();
            runtime.shutdown();
            throw;
        }
    }()));

    const std::vector<std::string> expected = {
        "initialize",
        "create window",
        "make context current",
        "clear context",
        "destroy window",
        "terminate",
    };
    CHECK_EQ(calls.events, expected);
    CHECK_EQ(calls.destroyedWindow, calls.window);

    g_calls = nullptr;
}

TEST_CASE(GlfwRuntime_DistinguishesLogicalAndFramebufferPixels) {
    RuntimeCalls calls;
    calls.videoMode.width = 1920;
    calls.videoMode.height = 1080;
    g_calls = &calls;
    {
        Rigel::GlfwRuntime runtime(fakeApi());
        CHECK(runtime.initialize());
        CHECK(runtime.createWindow(800, 600, "Rigel"));

        CHECK_EQ(runtime.windowBounds()->width, 800);
        CHECK_EQ(runtime.windowBounds()->height, 600);
        CHECK_EQ(
            runtime.framebufferSize(),
            std::optional(std::pair(1600, 1200)));
    }

    g_calls = nullptr;
}

TEST_CASE(GlfwRuntime_RejectsUnsupportedSwapIntervalWithoutCallingPlatform) {
    RuntimeCalls calls;
    calls.videoMode.width = 1920;
    calls.videoMode.height = 1080;
    calls.swapIntervalZeroSupported = false;
    g_calls = &calls;
    {
        Rigel::GlfwRuntime runtime(fakeApi());
        CHECK(runtime.initialize());
        CHECK(runtime.createWindow(800, 600, "Rigel"));
        CHECK(runtime.makeContextCurrent());

        CHECK(!runtime.setSwapInterval(0));
        CHECK(calls.swapIntervals.empty());
        CHECK_NE(
            runtime.lastError().find("cannot disable"), std::string::npos);
    }
    g_calls = nullptr;
}

TEST_CASE(GlfwRuntime_GeometryQueryErrorsDoNotProduceFallbackValues) {
    RuntimeCalls calls;
    calls.videoMode.width = 1920;
    calls.videoMode.height = 1080;
    g_calls = &calls;
    {
        Rigel::GlfwRuntime runtime(fakeApi());
        CHECK(runtime.initialize());
        CHECK(runtime.createWindow(800, 600, "Rigel"));

        calls.failWindowPositionQuery = true;
        CHECK(!runtime.windowBounds());
        CHECK_NE(
            runtime.lastError().find("window position query"),
            std::string::npos);

        calls.failDecorationQuery = true;
        CHECK(!runtime.windowDecorated());
        CHECK_NE(
            runtime.lastError().find("window decoration query"),
            std::string::npos);

        calls.failFramebufferSizeQuery = true;
        CHECK(!runtime.framebufferSize());
        CHECK_NE(
            runtime.lastError().find("framebuffer size query"),
            std::string::npos);

        calls.failMonitorPositionQuery = true;
        CHECK(!runtime.currentDesktopBounds());
        CHECK_NE(
            runtime.lastError().find("monitor position query"),
            std::string::npos);
    }
    g_calls = nullptr;
}
