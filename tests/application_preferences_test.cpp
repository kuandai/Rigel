#include "TestFramework.h"

#include "ApplicationPreferences.h"
#include "Rigel/Persistence/Storage.h"
#include "Rigel/Render/FrameRenderer.h"

#include <GLFW/glfw3.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Rigel::Preferences::detail {

void setUserPreferencesAfterSavePreflightHookForTesting(
    std::function<void()> hook);
void setUserPreferencesAfterPublicationHookForTesting(
    std::function<void()> hook);

} // namespace Rigel::Preferences::detail

namespace {

struct FakeDisplay {
    GLFWwindow* window = reinterpret_cast<GLFWwindow*>(0x1);
    GLFWmonitor* monitors[2] = {
        reinterpret_cast<GLFWmonitor*>(0x2),
        reinterpret_cast<GLFWmonitor*>(0x3)};
    GLFWvidmode modes[2]{};
    Rigel::GlfwRuntime::Rectangle bounds{2100, 120, 800, 600};
    std::pair<int, int> framebuffer{1600, 1200};
    int decorated = GLFW_TRUE;
    int nextDecorated = GLFW_TRUE;
    int error = GLFW_NO_ERROR;
    int createFailuresRemaining = 0;
    int configurationFailuresRemaining = 0;
    int swapFailuresRemaining = 0;
    int createAttempts = 0;
    int savePreflights = 0;
    std::vector<int> swapIntervals;
    Rigel::ApplicationPreferences* resizeObserver = nullptr;
};

FakeDisplay* g_display = nullptr;
double g_now = 0.0;

int initialize() { return GLFW_TRUE; }
void terminate() {}
void windowHint(int hint, int value) {
    if (hint == GLFW_DECORATED) {
        g_display->nextDecorated = value;
    }
}
GLFWwindow* createWindow(
    int width, int height, const char*, GLFWmonitor*, GLFWwindow*) {
    ++g_display->createAttempts;
    if (g_display->createFailuresRemaining > 0) {
        --g_display->createFailuresRemaining;
        return nullptr;
    }
    g_display->bounds.width = width;
    g_display->bounds.height = height;
    g_display->decorated = g_display->nextDecorated;
    return g_display->window;
}
void destroyWindow(GLFWwindow*) {}
void makeContextCurrent(GLFWwindow*) {}
GLFWmonitor** getMonitors(int* count) {
    *count = 2;
    return g_display->monitors;
}
GLFWmonitor* getPrimaryMonitor() { return g_display->monitors[0]; }
const GLFWvidmode* getVideoMode(GLFWmonitor* monitor) {
    return monitor == g_display->monitors[0]
        ? &g_display->modes[0]
        : &g_display->modes[1];
}
void getMonitorPos(GLFWmonitor* monitor, int* x, int* y) {
    *x = monitor == g_display->monitors[0] ? 0 : 1920;
    *y = 0;
}
void getWindowPos(GLFWwindow*, int* x, int* y) {
    *x = g_display->bounds.x;
    *y = g_display->bounds.y;
}
void getWindowSize(GLFWwindow*, int* width, int* height) {
    *width = g_display->bounds.width;
    *height = g_display->bounds.height;
}
void getFramebufferSize(GLFWwindow*, int* width, int* height) {
    *width = g_display->framebuffer.first;
    *height = g_display->framebuffer.second;
}
int getWindowAttrib(GLFWwindow*, int attribute) {
    return attribute == GLFW_DECORATED ? g_display->decorated : 0;
}
void setWindowAttrib(GLFWwindow*, int attribute, int value) {
    if (attribute == GLFW_DECORATED) {
        g_display->decorated = value;
    }
}
void setWindowMonitor(
    GLFWwindow*, GLFWmonitor*, int x, int y, int width, int height, int) {
    g_display->bounds = {x, y, width, height};
    if (g_display->resizeObserver) {
        g_display->resizeObserver->observeLogicalResize(
            width, height, g_now);
    }
    if (g_display->configurationFailuresRemaining > 0) {
        --g_display->configurationFailuresRemaining;
        g_display->error = GLFW_PLATFORM_ERROR;
    }
}
void setWindowPos(GLFWwindow*, int x, int y) {
    g_display->bounds.x = x;
    g_display->bounds.y = y;
}
void swapInterval(int interval) {
    g_display->swapIntervals.push_back(interval);
    if (g_display->swapFailuresRemaining > 0) {
        --g_display->swapFailuresRemaining;
        g_display->error = GLFW_PLATFORM_ERROR;
    }
}
int getError(const char** description) {
    if (description) {
        *description = g_display->error == GLFW_NO_ERROR
            ? nullptr
            : "injected display failure";
    }
    return std::exchange(g_display->error, GLFW_NO_ERROR);
}
Rigel::GlfwRuntime::WindowSizeCallback setWindowSizeCallback(
    GLFWwindow*, Rigel::GlfwRuntime::WindowSizeCallback) {
    return nullptr;
}
Rigel::GlfwRuntime::WindowSizeCallback setFramebufferSizeCallback(
    GLFWwindow*, Rigel::GlfwRuntime::WindowSizeCallback) {
    return nullptr;
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
        &swapInterval,
        &getError,
        &setWindowSizeCallback,
        &setFramebufferSizeCallback,
    };
}

