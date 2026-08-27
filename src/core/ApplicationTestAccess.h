#pragma once

#include "GlfwRuntime.h"
#include "Rigel/Application.h"

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
    void (*afterDisplayInitialized)(Application&) = nullptr;
};

struct ApplicationCloseHooks {
    std::shared_ptr<Persistence::StorageBackend> persistenceStorage;
    void (*closeFailureObserved)(bool dirtyWorld) = nullptr;
    void (*shutdownStageCompleted)(ApplicationShutdownStage) noexcept = nullptr;
    std::string persistenceRoot = "application-close-test";
};

struct ApplicationViewDistanceState {
    PreferenceApplyResult result;
    int requestedChunks = 0;
    int effectiveChunks = 0;
    int streamedChunks = 0;
    float renderDistance = 0.0f;
};

class ApplicationTestAccess {
public:
    static void construct(ApplicationConstructionHooks hooks);
    static void constructAndRun(
        ApplicationConstructionHooks hooks,
        void (*runLoop)(Application&));
    static void closeReadyWorld(ApplicationCloseHooks hooks);
    static void closeWithPendingResize(
        std::filesystem::path userPreferencesPath,
        int width,
        int height);
    static void shutdownWithPendingResize(
        std::filesystem::path userPreferencesPath,
        int width,
        int height);
    static ApplicationViewDistanceState applyViewDistanceAtFrameBoundary(
        std::filesystem::path userPreferencesPath,
        int initialChunks,
        int candidateChunks,
        bool activeSession);
    static bool initializeOptionalUserInterface(
        GLFWwindow* window,
        bool (*initialize)(GLFWwindow*)) noexcept;
};

} // namespace Rigel
