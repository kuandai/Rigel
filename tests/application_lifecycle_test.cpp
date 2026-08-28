#include "TestFramework.h"

#include "ApplicationEntry.h"
#include "ApplicationTestAccess.h"
#include "Rigel/Application.h"
#include "Rigel/Preferences/UserPreferences.h"
#include "Rigel/Persistence/Backends/Memory/MemoryFormat.h"
#include "Rigel/Persistence/PersistenceService.h"
#include "Rigel/Persistence/Storage.h"
#include "Rigel/UI/ImGuiLayer.h"
#include "Rigel/Voxel/Chunk.h"
#include "WorldGenerationTestFixture.h"

#include <GLFW/glfw3.h>

#include <cstdlib>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <spdlog/logger.h>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

namespace Rigel::Preferences::detail {

void setUserPreferencesBeforePublicationHookForTesting(
    std::function<void()> hook);

} // namespace Rigel::Preferences::detail

namespace {

struct LifecycleCalls {
    std::vector<std::string> runtime;
    std::vector<Rigel::ApplicationShutdownStage> shutdown;
    GLFWwindow* window = reinterpret_cast<GLFWwindow*>(0x1);
    GLFWwindow* destroyedWindow = nullptr;
    GLFWmonitor* monitor = reinterpret_cast<GLFWmonitor*>(0x2);
    GLFWmonitor* monitors[1] = {monitor};
    GLFWvidmode videoMode{};
    Rigel::GlfwRuntime::Rectangle windowBounds{100, 120, 800, 600};
    std::pair<int, int> framebufferSize{800, 600};
    int decorated = GLFW_TRUE;
    int nextDecorated = GLFW_TRUE;
    int error = GLFW_NO_ERROR;
    bool runLoopEntered = false;
    size_t persistenceAttempts = 0;
    size_t preferenceSavePreflights = 0;
    size_t windowConfigurationAttempts = 0;
    size_t swapIntervalAttempts = 0;
    Rigel::PreferenceApplyStatus invalidDisplayStatus =
        Rigel::PreferenceApplyStatus::Applied;
    Rigel::PreferenceApplyStatus invalidFovStatus =
        Rigel::PreferenceApplyStatus::Applied;
    bool failPreferencePublication = false;
    bool closeFailureObserved = false;
    bool dirtyAtCloseFailure = false;
    bool shutdownStartedAtCloseFailure = false;
    std::shared_ptr<Rigel::Persistence::StorageBackend> persistenceStorage;
    std::string persistenceRoot;
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

class LogCapture {
public:
    LogCapture()
        : m_previous(spdlog::default_logger())
        , m_logger(std::make_shared<spdlog::logger>(
              "application-lifecycle-test",
              std::make_shared<spdlog::sinks::ostream_sink_mt>(m_output))) {
        m_logger->set_level(spdlog::level::info);
        m_logger->set_pattern("%v");
        spdlog::set_default_logger(m_logger);
    }

    ~LogCapture() {
        spdlog::set_default_logger(m_previous);
    }

    std::string output() {
        m_logger->flush();
        return m_output.str();
    }

private:
    std::ostringstream m_output;
    std::shared_ptr<spdlog::logger> m_previous;
    std::shared_ptr<spdlog::logger> m_logger;
};

class ScopedCurrentDirectory {
public:
    explicit ScopedCurrentDirectory(const std::filesystem::path& path)
        : m_previous(std::filesystem::current_path()) {
        std::filesystem::current_path(path);
    }