double now() { return g_now; }
void sleepUntil(double deadline) { g_now = deadline; }

std::string readDocument(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return std::string(
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>());
}

void failBeforePublication() {
    throw Rigel::Persistence::AtomicFilePublicationError(
        Rigel::Persistence::AtomicFilePublicationState::NotPublished,
        "injected prepublication failure");
}

void failAfterPublication() {
    throw Rigel::Persistence::AtomicFilePublicationError(
        Rigel::Persistence::AtomicFilePublicationState::
            PublishedDurabilityUncertain,
        "injected durability uncertainty");
}

void countPreflight() {
    ++g_display->savePreflights;
}

class DisplayFixture {
private:
    Rigel::Test::TemporaryDirectory m_directory;

public:
    DisplayFixture()
        : m_directory("rigel_application_preferences")
        , path(m_directory.path() / "user-preferences.yaml")
        , runtime(fakeApi()) {
        g_display = &display;
        display.modes[0].width = 1920;
        display.modes[0].height = 1080;
        display.modes[1].width = 2560;
        display.modes[1].height = 1440;
        CHECK(runtime.initialize());
    }

    ~DisplayFixture() {
        runtime.shutdown();
        Rigel::Preferences::detail::
            setUserPreferencesAfterSavePreflightHookForTesting({});
        Rigel::Preferences::detail::
            setUserPreferencesAfterPublicationHookForTesting({});
        g_display = nullptr;
    }

    Rigel::ApplicationPreferences owner(
        const Rigel::Preferences::UserPreferences& requested) {
        Rigel::Preferences::UserPreferencesStore(path).saveRequested(requested);
        Rigel::ApplicationPreferences result(
            path,
            Rigel::Core::FramePacer::Clock{&now, &sleepUntil});
        result.load();
        return result;
    }

    FakeDisplay display;
    std::filesystem::path path;
    Rigel::GlfwRuntime runtime;
};

} // namespace

TEST_CASE(ApplicationPreferences_StartupConsumesRequestWithoutWriting) {
    DisplayFixture fixture;
    Rigel::Preferences::UserPreferences requested;
    requested.display.mode = Rigel::Preferences::DisplayMode::Borderless;
    requested.display.windowedSize = {1234, 777};
    requested.display.vsync = false;
    requested.display.fpsLimit = 144;
    requested.camera.verticalFovDegrees = 92.0;
    auto preferences = fixture.owner(requested);
    const std::string before = readDocument(fixture.path);

    const auto startup =
        preferences.initializeDisplay(fixture.runtime, false);

    CHECK(!startup.usedSafeFallback);
    CHECK_EQ(fixture.display.bounds,
             (Rigel::GlfwRuntime::Rectangle{0, 0, 1920, 1080}));
    CHECK_EQ(preferences.requested(), requested);
    CHECK_EQ(preferences.effectiveDisplay(), requested.display);
    CHECK_EQ(preferences.effectiveVerticalFovDegrees(), 92.0);
    CHECK_EQ(readDocument(fixture.path), before);
}

