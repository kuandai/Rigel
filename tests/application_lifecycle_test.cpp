#include "TestFramework.h"

#include "ApplicationTestAccess.h"

#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct LifecycleCalls {
    std::vector<std::string> runtime;
    std::vector<Rigel::ApplicationShutdownStage> shutdown;
    GLFWwindow* window = reinterpret_cast<GLFWwindow*>(0x1);
    GLFWwindow* destroyedWindow = nullptr;
};

LifecycleCalls* g_calls = nullptr;

class ScopedLifecycleCalls {
public:
    explicit ScopedLifecycleCalls(LifecycleCalls& calls) {
        g_calls = &calls;
    }

    ~ScopedLifecycleCalls() {
        g_calls = nullptr;
    }
};

int initialize() {
    g_calls->runtime.emplace_back("initialize");
    return 1;
}

void terminate() {
    g_calls->runtime.emplace_back("terminate");
}

void windowHint(int, int) {
}

GLFWwindow* createWindow(int, int, const char*, GLFWmonitor*, GLFWwindow*) {
    g_calls->runtime.emplace_back("create window");
    return g_calls->window;
}

void destroyWindow(GLFWwindow* window) {
    g_calls->destroyedWindow = window;
    g_calls->runtime.emplace_back("destroy window");
}

void makeContextCurrent(GLFWwindow* window) {
    g_calls->runtime.emplace_back(
        window == nullptr ? "clear context" : "make context current");
}

void failAfterContextAcquired() {
    throw std::runtime_error("construction failed");
}

void recordShutdownStage(Rigel::ApplicationShutdownStage stage) noexcept {
    g_calls->shutdown.push_back(stage);
}

Rigel::GlfwRuntime::Api fakeRuntimeApi() {
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

TEST_CASE(Application_ConstructionFailureUsesOrderedShutdownOnce) {
    LifecycleCalls calls;
    ScopedLifecycleCalls scopedCalls(calls);

    CHECK_THROWS(Rigel::ApplicationTestAccess::construct({
        fakeRuntimeApi(),
        &failAfterContextAcquired,
        &recordShutdownStage,
    }));

    const std::vector<Rigel::ApplicationShutdownStage> expectedShutdown = {
        Rigel::ApplicationShutdownStage::ContextMadeCurrent,
        Rigel::ApplicationShutdownStage::UserInterfaceReleased,
        Rigel::ApplicationShutdownStage::AsyncLoadingStopped,
        Rigel::ApplicationShutdownStage::WorldsReleased,
        Rigel::ApplicationShutdownStage::RenderResourcesReleased,
        Rigel::ApplicationShutdownStage::AssetCacheReleased,
        Rigel::ApplicationShutdownStage::RuntimeReleased,
    };
    CHECK_EQ(calls.shutdown, expectedShutdown);

    const std::vector<std::string> expectedRuntime = {
        "initialize",
        "create window",
        "make context current",
        "make context current",
        "clear context",
        "destroy window",
        "terminate",
    };
    CHECK_EQ(calls.runtime, expectedRuntime);
    CHECK_EQ(calls.destroyedWindow, calls.window);
}