    ~ScopedCurrentDirectory() {
        std::error_code error;
        std::filesystem::current_path(m_previous, error);
    }

private:
    std::filesystem::path m_previous;
};

int initialize() {
    g_calls->runtime.emplace_back("initialize");
    return 1;
}

void terminate() {
    g_calls->runtime.emplace_back("terminate");
}

void windowHint(int hint, int value) {
    if (hint == GLFW_DECORATED) {
        g_calls->nextDecorated = value;
    }
}

GLFWwindow* createWindow(
    int width, int height, const char*, GLFWmonitor*, GLFWwindow*) {
    g_calls->runtime.emplace_back("create window");
    g_calls->windowBounds.width = width;
    g_calls->windowBounds.height = height;
    g_calls->decorated = g_calls->nextDecorated;
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
}

void getWindowPos(GLFWwindow*, int* x, int* y) {
    *x = g_calls->windowBounds.x;
    *y = g_calls->windowBounds.y;
}

void getWindowSize(GLFWwindow*, int* width, int* height) {
    *width = g_calls->windowBounds.width;
    *height = g_calls->windowBounds.height;
}

void getFramebufferSize(GLFWwindow*, int* width, int* height) {
    *width = g_calls->framebufferSize.first;
    *height = g_calls->framebufferSize.second;
}

int getWindowAttrib(GLFWwindow*, int attribute) {
    return attribute == GLFW_DECORATED ? g_calls->decorated : 0;
}

void setWindowAttrib(GLFWwindow*, int attribute, int value) {
    if (attribute == GLFW_DECORATED) {
        g_calls->decorated = value;
    }
}

void setWindowMonitor(
    GLFWwindow*, GLFWmonitor*, int x, int y, int width, int height, int) {
    ++g_calls->windowConfigurationAttempts;
    g_calls->windowBounds = {x, y, width, height};
}

void setWindowPos(GLFWwindow*, int x, int y) {
    g_calls->windowBounds.x = x;
    g_calls->windowBounds.y = y;
}

bool supportsSwapInterval(int) {
    return true;
}

void swapInterval(int) {
    ++g_calls->swapIntervalAttempts;
}

int getError(const char** description) {
    if (description) {
        *description = nullptr;
    }
    return std::exchange(g_calls->error, GLFW_NO_ERROR);
}

Rigel::GlfwRuntime::WindowSizeCallback setWindowSizeCallback(
    GLFWwindow*, Rigel::GlfwRuntime::WindowSizeCallback) {
    return nullptr;
}

Rigel::GlfwRuntime::WindowSizeCallback setFramebufferSizeCallback(
    GLFWwindow*, Rigel::GlfwRuntime::WindowSizeCallback) {
    return nullptr;
}

void failAfterContextAcquired() {
    throw std::runtime_error("required bootstrap data unavailable");
}

void validateInvalidApplicationPreferences(Rigel::Application& application) {
    auto invalidDisplay = application.requestedPreferences().display;
    invalidDisplay.windowedSize.width =
        Rigel::Preferences::kMinimumWindowDimension - 1;
    g_calls->invalidDisplayStatus =
        application
            .applyDisplayPreferences(
                invalidDisplay, Rigel::WindowedSizeIntent::Changed)
            .status;
    g_calls->invalidFovStatus = application.applyVerticalFov(
        Rigel::Preferences::kMaximumVerticalFovDegrees + 1.0).status;
    throw std::runtime_error("application preference validation completed");
}

void preferenceSavePreflight() {
    ++g_calls->preferenceSavePreflights;
    if (g_calls->failPreferencePublication) {
        throw Rigel::Persistence::AtomicFilePublicationError(
            Rigel::Persistence::AtomicFilePublicationState::NotPublished,
            "injected preference publication failure");
    }
}

class ScopedPreferenceSavePreflight {
public:
    ScopedPreferenceSavePreflight() {
        Rigel::Preferences::detail::
            setUserPreferencesBeforePublicationHookForTesting(
                &preferenceSavePreflight);
    }

    ~ScopedPreferenceSavePreflight() {
        Rigel::Preferences::detail::
            setUserPreferencesBeforePublicationHookForTesting({});
    }
};

void recordRunLoopEntry(Rigel::Application&) {
    g_calls->runLoopEntered = true;
}

void recordShutdownStage(Rigel::ApplicationShutdownStage stage) noexcept {
    g_calls->shutdown.push_back(stage);
}

bool initializeUiSuccessfully(GLFWwindow*) {
    return true;
}

bool failUiInitialization(GLFWwindow*) {
    return false;
}

bool throwDuringUiInitialization(GLFWwindow*) {
    throw std::runtime_error("injected ImGui backend failure");
}

void runWithOptionalUiFailure() {
    if (Rigel::ApplicationTestAccess::initializeOptionalUserInterface(
            g_calls->window, &throwDuringUiInitialization)) {
        throw std::runtime_error("optional UI failure was accepted");
    }
    g_calls->runLoopEntered = true;
}

enum class PersistenceFailurePoint {
    ChunkWrite,
    JournalPublication,
    EntityWrite,
};

class FailingStorageBackend final : public Rigel::Persistence::StorageBackend {
public:
    explicit FailingStorageBackend(PersistenceFailurePoint failurePoint)
        : m_failurePoint(failurePoint) {
    }