TEST_CASE(ApplicationPreferences_StartupFallbackIsFixedAndNonMutating) {
    DisplayFixture fixture;
    Rigel::Preferences::UserPreferences requested;
    requested.display.windowedSize = {1600, 900};
    requested.display.fpsLimit = 120;
    auto preferences = fixture.owner(requested);
    const std::string before = readDocument(fixture.path);
    fixture.display.createFailuresRemaining = 1;

    const auto startup =
        preferences.initializeDisplay(fixture.runtime, false);

    CHECK(startup.usedSafeFallback);
    CHECK_EQ(fixture.display.createAttempts, 2);
    CHECK_EQ(fixture.display.bounds.width, 800);
    CHECK_EQ(fixture.display.bounds.height, 600);
    CHECK_EQ(preferences.requested(), requested);
    CHECK_EQ(
        preferences.effectiveDisplay().mode,
        Rigel::Preferences::DisplayMode::Windowed);
    CHECK(!preferences.effectiveDisplay().vsync);
    CHECK(!preferences.effectiveDisplay().fpsLimit);
    CHECK_EQ(readDocument(fixture.path), before);
}

TEST_CASE(ApplicationPreferences_StartupIsFatalWhenFallbackFails) {
    DisplayFixture fixture;
    Rigel::Preferences::UserPreferences requested;
    auto preferences = fixture.owner(requested);
    fixture.display.createFailuresRemaining = 2;

    CHECK_THROWS(preferences.initializeDisplay(fixture.runtime, false));
    CHECK_EQ(fixture.display.createAttempts, 2);
    CHECK_EQ(fixture.runtime.window(), nullptr);
}

TEST_CASE(ApplicationPreferences_BenchmarkOverridesEffectivePacingOnly) {
    DisplayFixture fixture;
    Rigel::Preferences::UserPreferences requested;
    requested.display.vsync = true;
    requested.display.fpsLimit = 165;
    auto preferences = fixture.owner(requested);
    const std::string before = readDocument(fixture.path);

    preferences.initializeDisplay(fixture.runtime, true);

    CHECK(preferences.requested().display.vsync);
    CHECK_EQ(preferences.requested().display.fpsLimit, std::optional<int>(165));
    CHECK(!preferences.effectiveDisplay().vsync);
    CHECK(!preferences.effectiveDisplay().fpsLimit);
    CHECK_EQ(fixture.display.swapIntervals.back(), 0);
    CHECK_EQ(readDocument(fixture.path), before);
}

TEST_CASE(ApplicationPreferences_BorderlessRoundTripRemembersWindowedSize) {
    DisplayFixture fixture;
    Rigel::Preferences::UserPreferences requested;
    requested.display.windowedSize = {1000, 700};
    auto preferences = fixture.owner(requested);
    preferences.initializeDisplay(fixture.runtime, false);
    fixture.display.resizeObserver = &preferences;

    auto borderless = requested.display;
    borderless.mode = Rigel::Preferences::DisplayMode::Borderless;
    CHECK_EQ(
        preferences.applyDisplay(fixture.runtime, borderless).status,
        Rigel::PreferenceApplyStatus::Applied);
    CHECK_EQ(fixture.display.bounds,
             (Rigel::GlfwRuntime::Rectangle{1920, 0, 2560, 1440}));
    CHECK_EQ(preferences.effectiveDisplay().windowedSize,
             (Rigel::Preferences::WindowedSize{1000, 700}));
    CHECK(!preferences.flushResizePersistence(10.0));

    preferences.observeLogicalResize(400, 300, 10.0);
    CHECK(!preferences.flushResizePersistence(11.0));

    auto windowed = borderless;
    windowed.mode = Rigel::Preferences::DisplayMode::Windowed;
    CHECK_EQ(
        preferences.applyDisplay(fixture.runtime, windowed).status,
        Rigel::PreferenceApplyStatus::Applied);
    CHECK_EQ(fixture.display.bounds,
             (Rigel::GlfwRuntime::Rectangle{2100, 120, 1000, 700}));
    CHECK(!preferences.flushResizePersistence(12.0));
}

