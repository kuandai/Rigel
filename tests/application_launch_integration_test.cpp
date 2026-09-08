#include "TestFramework.h"

#include "ApplicationTestAccess.h"
#include "OpenGLFixture.h"
#include "Rigel/Preferences/UserPreferences.h"

#include <GLFW/glfw3.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

namespace {

class ScopedCurrentDirectory final {
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

struct HeadlessRuntimeState {
    GLFWwindow* window = reinterpret_cast<GLFWwindow*>(0x1);
    GLFWmonitor* monitor = reinterpret_cast<GLFWmonitor*>(0x2);
    GLFWmonitor* monitors[1] = {monitor};
    GLFWvidmode videoMode{};
    int width = 800;
    int height = 600;
    int decorated = GLFW_TRUE;
    bool shouldClose = false;
    size_t polls = 0;
    size_t closeAfterPolls = 0;
    size_t renderedFrames = 0;
};

HeadlessRuntimeState* g_runtime = nullptr;

int initializeRuntime() { return GLFW_TRUE; }
void terminateRuntime() {}
void windowHint(int, int) {}
GLFWwindow* createWindow(
    int width, int height, const char*, GLFWmonitor*, GLFWwindow*) {
    g_runtime->width = width;
    g_runtime->height = height;
    return g_runtime->window;
}
void destroyWindow(GLFWwindow*) {}
void makeContextCurrent(GLFWwindow*) {}
GLFWmonitor** getMonitors(int* count) {
    *count = 1;
    return g_runtime->monitors;
}
GLFWmonitor* getPrimaryMonitor() { return g_runtime->monitor; }
const GLFWvidmode* getVideoMode(GLFWmonitor*) {
    return &g_runtime->videoMode;
}
void getMonitorPos(GLFWmonitor*, int* x, int* y) {
    *x = 0;
    *y = 0;
}
void getWindowPos(GLFWwindow*, int* x, int* y) {
    *x = 0;
    *y = 0;
}
void getWindowSize(GLFWwindow*, int* width, int* height) {
    *width = g_runtime->width;
    *height = g_runtime->height;
}
void getFramebufferSize(GLFWwindow*, int* width, int* height) {
    *width = 64;
    *height = 64;
}
int getWindowAttrib(GLFWwindow*, int attribute) {
    return attribute == GLFW_DECORATED ? g_runtime->decorated : 0;
}
void setWindowAttrib(GLFWwindow*, int attribute, int value) {
    if (attribute == GLFW_DECORATED) {
        g_runtime->decorated = value;
    }
}
void setWindowMonitor(
    GLFWwindow*, GLFWmonitor*, int, int, int width, int height, int) {
    g_runtime->width = width;
    g_runtime->height = height;
}
void setWindowPos(GLFWwindow*, int, int) {}
bool supportsSwapInterval(int) { return true; }
void swapInterval(int) {}
int getError(const char**) { return GLFW_NO_ERROR; }
Rigel::GlfwRuntime::WindowSizeCallback setWindowSizeCallback(
    GLFWwindow*, Rigel::GlfwRuntime::WindowSizeCallback) {
    return nullptr;
}
double runtimeTime() {
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
int windowShouldClose(GLFWwindow*) {
    return g_runtime->shouldClose ? GLFW_TRUE : GLFW_FALSE;
}
void pollEvents() {
    ++g_runtime->polls;
    if (g_runtime->closeAfterPolls != 0 &&
        g_runtime->polls >= g_runtime->closeAfterPolls) {
        g_runtime->shouldClose = true;
    }
}
void swapBuffers(GLFWwindow*) {
    glFinish();
    ++g_runtime->renderedFrames;
}
void setWindowShouldClose(GLFWwindow*, int value) {
    g_runtime->shouldClose = value != GLFW_FALSE;
}

Rigel::GlfwRuntime::Api headlessRuntimeApi() {
    return {
        &initializeRuntime,
        &terminateRuntime,
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
        &setWindowSizeCallback,
        &runtimeTime,
        &windowShouldClose,
        &pollEvents,
        &swapBuffers,
        &setWindowShouldClose,
    };
}

void runAndClose(Rigel::Application& application) {
    application.run();
    application.close();
}

void runNormalApplication(
    HeadlessRuntimeState& runtime,
    const std::filesystem::path& preferencesPath
) {
    runtime.shouldClose = false;
    runtime.polls = 0;
    runtime.closeAfterPolls = 4;
    g_runtime = &runtime;
    Rigel::ApplicationConstructionHooks hooks;
    hooks.runtimeApi = headlessRuntimeApi();
    hooks.userPreferencesPath = preferencesPath;
    hooks.initializeWindowIntegrations = false;
    try {
        Rigel::ApplicationTestAccess::constructAndRun(
            std::move(hooks), &runAndClose);
    } catch (...) {
        g_runtime = nullptr;
        throw;
    }
    g_runtime = nullptr;
}

} // namespace

TEST_CASE(Application_NormalAuthorityLaunchRendersCheckpointsAndRecovers) {
    Rigel::Test::TemporaryDirectory directory(
        "rigel_application_normal_authority_launch");
    ScopedCurrentDirectory currentDirectory(directory.path());
    Rigel::Test::HiddenOpenGLContext context;
    context.require();
    HeadlessRuntimeState runtime;
    runtime.videoMode.width = 1280;
    runtime.videoMode.height = 720;
    runtime.videoMode.refreshRate = 60;

    Rigel::Preferences::UserPreferences preferences;
    preferences.display.vsync = false;
    preferences.display.fpsLimit = 120;
    preferences.graphics.viewDistanceChunks = 8;
    preferences.graphics.shadows = false;
    const std::filesystem::path preferencesPath =
        directory.path() / "config/user-preferences.yaml";
    Rigel::Preferences::UserPreferencesStore(preferencesPath)
        .saveRequested(preferences);

    runNormalApplication(runtime, preferencesPath);
    CHECK(runtime.renderedFrames >= 3);
    const std::filesystem::path authorityRoot =
        directory.path() / "saves/world_0/authority";
    CHECK(std::filesystem::exists(authorityRoot / "current"));

    const size_t firstLaunchFrames = runtime.renderedFrames;
    runNormalApplication(runtime, preferencesPath);
    CHECK(runtime.renderedFrames >= firstLaunchFrames + 3);
    CHECK(std::filesystem::exists(authorityRoot / "current"));
}

TEST_CASE(Application_NormalAuthorityRejectsUnknownSaveWithoutMutation) {
    Rigel::Test::TemporaryDirectory directory(
        "rigel_application_normal_authority_unknown_save");
    ScopedCurrentDirectory currentDirectory(directory.path());
    Rigel::Test::HiddenOpenGLContext context;
    context.require();
    HeadlessRuntimeState runtime;
    runtime.videoMode.width = 1280;
    runtime.videoMode.height = 720;
    runtime.videoMode.refreshRate = 60;

    const auto saveRoot = directory.path() / "saves/world_0";
    std::filesystem::create_directories(saveRoot);
    const auto markerPath = saveRoot / "unknown-format.bin";
    std::ofstream(markerPath, std::ios::binary) << "preserve me";
    const std::filesystem::path preferencesPath =
        directory.path() / "config/user-preferences.yaml";
    Rigel::Preferences::UserPreferencesStore(preferencesPath)
        .saveRequested({});

    CHECK_THROWS(runNormalApplication(runtime, preferencesPath));
    g_runtime = nullptr;
    std::ifstream marker(markerPath, std::ios::binary);
    std::string contents;
    std::getline(marker, contents);
    CHECK_EQ(contents, std::string("preserve me"));
    CHECK(!std::filesystem::exists(saveRoot / "authority"));
}

TEST_CASE(Application_BlockGalleryLaunchUsesProductionLifecycle) {
    Rigel::Test::TemporaryDirectory directory(
        "rigel_application_block_gallery_production_launch");
    ScopedCurrentDirectory currentDirectory(directory.path());
    Rigel::Test::HiddenOpenGLContext context;
    context.require();
    HeadlessRuntimeState runtime;
    runtime.videoMode.width = 1280;
    runtime.videoMode.height = 720;
    runtime.videoMode.refreshRate = 60;
    g_runtime = &runtime;

    Rigel::Preferences::UserPreferences preferences;
    preferences.display.vsync = false;
    preferences.display.fpsLimit = 120;
    preferences.graphics.viewDistanceChunks = 8;
    preferences.graphics.shadows = false;
    const std::filesystem::path preferencesPath =
        directory.path() / "config/user-preferences.yaml";
    Rigel::Preferences::UserPreferencesStore(preferencesPath)
        .saveRequested(preferences);

    const std::filesystem::path existingSave =
        directory.path() / "saves/world_0";
    std::filesystem::create_directories(existingSave);
    {
        std::ofstream(existingSave / "marker.txt") << "preserved";
    }
    const char* arguments[] = {
        "Rigel", "--world-mode", "block-gallery"};

    const Rigel::ApplicationBlockGalleryLifecycleState observed =
        Rigel::ApplicationTestAccess::runBlockGalleryLaunchLifecycle(
            3, arguments, headlessRuntimeApi(), preferencesPath);
    g_runtime = nullptr;

    CHECK_EQ(observed.exitCode, EXIT_SUCCESS);
    CHECK_EQ(observed.decodedWorldMode, Rigel::WorldMode::BlockGallery);
    CHECK_EQ(
        observed.persistenceRoot,
        std::string("developer/block-gallery"));
    CHECK(observed.processPrivateStorage);
    CHECK(observed.resourcesInitialized);
    CHECK(observed.runtimeRegistrationCount > 1);
    CHECK_EQ(
        observed.gallerySpecimenCount +
            observed.emptyGeometryExclusionCount,
        observed.runtimeRegistrationCount);
    CHECK(observed.textureCount > 0);
#ifdef RIGEL_EXPECT_COSMIC_REACH_0_6_1_ASSETS
    CHECK_EQ(observed.runtimeRegistrationCount, static_cast<size_t>(2021));
    CHECK_EQ(observed.gallerySpecimenCount, static_cast<size_t>(2020));
    CHECK_EQ(observed.emptyGeometryExclusionCount, static_cast<size_t>(1));
    CHECK_EQ(observed.textureCount, static_cast<size_t>(276));
#endif
    CHECK(observed.worldBootstrapped);
    CHECK(observed.overviewInstalled);
    CHECK(observed.freeFlyMoved);
    CHECK(observed.specimenLoadedThroughAsyncLoader);
    CHECK(observed.specimenMeshSubmitted);
    CHECK(observed.specimenTargetPresented);
    CHECK(observed.exactTargetMetadataPresented);
    CHECK(observed.targetChangePresented);
    CHECK(observed.noTargetPresented);
    CHECK(observed.galleryMutationsSuppressed);
    CHECK(observed.frameRendererSubmitted);
    CHECK(observed.chunkLoadsStarted > 0);
    CHECK(observed.renderedFrames >= 2);
    CHECK(observed.renderedFrames < 600);
    CHECK(observed.generatedChunkPersistedOnClose);

    std::ifstream marker(existingSave / "marker.txt");
    std::string markerContents;
    marker >> markerContents;
    CHECK_EQ(markerContents, std::string("preserved"));
    CHECK(!std::filesystem::exists(existingSave / "zones"));
    CHECK(!std::filesystem::exists(directory.path() / "developer"));
}