    std::unique_ptr<Rigel::Persistence::ByteReader> openRead(
        const std::string& path
    ) override {
        return m_storage.openRead(path);
    }

    std::unique_ptr<Rigel::Persistence::AtomicWriteSession> openWrite(
        const std::string& path
    ) override {
        if (matches(path)) {
            ++g_calls->persistenceAttempts;
            if (m_failuresRemaining > 0) {
                --m_failuresRemaining;
                fail(path);
            }
        }
        return m_storage.openWrite(path);
    }

    bool exists(const std::string& path) override {
        return m_storage.exists(path);
    }

    Rigel::Persistence::StorageEntryKind entryKind(
        const std::string& path) override {
        return m_storage.entryKind(path);
    }

    void forEachEntry(
        const std::string& path,
        const Rigel::Persistence::StorageEntryVisitor& visitor) override {
        m_storage.forEachEntry(path, visitor);
    }

    std::vector<std::string> list(const std::string& path) override {
        return m_storage.list(path);
    }

    void mkdirs(const std::string& path) override {
        m_storage.mkdirs(path);
    }

    void remove(const std::string& path) override {
        m_storage.remove(path);
    }

private:
    bool matches(const std::string& path) const {
        switch (m_failurePoint) {
            case PersistenceFailurePoint::ChunkWrite:
                return path.find("/regions/region_") != std::string::npos;
            case PersistenceFailurePoint::JournalPublication:
                return path.ends_with("/entity-regions.journal");
            case PersistenceFailurePoint::EntityWrite:
                return path.find("/entities/entityRegion_") != std::string::npos;
        }
        return false;
    }

    [[noreturn]] void fail(const std::string& path) {
        throw std::runtime_error(
            "injected storage failure for " + path);
    }