TEST_CASE(ApplicationPreferences_VSyncFailureRollsBackBeforeFpsPublication) {
    DisplayFixture fixture;
    Rigel::Preferences::UserPreferences requested;
    auto preferences = fixture.owner(requested);
    preferences.initializeDisplay(fixture.runtime, false);
    auto candidate = requested.display;
    candidate.vsync = false;
    candidate.fpsLimit = 144;
    fixture.display.swapFailuresRemaining = 1;

    const auto rejected =
        preferences.applyDisplay(fixture.runtime, candidate);

    CHECK_EQ(rejected.status, Rigel::PreferenceApplyStatus::Rejected);
    CHECK_EQ(preferences.requested(), requested);
    CHECK_EQ(preferences.effectiveDisplay(), requested.display);
    CHECK_EQ(fixture.display.swapIntervals.back(), 1);

    const auto applied =
        preferences.applyDisplay(fixture.runtime, candidate);
    CHECK_EQ(applied.status, Rigel::PreferenceApplyStatus::Applied);
    CHECK(!preferences.effectiveDisplay().vsync);
    CHECK_EQ(
        preferences.effectiveDisplay().fpsLimit, std::optional<int>(144));
    CHECK_EQ(fixture.display.swapIntervals.back(), 0);
}

TEST_CASE(ApplicationPreferences_HardwareFailureRollsBackWithoutSaving) {
    DisplayFixture fixture;
    Rigel::Preferences::UserPreferences requested;
    auto preferences = fixture.owner(requested);
    preferences.initializeDisplay(fixture.runtime, false);
    const std::string before = readDocument(fixture.path);
    const auto oldBounds = fixture.display.bounds;
    fixture.display.configurationFailuresRemaining = 1;
    auto candidate = requested.display;
    candidate.windowedSize = {1280, 720};

    const auto result = preferences.applyDisplay(fixture.runtime, candidate);

    CHECK_EQ(result.status, Rigel::PreferenceApplyStatus::Rejected);
    CHECK_EQ(fixture.display.bounds, oldBounds);
    CHECK_EQ(preferences.requested(), requested);
    CHECK_EQ(preferences.effectiveDisplay(), requested.display);
    CHECK_EQ(readDocument(fixture.path), before);
}

TEST_CASE(ApplicationPreferences_RollbackFailureIsReportedAsFatal) {
    DisplayFixture fixture;
    Rigel::Preferences::UserPreferences requested;
    auto preferences = fixture.owner(requested);
    preferences.initializeDisplay(fixture.runtime, false);
    auto candidate = requested.display;
    candidate.windowedSize = {1280, 720};
    fixture.display.configurationFailuresRemaining = 2;

    CHECK_THROWS(preferences.applyDisplay(fixture.runtime, candidate));
    CHECK_EQ(preferences.requested(), requested);
    CHECK_EQ(
        Rigel::Preferences::UserPreferencesStore(fixture.path).load(),
        requested);
}

