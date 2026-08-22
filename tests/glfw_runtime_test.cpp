#include "TestFramework.h"
#include "GlfwRuntime.h"

#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct RuntimeCalls {
    std::vector<std::string> events;
    GLFWwindow* window = reinterpret_cast<GLFWwindow*>(0x1);
    GLFWwindow* destroyedWindow = nullptr;
};

RuntimeCalls* g_calls = nullptr;

int initialize() {
    g_calls->events.emplace_back("initialize");
    return 1;
}

void terminate() {
    g_calls->events.emplace_back("terminate");
}

void windowHint(int, int) {
}

GLFWwindow* createWindow(int, int, const char*, GLFWmonitor*, GLFWwindow*) {
    g_calls->events.emplace_back("create window");
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

Rigel::GlfwRuntime::Api fakeApi() {
    return {
        &initialize,
        &terminate,
        &windowHint,
        &createWindow,
        &destroyWindow,
        &makeContextCurrent,
    };
}

} // namespace

TEST_CASE(GlfwRuntime_PartialConstructionCleanupRunsOnce) {
    RuntimeCalls calls;
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
