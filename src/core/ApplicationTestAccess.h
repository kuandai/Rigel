#pragma once

#include "GlfwRuntime.h"

#include <filesystem>
#include <memory>
#include <string>

namespace Rigel {

class Application;
namespace Persistence {
class StorageBackend;
}

enum class ApplicationShutdownStage {
    ContextMadeCurrent,
    UserInterfaceReleased,
    AsyncLoadingStopped,
    WorldsReleased,
    RenderResourcesReleased,
    AssetCacheReleased,
    RuntimeReleased,
};

struct ApplicationConstructionHooks {
    GlfwRuntime::Api runtimeApi;
    void (*afterContextAcquired)() = nullptr;
    void (*shutdownStageCompleted)(ApplicationShutdownStage) noexcept = nullptr;
    std::filesystem::path userPreferencesPath;
};

struct ApplicationCloseHooks {
    std::shared_ptr<Persistence::StorageBackend> persistenceStorage;
    void (*closeFailureObserved)(bool dirtyWorld) = nullptr;
    void (*shutdownStageCompleted)(ApplicationShutdownStage) noexcept = nullptr;
    std::string persistenceRoot = "application-close-test";
};

class ApplicationTestAccess {
public:
    static void construct(ApplicationConstructionHooks hooks);
    static void constructAndRun(
        ApplicationConstructionHooks hooks,
        void (*runLoop)(Application&));
    static void closeReadyWorld(ApplicationCloseHooks hooks);
    static bool initializeOptionalUserInterface(
        GLFWwindow* window,
        bool (*initialize)(GLFWwindow*)) noexcept;
};

} // namespace Rigel