TEST_CASE(ApplicationPreferences_PublicationOutcomesMatchPhysicalState) {
    DisplayFixture fixture;
    Rigel::Preferences::UserPreferences requested;
    auto preferences = fixture.owner(requested);
    preferences.initializeDisplay(fixture.runtime, false);
    const auto oldBounds = fixture.display.bounds;
    auto candidate = requested.display;
    candidate.windowedSize = {1280, 720};
    Rigel::Preferences::detail::
        setUserPreferencesAfterSavePreflightHookForTesting(
            &failBeforePublication);

    const auto notPublished =
        preferences.applyDisplay(fixture.runtime, candidate);

    CHECK_EQ(
        notPublished.status, Rigel::PreferenceApplyStatus::NotPublished);
    CHECK_EQ(fixture.display.bounds, oldBounds);
    CHECK_EQ(preferences.requested(), requested);
    CHECK_EQ(preferences.effectiveDisplay(), requested.display);

    Rigel::Preferences::detail::
        setUserPreferencesAfterSavePreflightHookForTesting({});
    Rigel::Preferences::detail::
        setUserPreferencesAfterPublicationHookForTesting(
            &failAfterPublication);
    const auto uncertain =
        preferences.applyDisplay(fixture.runtime, candidate);

    CHECK_EQ(
        uncertain.status,
        Rigel::PreferenceApplyStatus::PublishedDurabilityUncertain);
    CHECK_EQ(fixture.display.bounds.width, 1280);
    CHECK_EQ(preferences.requested().display, candidate);
    CHECK_EQ(preferences.effectiveDisplay(), candidate);
    CHECK_EQ(
        Rigel::Preferences::UserPreferencesStore(fixture.path)
            .load()
            .display,
        candidate);
}

TEST_CASE(ApplicationPreferences_ResizeDebouncesAndCoalescesOneSave) {
    DisplayFixture fixture;
    Rigel::Preferences::UserPreferences requested;
    auto preferences = fixture.owner(requested);
    preferences.initializeDisplay(fixture.runtime, false);
    Rigel::Preferences::detail::
        setUserPreferencesAfterSavePreflightHookForTesting(&countPreflight);

    preferences.observeLogicalResize(900, 700, 1.0);
    preferences.observeLogicalResize(1100, 750, 1.1);
    CHECK(!preferences.flushResizePersistence(1.34));
    const auto saved = preferences.flushResizePersistence(1.36);

    CHECK(saved.has_value());
    CHECK_EQ(saved->status, Rigel::PreferenceApplyStatus::Applied);
    CHECK_EQ(fixture.display.savePreflights, 1);
    CHECK_EQ(preferences.requested().display.windowedSize,
             (Rigel::Preferences::WindowedSize{1100, 750}));
    CHECK(!preferences.flushResizePersistence(5.0));
}

TEST_CASE(ApplicationPreferences_ResizeFailureLeavesPhysicalSizeUnsaved) {
    DisplayFixture fixture;
    Rigel::Preferences::UserPreferences requested;
    auto preferences = fixture.owner(requested);
    preferences.initializeDisplay(fixture.runtime, false);
    preferences.observeLogicalResize(1200, 800, 1.0);
    Rigel::Preferences::detail::
        setUserPreferencesAfterSavePreflightHookForTesting(
            &failBeforePublication);

    const auto result = preferences.flushResizePersistence(1.25);

    CHECK(result.has_value());
    CHECK_EQ(result->status, Rigel::PreferenceApplyStatus::NotPublished);
    CHECK_EQ(preferences.requested().display.windowedSize,
             requested.display.windowedSize);
    CHECK_EQ(preferences.effectiveDisplay().windowedSize,
             (Rigel::Preferences::WindowedSize{1200, 800}));
    CHECK(!preferences.flushResizePersistence(5.0));
}

TEST_CASE(ApplicationPreferences_FovPublicationFailureRestoresProjection) {
    DisplayFixture fixture;
    Rigel::Preferences::UserPreferences requested;
    auto preferences = fixture.owner(requested);
    Rigel::Render::FrameRenderer renderer;
    renderer.setVerticalFovDegrees(requested.camera.verticalFovDegrees);
    Rigel::Preferences::detail::
        setUserPreferencesAfterSavePreflightHookForTesting(
            &failBeforePublication);

    const auto result = preferences.applyVerticalFov(renderer, 90.0);

    CHECK_EQ(result.status, Rigel::PreferenceApplyStatus::NotPublished);
    CHECK_EQ(preferences.requested(), requested);
    CHECK_EQ(preferences.effectiveDisplay(), requested.display);
    CHECK_EQ(preferences.effectiveVerticalFovDegrees(),
             requested.camera.verticalFovDegrees);
}