    PersistenceFailurePoint m_failurePoint;
    size_t m_failuresRemaining = 1;
    Rigel::Persistence::FilesystemBackend m_storage;
};

void observeCloseFailure(bool dirtyWorld) {
    g_calls->closeFailureObserved = true;
    g_calls->dirtyAtCloseFailure = dirtyWorld;
    g_calls->shutdownStartedAtCloseFailure = !g_calls->shutdown.empty();
}

Rigel::GlfwRuntime::Api fakeRuntimeApi() {
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

void runFailingApplication() {
    Rigel::ApplicationTestAccess::constructAndRun(
        {
            fakeRuntimeApi(),
            &failAfterContextAcquired,
            &recordShutdownStage,
        },
        &recordRunLoopEntry);
}

void runApplicationWithCloseFailure() {
    Rigel::Persistence::FormatRegistry formats;
    formats.registerFormat(
        Rigel::Persistence::Backends::Memory::descriptor(),
        Rigel::Persistence::Backends::Memory::factory(),
        Rigel::Persistence::Backends::Memory::probe());
    Rigel::Persistence::PersistenceService service(formats);
    Rigel::Persistence::PersistenceContext context;
    context.rootPath = g_calls->persistenceRoot;
    context.preferredFormat = "memory";
    context.storage = g_calls->persistenceStorage;
    Rigel::Test::installSavedWorldGenerationFixture(
        service,
        context,
        Rigel::Test::savedWorldSettingsFixture(
            "Application Close Test World"));
    Rigel::ApplicationTestAccess::closeReadyWorld({
        g_calls->persistenceStorage,
        &observeCloseFailure,
        &recordShutdownStage,
        g_calls->persistenceRoot,
    });
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

TEST_CASE(Application_BootstrapFailureReturnsFailureBeforeRunLoop) {
    LifecycleCalls calls;
    ScopedLifecycleCalls scopedCalls(calls);
    LogCapture logs;

    const int result = Rigel::runApplication(&runFailingApplication);

    CHECK_EQ(result, EXIT_FAILURE);
    CHECK(!calls.runLoopEntered);
    CHECK(logs.output().find("required bootstrap data unavailable") !=
          std::string::npos);
}

TEST_CASE(Application_ReportsObsoleteGenerationConfigurationAtStartup) {
    LifecycleCalls calls;
    ScopedLifecycleCalls scopedCalls(calls);
    Rigel::Test::TemporaryDirectory directory(
        "rigel_application_obsolete_generation_configuration");
    const auto obsoletePath =
        directory.path() / "config/world_generation.yaml";
    std::filesystem::create_directories(obsoletePath.parent_path());
    std::ofstream(obsoletePath) << "flags:\n  no_carvers: true\n";
    ScopedCurrentDirectory currentDirectory(directory.path());
    LogCapture logs;

    Rigel::ApplicationConstructionHooks hooks;
    hooks.runtimeApi = fakeRuntimeApi();
    hooks.userPreferencesPath =
        directory.path() / "user-preferences.yaml";
    hooks.afterContextAcquired = &failAfterContextAcquired;
    hooks.shutdownStageCompleted = &recordShutdownStage;
    CHECK_THROWS(Rigel::ApplicationTestAccess::construct(std::move(hooks)));

    const std::string output = logs.output();
#if defined(RIGEL_EXPECT_PROFILER_ENABLED)
    CHECK(output.find(
              "Obsolete configuration "
              "'config/world_generation.yaml' is ignored") !=
          std::string::npos);
    CHECK(output.find("assets/manifest.yaml") != std::string::npos);
#else
    CHECK(output.find("Obsolete configuration") == std::string::npos);
#endif
}

TEST_CASE(Application_InstalledPersistencePolicyIgnoresWorkingDirectory) {
    Rigel::Test::TemporaryDirectory directory(
        "rigel_application_installed_persistence_policy");
    const std::vector<std::filesystem::path> obsoletePaths = {
        "persistence.yaml",
        "config/persistence.yaml",
        "config/worlds/0/persistence.yaml",
    };
    for (const auto& path : obsoletePaths) {
        const auto absolute = directory.path() / path;
        std::filesystem::create_directories(absolute.parent_path());
        std::ofstream(absolute)
            << "persistence:\n"
               "  format: memory\n"
               "  providers:\n"
               "    rigel:persistence.cr:\n"
               "      lz4: true\n";
    }
    ScopedCurrentDirectory currentDirectory(directory.path());

    const auto policy =
        Rigel::ApplicationTestAccess::installedPersistencePolicy();

    CHECK_EQ(policy.preferredFormat, std::string("cr"));
    CHECK(!policy.crLz4Enabled);
}

TEST_CASE(Application_InvalidPreferencesRejectBeforeMutationOrPublication) {
    LifecycleCalls calls;
    calls.videoMode.width = 1920;
    calls.videoMode.height = 1080;
    ScopedLifecycleCalls scopedCalls(calls);
    Rigel::Test::TemporaryDirectory directory(
        "rigel_application_invalid_preferences");
    const auto preferencesPath =
        directory.path() / "user-preferences.yaml";
    Rigel::Preferences::UserPreferencesStore(preferencesPath).saveRequested({});
    ScopedPreferenceSavePreflight savePreflight;

    Rigel::ApplicationConstructionHooks hooks;
    hooks.runtimeApi = fakeRuntimeApi();
    hooks.userPreferencesPath = preferencesPath;
    hooks.afterDisplayInitialized = &validateInvalidApplicationPreferences;
    CHECK_THROWS(Rigel::ApplicationTestAccess::construct(std::move(hooks)));

    CHECK_EQ(
        calls.invalidDisplayStatus,
        Rigel::PreferenceApplyStatus::Rejected);
    CHECK_EQ(calls.invalidFovStatus, Rigel::PreferenceApplyStatus::Rejected);
    CHECK_EQ(calls.windowConfigurationAttempts, static_cast<size_t>(0));
    CHECK_EQ(calls.swapIntervalAttempts, static_cast<size_t>(1));
    CHECK_EQ(calls.preferenceSavePreflights, static_cast<size_t>(0));
}

TEST_CASE(Application_RunConsumesViewDistanceAtTheRealFrameBoundary) {
    Rigel::Test::TemporaryDirectory directory(
        "rigel_application_view_distance");
    const auto activePath =
        directory.path() / "active-user-preferences.yaml";

    const auto active =
        Rigel::ApplicationTestAccess::applyViewDistanceAtFrameBoundary(
            activePath, 7, 15, true);

    CHECK_EQ(
        active.requestResult.status,
        Rigel::PreferenceApplyStatus::Applied);
    CHECK_EQ(active.beforeRequestedChunks, 7);
    CHECK_EQ(active.beforePersistedChunks, 7);
    CHECK_EQ(active.beforeEffectiveChunks, 7);
    CHECK_EQ(active.beforeStreamedChunks, 7);
    CHECK_NEAR(
        active.beforeRenderDistance,
        static_cast<float>(8 * Rigel::Voxel::Chunk::SIZE),
        0.0001f);
    CHECK_EQ(active.beforePolicyGeneration, static_cast<uint64_t>(1));
    CHECK_EQ(active.result.status, Rigel::PreferenceApplyStatus::Applied);
    CHECK_EQ(active.requestedChunks, 15);
    CHECK_EQ(active.effectiveChunks, 15);
    CHECK_EQ(active.streamedChunks, 15);
    CHECK_NEAR(
        active.renderDistance,
        static_cast<float>(16 * Rigel::Voxel::Chunk::SIZE),
        0.0001f);
    CHECK_EQ(active.unloadChunks, 16);
    CHECK_EQ(active.preloadRadiusRegions, 1);
    CHECK_NEAR(
        active.projectionFarPlane,
        active.renderDistance +
            static_cast<float>(Rigel::Voxel::Chunk::SIZE),
        0.0001f);
    CHECK_NEAR(
        active.shadowDistanceCeiling,
        active.renderDistance,
        0.0001f);
    CHECK_EQ(active.policyGeneration, static_cast<uint64_t>(2));
    CHECK_EQ(active.worldWorkCoordinatesInspected, static_cast<uint64_t>(0));
    CHECK_EQ(
        Rigel::Preferences::UserPreferencesStore(activePath)
            .load()
            .graphics.viewDistanceChunks,
        15);

    const auto inactivePath =
        directory.path() / "inactive-user-preferences.yaml";
    const auto inactive =
        Rigel::ApplicationTestAccess::applyViewDistanceAtFrameBoundary(
            inactivePath, 7, 15, false);

    CHECK_EQ(inactive.result.status, Rigel::PreferenceApplyStatus::Rejected);
    CHECK_EQ(
        inactive.requestResult.status,
        Rigel::PreferenceApplyStatus::Rejected);
    CHECK_EQ(inactive.requestedChunks, 7);
    CHECK_EQ(inactive.effectiveChunks, 7);
    CHECK_EQ(inactive.streamedChunks, 7);
    CHECK_EQ(inactive.worldWorkCoordinatesInspected, static_cast<uint64_t>(0));
    CHECK_EQ(
        Rigel::Preferences::UserPreferencesStore(inactivePath)
            .load()
            .graphics.viewDistanceChunks,
        7);
}

TEST_CASE(Application_ViewDistancePublicationFailureRestoresActiveSession) {
    Rigel::Test::TemporaryDirectory directory(
        "rigel_application_view_distance_publication_failure");
    const auto path = directory.path() / "user-preferences.yaml";
    Rigel::Preferences::UserPreferences requested;
    requested.graphics.viewDistanceChunks = 7;
    Rigel::Preferences::UserPreferencesStore(path).saveRequested(requested);

    LifecycleCalls calls;
    calls.failPreferencePublication = true;
    ScopedLifecycleCalls scopedCalls(calls);
    ScopedPreferenceSavePreflight savePreflight;

    const auto observed =
        Rigel::ApplicationTestAccess::applyViewDistanceAtFrameBoundary(
            path, 7, 15, true);

    CHECK_EQ(
        observed.result.status,
        Rigel::PreferenceApplyStatus::NotPublished);
    CHECK_EQ(
        observed.requestResult.status,
        Rigel::PreferenceApplyStatus::Applied);
    CHECK_EQ(observed.beforeRequestedChunks, 7);
    CHECK_EQ(observed.beforePersistedChunks, 7);
    CHECK_EQ(observed.beforeEffectiveChunks, 7);
    CHECK_EQ(observed.beforeStreamedChunks, 7);
    CHECK_EQ(observed.requestedChunks, 7);
    CHECK_EQ(observed.effectiveChunks, 7);
    CHECK_EQ(observed.streamedChunks, 7);
    CHECK_NEAR(
        observed.renderDistance,
        static_cast<float>(8 * Rigel::Voxel::Chunk::SIZE),
        0.0001f);
    CHECK_EQ(observed.unloadChunks, 8);
    CHECK_EQ(observed.policyGeneration, static_cast<uint64_t>(1));
    CHECK_EQ(
        observed.worldWorkCoordinatesInspected,
        static_cast<uint64_t>(0));
    CHECK_EQ(
        Rigel::Preferences::UserPreferencesStore(path)
            .load()
            .graphics.viewDistanceChunks,
        7);
}

TEST_CASE(Application_CloseFlushesPendingResizeBeforeDebounceExpires) {
    LifecycleCalls calls;
    ScopedLifecycleCalls scopedCalls(calls);
    Rigel::Test::TemporaryDirectory directory(
        "rigel_application_close_resize");
    const auto preferencesPath =
        directory.path() / "user-preferences.yaml";
    Rigel::Preferences::UserPreferencesStore(preferencesPath).saveRequested({});
    ScopedPreferenceSavePreflight savePreflight;

    Rigel::ApplicationTestAccess::closeWithPendingResize(
        preferencesPath, 1180, 720);

    CHECK_EQ(calls.preferenceSavePreflights, static_cast<size_t>(1));
    CHECK_EQ(
        Rigel::Preferences::UserPreferencesStore(preferencesPath)
            .load()
            .display.windowedSize,
        (Rigel::Preferences::WindowedSize{1180, 720}));
}

TEST_CASE(Application_CloseReportsPendingResizePublicationFailure) {
    LifecycleCalls calls;
    calls.failPreferencePublication = true;
    ScopedLifecycleCalls scopedCalls(calls);
    Rigel::Test::TemporaryDirectory directory(
        "rigel_application_close_resize_failure");
    const auto preferencesPath =
        directory.path() / "user-preferences.yaml";
    Rigel::Preferences::UserPreferencesStore(preferencesPath).saveRequested({});
    ScopedPreferenceSavePreflight savePreflight;

    bool reported = false;
    try {
        Rigel::ApplicationTestAccess::closeWithPendingResize(
            preferencesPath, 1180, 720);
    } catch (const std::exception& error) {
        reported = std::string(error.what()).find(
            "Failed to save pending window resize during application close") !=
            std::string::npos;
    }

    CHECK(reported);
    CHECK_EQ(calls.preferenceSavePreflights, static_cast<size_t>(2));
    CHECK_EQ(
        Rigel::Preferences::UserPreferencesStore(preferencesPath)
            .load()
            .display.windowedSize,
        (Rigel::Preferences::WindowedSize{800, 600}));
}

TEST_CASE(Application_ShutdownFlushesPendingResizeBeforeDebounceExpires) {
    LifecycleCalls calls;
    ScopedLifecycleCalls scopedCalls(calls);
    Rigel::Test::TemporaryDirectory directory(
        "rigel_application_shutdown_resize");
    const auto preferencesPath =
        directory.path() / "user-preferences.yaml";
    Rigel::Preferences::UserPreferencesStore(preferencesPath).saveRequested({});
    ScopedPreferenceSavePreflight savePreflight;

    Rigel::ApplicationTestAccess::shutdownWithPendingResize(
        preferencesPath, 1040, 780);

    CHECK_EQ(calls.preferenceSavePreflights, static_cast<size_t>(1));
    CHECK_EQ(
        Rigel::Preferences::UserPreferencesStore(preferencesPath)
            .load()
            .display.windowedSize,
        (Rigel::Preferences::WindowedSize{1040, 780}));
}

TEST_CASE(Application_ShutdownBoundsPendingResizePublicationRetries) {
    LifecycleCalls calls;
    calls.failPreferencePublication = true;
    ScopedLifecycleCalls scopedCalls(calls);
    Rigel::Test::TemporaryDirectory directory(
        "rigel_application_shutdown_resize_failure");
    const auto preferencesPath =
        directory.path() / "user-preferences.yaml";
    Rigel::Preferences::UserPreferencesStore(preferencesPath).saveRequested({});
    ScopedPreferenceSavePreflight savePreflight;
    LogCapture logs;

    Rigel::ApplicationTestAccess::shutdownWithPendingResize(
        preferencesPath, 1040, 780);

    CHECK_EQ(calls.preferenceSavePreflights, static_cast<size_t>(2));
    CHECK_NE(
        logs.output().find("Pending window resize was not saved"),
        std::string::npos);
    CHECK_EQ(
        Rigel::Preferences::UserPreferencesStore(preferencesPath)
            .load()
            .display.windowedSize,
        (Rigel::Preferences::WindowedSize{800, 600}));
}

TEST_CASE(Application_BlockedResizeDoesNotRetryOrFailClose) {
    LifecycleCalls calls;
    ScopedLifecycleCalls scopedCalls(calls);
    Rigel::Test::TemporaryDirectory directory(
        "rigel_application_blocked_resize");
    const auto preferencesPath =
        directory.path() / "user-preferences.yaml";
    const std::string newer =
        "schema_version: 2\nfuture: preserve-until-explicit-replacement\n";
    std::filesystem::create_directories(preferencesPath.parent_path());
    {
        std::ofstream stream(
            preferencesPath, std::ios::binary | std::ios::trunc);
        stream << newer;
    }
    ScopedPreferenceSavePreflight savePreflight;
    LogCapture logs;

    CHECK_NO_THROW(Rigel::ApplicationTestAccess::closeWithPendingResize(
        preferencesPath, 1160, 740));

    CHECK_EQ(calls.preferenceSavePreflights, static_cast<size_t>(0));
    CHECK_NE(
        logs.output().find("preference persistence is blocked"),
        std::string::npos);
    std::ifstream stream(preferencesPath, std::ios::binary);
    const std::string after{
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()};
    CHECK_EQ(after, newer);
}

TEST_CASE(Application_OptionalUserInterfaceFailuresContinueOnce) {
    LifecycleCalls calls;
    ScopedLifecycleCalls scopedCalls(calls);
    LogCapture logs;

    CHECK(Rigel::ApplicationTestAccess::initializeOptionalUserInterface(
        calls.window, &initializeUiSuccessfully));
    CHECK(!Rigel::ApplicationTestAccess::initializeOptionalUserInterface(
        calls.window, &failUiInitialization));
    CHECK(!Rigel::ApplicationTestAccess::initializeOptionalUserInterface(
        calls.window, &throwDuringUiInitialization));
    Rigel::UI::shutdown();
    Rigel::UI::shutdown();

    const int result = Rigel::runApplication(&runWithOptionalUiFailure);
    CHECK_EQ(result, EXIT_SUCCESS);
    CHECK(calls.runLoopEntered);

    const std::string output = logs.output();
    const std::string identifier = "Optional startup resource 'ImGui'";
    size_t warningCount = 0;
    size_t position = 0;
    while ((position = output.find(identifier, position)) !=
           std::string::npos) {
        ++warningCount;
        position += identifier.size();
    }
    CHECK_EQ(warningCount, static_cast<size_t>(3));
    CHECK(output.find("injected ImGui backend failure") != std::string::npos);
}

TEST_CASE(Application_ClosePersistenceFailuresRetryDuringCleanup) {
    const std::vector<std::pair<PersistenceFailurePoint, std::string>> cases = {
        {PersistenceFailurePoint::ChunkWrite, "application-close-chunk"},
        {PersistenceFailurePoint::JournalPublication, "application-close-journal"},
        {PersistenceFailurePoint::EntityWrite, "application-close-entity"},
    };

    for (const auto& [failurePoint, root] : cases) {
        LifecycleCalls calls;
        ScopedLifecycleCalls scopedCalls(calls);
        LogCapture logs;
        calls.persistenceRoot = root;
        calls.persistenceStorage =
            std::make_shared<FailingStorageBackend>(failurePoint);

        const int result = Rigel::runApplication(&runApplicationWithCloseFailure);

        CHECK_EQ(result, EXIT_FAILURE);
        CHECK(calls.closeFailureObserved);
        CHECK(calls.dirtyAtCloseFailure);
        CHECK(!calls.shutdownStartedAtCloseFailure);
        const size_t expectedAttempts =
            failurePoint == PersistenceFailurePoint::EntityWrite ? 3 : 2;
        CHECK_EQ(calls.persistenceAttempts, expectedAttempts);
        CHECK(!calls.shutdown.empty());
        CHECK(!calls.persistenceStorage->exists(
            root + "/entity-regions.journal"));
        CHECK(calls.persistenceStorage->exists(
            root +
            "/zones/rigel/default/entities/entityRegion_0_0_0.mem"));
        CHECK(calls.persistenceStorage->exists(
            root + "/zones/rigel/default/regions/region_0_0_0.mem"));
        CHECK(logs.output().find(
                  "Failed to save world during application close") !=
              std::string::npos);
        CHECK(logs.output().find(
                  "injected storage failure for " + root) !=
              std::string::npos);
        CHECK(logs.output().find("Application terminated successfully") ==
              std::string::npos);
    }
}
