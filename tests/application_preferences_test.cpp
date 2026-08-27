#include "TestFramework.h"

#include "ApplicationPreferences.h"
#include "FrameRendererTestAccess.h"
#include "Rigel/Asset/AssetManager.h"
#include "Rigel/Persistence/Storage.h"
#include "Rigel/Render/FrameRenderer.h"
#include "Rigel/input/GameplayInput.h"
#include "Rigel/input/InputBindingsLoader.h"

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
void setUserPreferencesBeforePublicationHookForTesting(
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
    int configurationAttempts = 0;
    int savePreflights = 0;
    bool swapIntervalZeroSupported = true;
    bool failWindowPositionQuery = false;
    bool failMonitorPositionQuery = false;
    bool deferConfigurationResizeCallbacks = false;
    std::vector<std::pair<int, int>> deferredLogicalResizes;
    std::vector<int> swapIntervals;
    std::vector<double> sleepDeadlines;
    Rigel::Input::InputCallbackContext* inputCallbacks = nullptr;
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
    if (g_display->failMonitorPositionQuery) {
        g_display->failMonitorPositionQuery = false;
        g_display->error = GLFW_PLATFORM_ERROR;
    }
}
void getWindowPos(GLFWwindow*, int* x, int* y) {
    *x = g_display->bounds.x;
    *y = g_display->bounds.y;
    if (g_display->failWindowPositionQuery) {
        g_display->failWindowPositionQuery = false;
        g_display->error = GLFW_PLATFORM_ERROR;
    }
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
    ++g_display->configurationAttempts;
    g_display->bounds = {x, y, width, height};
    if (g_display->inputCallbacks &&
        g_display->inputCallbacks->logicalResize) {
        if (g_display->deferConfigurationResizeCallbacks) {
            g_display->deferredLogicalResizes.emplace_back(width, height);
        } else {
            g_display->inputCallbacks->logicalResize(
                g_display->inputCallbacks->logicalResizeContext,
                width,
                height);
        }
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
bool supportsSwapInterval(int interval) {
    return interval != 0 || g_display->swapIntervalZeroSupported;
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
        &supportsSwapInterval,
        &swapInterval,
        &getError,
        &setWindowSizeCallback,
        &setFramebufferSizeCallback,
    };
}

double now() { return g_now; }
void sleepUntil(double deadline) {
    g_display->sleepDeadlines.push_back(deadline);
    g_now = deadline;
}

std::string readDocument(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return std::string(
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>());
}

void writeDocument(
    const std::filesystem::path& path,
    const std::string& document) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << document;
}

std::string maximumRetainedDocument() {
    constexpr size_t maximumDocumentBytes = 262144;
    const std::string prefix = "schema_version: 1\nretained: ";
    const std::string suffix = "\n";
    return prefix +
        std::string(
               maximumDocumentBytes - prefix.size() - suffix.size(),
               'x') +
        suffix;
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

void countAndFailBeforePublication() {
    ++g_display->savePreflights;
    failBeforePublication();
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
        g_now = 0.0;
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
            setUserPreferencesBeforePublicationHookForTesting({});
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

    void connectResizeCallback(Rigel::ApplicationPreferences& preferences) {
        Rigel::registerApplicationPreferenceCallbacks(
            inputCallbacks, preferences);
        display.inputCallbacks = &inputCallbacks;
        preferenceOwner = &preferences;
    }

    void manualResize(int width, int height, double observedAt) {
        g_now = observedAt;
        display.bounds.width = width;
        display.bounds.height = height;
        emitResizeCallback(width, height);
        CHECK(!finishEventBatch(observedAt));
    }

    void emitResizeCallback(int width, int height) {
        CHECK(inputCallbacks.logicalResize != nullptr);
        inputCallbacks.logicalResize(
            inputCallbacks.logicalResizeContext, width, height);
    }

    std::optional<Rigel::PreferenceApplyResult> finishEventBatch(
        double observedAt) {
        CHECK(preferenceOwner != nullptr);
        g_now = observedAt;
        return preferenceOwner->consumeLogicalResize(runtime, observedAt);
    }

    void dispatchDeferredResizeCallbacks() {
        const auto deferred = std::exchange(
            display.deferredLogicalResizes,
            std::vector<std::pair<int, int>>{});
        for (const auto [width, height] : deferred) {
            emitResizeCallback(width, height);
        }
    }

    FakeDisplay display;
    Rigel::Input::InputCallbackContext inputCallbacks;
    Rigel::ApplicationPreferences* preferenceOwner = nullptr;
    std::filesystem::path path;
    Rigel::GlfwRuntime runtime;
};

class InputFixture {
public:
    InputFixture() {
        assets.loadManifest("manifest.yaml");
        assets.registerLoader(
            "input",
            std::make_unique<Rigel::Input::InputBindingsLoader>());
        defaults = Rigel::Input::loadPlayerDefaultBindings(assets);
    }

    Rigel::Asset::AssetManager assets;
    std::shared_ptr<const Rigel::Input::InputBindings> defaults;
    Rigel::Input::InputState input;
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
    fixture.display.createFailuresRemaining = 3;

    CHECK_THROWS(preferences.initializeDisplay(fixture.runtime, false));
    CHECK_EQ(fixture.display.createAttempts, 3);
    CHECK_EQ(fixture.runtime.window(), nullptr);
}

TEST_CASE(ApplicationPreferences_StartupFallbackCanUseSupportedVSyncOn) {
    DisplayFixture fixture;
    Rigel::Preferences::UserPreferences requested;
    requested.display.vsync = false;
    auto preferences = fixture.owner(requested);
    const std::string before = readDocument(fixture.path);
    fixture.display.swapIntervalZeroSupported = false;

    const auto startup =
        preferences.initializeDisplay(fixture.runtime, false);

    CHECK(startup.usedSafeFallback);
    CHECK_EQ(fixture.display.createAttempts, 3);
    CHECK_EQ(fixture.display.bounds.width, 800);
    CHECK_EQ(fixture.display.bounds.height, 600);
    CHECK(preferences.effectiveDisplay().vsync);
    CHECK(!preferences.effectiveDisplay().fpsLimit);
    CHECK_EQ(preferences.requested(), requested);
    CHECK_EQ(readDocument(fixture.path), before);
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
    preferences.waitForNextFrame();
    CHECK(fixture.display.sleepDeadlines.empty());
    CHECK_EQ(readDocument(fixture.path), before);
}

TEST_CASE(ApplicationPreferences_BenchmarkRejectsVSyncOnFallback) {
    DisplayFixture fixture;
    Rigel::Preferences::UserPreferences requested;
    requested.display.vsync = true;
    auto preferences = fixture.owner(requested);
    const std::string before = readDocument(fixture.path);
    fixture.display.swapIntervalZeroSupported = false;

    CHECK_THROWS(preferences.initializeDisplay(fixture.runtime, true));
    CHECK_EQ(fixture.display.createAttempts, 2);
    CHECK_EQ(fixture.runtime.window(), nullptr);
    CHECK_EQ(preferences.requested(), requested);
    CHECK_EQ(readDocument(fixture.path), before);
}

TEST_CASE(ApplicationPreferences_RequestedFpsLimitDrivesPacingDeadlines) {
    DisplayFixture fixture;
    Rigel::Preferences::UserPreferences requested;
    requested.display.fpsLimit = 50;
    auto preferences = fixture.owner(requested);
    preferences.initializeDisplay(fixture.runtime, false);

    preferences.waitForNextFrame();

    CHECK_EQ(fixture.display.sleepDeadlines.size(), static_cast<size_t>(1));
    CHECK_NEAR(fixture.display.sleepDeadlines.back(), 0.02, 0.000001);

    auto candidate = requested.display;
    candidate.fpsLimit = 100;
    CHECK_EQ(
        preferences.applyDisplay(fixture.runtime, candidate).status,
        Rigel::PreferenceApplyStatus::Applied);
    g_now = 1.0;
    preferences.waitForNextFrame();
    CHECK_EQ(fixture.display.sleepDeadlines.size(), static_cast<size_t>(2));
    CHECK_NEAR(fixture.display.sleepDeadlines.back(), 1.01, 0.000001);
}

TEST_CASE(ApplicationPreferences_TimeResetStartsANewPacingSchedule) {
    DisplayFixture fixture;
    Rigel::Preferences::UserPreferences requested;
    requested.display.fpsLimit = 100;
    auto preferences = fixture.owner(requested);
    preferences.initializeDisplay(fixture.runtime, false);
    preferences.waitForNextFrame();

    g_now = 5.0;
    preferences.resetFramePacingSchedule();
    preferences.waitForNextFrame();

    CHECK_EQ(fixture.display.sleepDeadlines.size(), static_cast<size_t>(2));
    CHECK_NEAR(fixture.display.sleepDeadlines.back(), 5.01, 0.000001);
}

TEST_CASE(ApplicationPreferences_BorderlessRoundTripRemembersWindowedSize) {
    DisplayFixture fixture;
    Rigel::Preferences::UserPreferences requested;
    requested.display.windowedSize = {1000, 700};
    auto preferences = fixture.owner(requested);
    preferences.initializeDisplay(fixture.runtime, false);
    fixture.connectResizeCallback(preferences);

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

    g_now = 10.0;
    fixture.emitResizeCallback(400, 300);
    CHECK(!fixture.finishEventBatch(10.0));
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

TEST_CASE(ApplicationPreferences_UnsupportedVSyncChangeIsNotPublished) {
    DisplayFixture fixture;
    Rigel::Preferences::UserPreferences requested;
    auto preferences = fixture.owner(requested);
    preferences.initializeDisplay(fixture.runtime, false);
    const std::string before = readDocument(fixture.path);
    auto candidate = requested.display;
    candidate.vsync = false;
    candidate.fpsLimit = 144;
    fixture.display.swapIntervalZeroSupported = false;
    const size_t callsBefore = fixture.display.swapIntervals.size();

    const auto rejected =
        preferences.applyDisplay(fixture.runtime, candidate);

    CHECK_EQ(rejected.status, Rigel::PreferenceApplyStatus::Rejected);
    CHECK_NE(rejected.message.find("cannot disable"), std::string::npos);
    CHECK_EQ(fixture.display.swapIntervals.size(), callsBefore);
    CHECK_EQ(preferences.requested(), requested);
    CHECK_EQ(preferences.effectiveDisplay(), requested.display);
    CHECK_EQ(readDocument(fixture.path), before);
}

TEST_CASE(ApplicationPreferences_GeometryQueryFailurePrecedesMutationAndSave) {
    DisplayFixture fixture;
    Rigel::Preferences::UserPreferences requested;
    auto preferences = fixture.owner(requested);
    preferences.initializeDisplay(fixture.runtime, false);
    const std::string before = readDocument(fixture.path);
    Rigel::Preferences::detail::
        setUserPreferencesAfterSavePreflightHookForTesting(&countPreflight);
    auto candidate = requested.display;
    candidate.mode = Rigel::Preferences::DisplayMode::Borderless;
    fixture.display.failMonitorPositionQuery = true;

    const auto rejected =
        preferences.applyDisplay(fixture.runtime, candidate);

    CHECK_EQ(rejected.status, Rigel::PreferenceApplyStatus::Rejected);
    CHECK_NE(
        rejected.message.find("monitor position query"), std::string::npos);
    CHECK_EQ(fixture.display.configurationAttempts, 0);
    CHECK_EQ(fixture.display.savePreflights, 0);
    CHECK_EQ(preferences.requested(), requested);
    CHECK_EQ(preferences.effectiveDisplay(), requested.display);
    CHECK_EQ(readDocument(fixture.path), before);
}

TEST_CASE(ApplicationPreferences_FpsOnlyChangeDoesNotQueryWindowGeometry) {
    DisplayFixture fixture;
    Rigel::Preferences::UserPreferences requested;
    auto preferences = fixture.owner(requested);
    preferences.initializeDisplay(fixture.runtime, false);
    auto candidate = requested.display;
    candidate.fpsLimit = 100;
    fixture.display.failWindowPositionQuery = true;

    const auto applied =
        preferences.applyDisplay(fixture.runtime, candidate);

    CHECK_EQ(applied.status, Rigel::PreferenceApplyStatus::Applied);
    CHECK_EQ(preferences.requested().display, candidate);
    CHECK_EQ(preferences.effectiveDisplay(), candidate);
    preferences.waitForNextFrame();
    CHECK_EQ(fixture.display.sleepDeadlines.size(), static_cast<size_t>(1));
    CHECK_NEAR(fixture.display.sleepDeadlines.back(), 0.01, 0.000001);
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
        setUserPreferencesBeforePublicationHookForTesting(
            &failBeforePublication);

    const auto notPublished =
        preferences.applyDisplay(fixture.runtime, candidate);

    CHECK_EQ(
        notPublished.status, Rigel::PreferenceApplyStatus::NotPublished);
    CHECK_EQ(fixture.display.bounds, oldBounds);
    CHECK_EQ(preferences.requested(), requested);
    CHECK_EQ(preferences.effectiveDisplay(), requested.display);

    Rigel::Preferences::detail::
        setUserPreferencesBeforePublicationHookForTesting({});
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

TEST_CASE(ApplicationPreferences_OversizedRetainedSavePreventsDisplayMutation) {
    DisplayFixture fixture;
    const std::string original = maximumRetainedDocument();
    writeDocument(fixture.path, original);
    Rigel::ApplicationPreferences preferences(
        fixture.path,
        Rigel::Core::FramePacer::Clock{&now, &sleepUntil});
    preferences.load();
    preferences.initializeDisplay(fixture.runtime, false);
    const auto oldBounds = fixture.display.bounds;
    const int oldDecoration = fixture.display.decorated;
    const size_t oldSwapAttempts = fixture.display.swapIntervals.size();
    const int oldConfigurationAttempts =
        fixture.display.configurationAttempts;
    auto candidate = preferences.requested().display;
    candidate.windowedSize = {1280, 720};
    candidate.vsync = false;

    const auto result = preferences.applyDisplay(fixture.runtime, candidate);

    CHECK_EQ(result.status, Rigel::PreferenceApplyStatus::PersistenceBlocked);
    CHECK_NE(result.message.find("262144-byte limit"), std::string::npos);
    CHECK_EQ(fixture.display.bounds, oldBounds);
    CHECK_EQ(fixture.display.decorated, oldDecoration);
    CHECK_EQ(fixture.display.swapIntervals.size(), oldSwapAttempts);
    CHECK_EQ(
        fixture.display.configurationAttempts, oldConfigurationAttempts);
    CHECK_EQ(preferences.requested().display,
             Rigel::Preferences::DisplayPreferences{});
    CHECK_EQ(preferences.effectiveDisplay(),
             Rigel::Preferences::DisplayPreferences{});
    CHECK_EQ(readDocument(fixture.path), original);
}

TEST_CASE(ApplicationPreferences_OversizedRetainedSavePreservesFovHistory) {
    DisplayFixture fixture;
    const std::string original = maximumRetainedDocument();
    writeDocument(fixture.path, original);
    Rigel::ApplicationPreferences preferences(
        fixture.path,
        Rigel::Core::FramePacer::Clock{&now, &sleepUntil});
    preferences.load();
    Rigel::Render::FrameRenderer renderer;
    renderer.setVerticalFovDegrees(
        preferences.effectiveVerticalFovDegrees());
    Rigel::Render::FrameRendererTestAccess::markTemporalHistoryValid(renderer);

    const auto result = preferences.applyVerticalFov(renderer, 90.0);

    CHECK_EQ(result.status, Rigel::PreferenceApplyStatus::PersistenceBlocked);
    CHECK_EQ(preferences.requested().camera.verticalFovDegrees, 60.0);
    CHECK_EQ(preferences.effectiveVerticalFovDegrees(), 60.0);
    CHECK_EQ(
        Rigel::Render::FrameRendererTestAccess::verticalFovDegrees(renderer),
        60.0);
    CHECK(Rigel::Render::FrameRendererTestAccess::temporalHistoryValid(
        renderer));
    CHECK_EQ(readDocument(fixture.path), original);
}

TEST_CASE(ApplicationPreferences_DeferredAppliedResizeCallbackIsDiscarded) {
    DisplayFixture fixture;
    Rigel::Preferences::UserPreferences requested;
    auto preferences = fixture.owner(requested);
    preferences.initializeDisplay(fixture.runtime, false);
    fixture.connectResizeCallback(preferences);
    fixture.display.deferConfigurationResizeCallbacks = true;
    auto candidate = requested.display;
    candidate.windowedSize = {1280, 720};

    CHECK_EQ(
        preferences.applyDisplay(fixture.runtime, candidate).status,
        Rigel::PreferenceApplyStatus::Applied);
    CHECK_EQ(fixture.display.deferredLogicalResizes.size(),
             static_cast<size_t>(1));
    Rigel::Preferences::detail::
        setUserPreferencesAfterSavePreflightHookForTesting(&countPreflight);
    fixture.dispatchDeferredResizeCallbacks();

    CHECK(!fixture.finishEventBatch(1.0));
    CHECK(!preferences.flushResizePersistence(10.0));
    CHECK_EQ(fixture.display.savePreflights, 0);
    CHECK_EQ(preferences.requested().display, candidate);
    CHECK_EQ(preferences.effectiveDisplay(), candidate);
}

TEST_CASE(ApplicationPreferences_DeferredRollbackCallbacksAreDiscarded) {
    DisplayFixture fixture;
    Rigel::Preferences::UserPreferences requested;
    auto preferences = fixture.owner(requested);
    preferences.initializeDisplay(fixture.runtime, false);
    fixture.connectResizeCallback(preferences);
    fixture.display.deferConfigurationResizeCallbacks = true;
    auto candidate = requested.display;
    candidate.windowedSize = {1280, 720};
    Rigel::Preferences::detail::
        setUserPreferencesBeforePublicationHookForTesting(
            &failBeforePublication);

    CHECK_EQ(
        preferences.applyDisplay(fixture.runtime, candidate).status,
        Rigel::PreferenceApplyStatus::NotPublished);
    CHECK_EQ(fixture.display.deferredLogicalResizes.size(),
             static_cast<size_t>(2));
    Rigel::Preferences::detail::
        setUserPreferencesBeforePublicationHookForTesting({});
    Rigel::Preferences::detail::
        setUserPreferencesAfterSavePreflightHookForTesting(&countPreflight);
    fixture.dispatchDeferredResizeCallbacks();

    CHECK(!fixture.finishEventBatch(1.0));
    CHECK(!preferences.flushResizePersistence(10.0));
    CHECK_EQ(fixture.display.savePreflights, 0);
    CHECK_EQ(preferences.requested(), requested);
    CHECK_EQ(preferences.effectiveDisplay(), requested.display);
}

TEST_CASE(ApplicationPreferences_SameBatchManualResizeSupersedesDeferredApply) {
    DisplayFixture fixture;
    Rigel::Preferences::UserPreferences requested;
    auto preferences = fixture.owner(requested);
    preferences.initializeDisplay(fixture.runtime, false);
    fixture.connectResizeCallback(preferences);
    fixture.display.deferConfigurationResizeCallbacks = true;
    auto candidate = requested.display;
    candidate.windowedSize = {1280, 720};
    CHECK_EQ(
        preferences.applyDisplay(fixture.runtime, candidate).status,
        Rigel::PreferenceApplyStatus::Applied);

    fixture.dispatchDeferredResizeCallbacks();
    fixture.display.bounds.width = 1400;
    fixture.display.bounds.height = 900;
    fixture.emitResizeCallback(1400, 900);
    CHECK(!fixture.finishEventBatch(1.0));
    CHECK_EQ(preferences.effectiveDisplay().windowedSize,
             (Rigel::Preferences::WindowedSize{1400, 900}));
    CHECK(!preferences.flushResizePersistence(1.24));

    const auto saved = preferences.flushResizePersistence(1.25);
    CHECK(saved.has_value());
    CHECK_EQ(saved->status, Rigel::PreferenceApplyStatus::Applied);
    CHECK_EQ(preferences.requested().display.windowedSize,
             (Rigel::Preferences::WindowedSize{1400, 900}));
}

TEST_CASE(ApplicationPreferences_ResizeQueryFailureIsConsumedOnce) {
    DisplayFixture fixture;
    Rigel::Preferences::UserPreferences requested;
    auto preferences = fixture.owner(requested);
    preferences.initializeDisplay(fixture.runtime, false);
    fixture.connectResizeCallback(preferences);
    fixture.display.bounds.width = 1200;
    fixture.display.bounds.height = 800;
    fixture.display.failWindowPositionQuery = true;
    fixture.emitResizeCallback(1200, 800);

    const auto failed = fixture.finishEventBatch(1.0);

    CHECK(failed.has_value());
    CHECK_EQ(failed->status, Rigel::PreferenceApplyStatus::Rejected);
    CHECK_NE(failed->message.find("window position query"),
             std::string::npos);
    fixture.display.failWindowPositionQuery = true;
    CHECK(!fixture.finishEventBatch(2.0));
    CHECK(fixture.display.failWindowPositionQuery);
    CHECK(!preferences.flushResizePersistence(10.0));
    CHECK_EQ(preferences.effectiveDisplay().windowedSize,
             requested.display.windowedSize);
}

TEST_CASE(ApplicationPreferences_ResizeDebouncesAndCoalescesOneSave) {
    DisplayFixture fixture;
    Rigel::Preferences::UserPreferences requested;
    auto preferences = fixture.owner(requested);
    preferences.initializeDisplay(fixture.runtime, false);
    fixture.connectResizeCallback(preferences);
    Rigel::Preferences::detail::
        setUserPreferencesAfterSavePreflightHookForTesting(&countPreflight);

    fixture.manualResize(900, 700, 1.0);
    fixture.manualResize(1100, 750, 1.1);
    CHECK(!preferences.flushResizePersistence(1.34));
    const auto saved = preferences.flushResizePersistence(1.36);

    CHECK(saved.has_value());
    CHECK_EQ(saved->status, Rigel::PreferenceApplyStatus::Applied);
    CHECK_EQ(fixture.display.savePreflights, 1);
    CHECK_EQ(preferences.requested().display.windowedSize,
             (Rigel::Preferences::WindowedSize{1100, 750}));
    CHECK(!preferences.flushResizePersistence(5.0));
}

TEST_CASE(ApplicationPreferences_ResizeFailureRetainsBoundedRetry) {
    DisplayFixture fixture;
    Rigel::Preferences::UserPreferences requested;
    auto preferences = fixture.owner(requested);
    preferences.initializeDisplay(fixture.runtime, false);
    fixture.connectResizeCallback(preferences);
    fixture.manualResize(1200, 800, 1.0);
    Rigel::Preferences::detail::
        setUserPreferencesBeforePublicationHookForTesting(
            &failBeforePublication);

    const auto result = preferences.flushResizePersistence(1.25);

    CHECK(result.has_value());
    CHECK_EQ(result->status, Rigel::PreferenceApplyStatus::NotPublished);
    CHECK_EQ(preferences.requested().display.windowedSize,
             requested.display.windowedSize);
    CHECK_EQ(preferences.effectiveDisplay().windowedSize,
             (Rigel::Preferences::WindowedSize{1200, 800}));
    CHECK(!preferences.flushResizePersistence(1.26));
    CHECK(!preferences.flushResizePersistence(2.24));

    Rigel::Preferences::detail::
        setUserPreferencesBeforePublicationHookForTesting({});
    Rigel::Preferences::detail::
        setUserPreferencesAfterSavePreflightHookForTesting(&countPreflight);
    const auto retried = preferences.flushResizePersistence(2.25);
    CHECK(retried.has_value());
    CHECK_EQ(retried->status, Rigel::PreferenceApplyStatus::Applied);
    CHECK_EQ(fixture.display.savePreflights, 1);
    CHECK_EQ(preferences.requested().display.windowedSize,
             (Rigel::Preferences::WindowedSize{1200, 800}));
    CHECK_EQ(
        Rigel::Preferences::UserPreferencesStore(fixture.path)
            .load()
            .display.windowedSize,
        (Rigel::Preferences::WindowedSize{1200, 800}));
}

TEST_CASE(ApplicationPreferences_ResizePublicationRetriesExactlyOnce) {
    DisplayFixture fixture;
    Rigel::Preferences::UserPreferences requested;
    auto preferences = fixture.owner(requested);
    preferences.initializeDisplay(fixture.runtime, false);
    fixture.connectResizeCallback(preferences);
    fixture.manualResize(1200, 800, 1.0);
    Rigel::Preferences::detail::
        setUserPreferencesBeforePublicationHookForTesting(
            &countAndFailBeforePublication);

    const auto first = preferences.flushResizePersistence(1.25);
    CHECK(first.has_value());
    CHECK_EQ(first->status, Rigel::PreferenceApplyStatus::NotPublished);
    CHECK_EQ(fixture.display.savePreflights, 1);

    fixture.emitResizeCallback(1200, 800);
    CHECK(!fixture.finishEventBatch(1.5));
    CHECK(!preferences.flushResizePersistence(2.24));
    const auto retry = preferences.flushResizePersistence(2.25);
    CHECK(retry.has_value());
    CHECK_EQ(retry->status, Rigel::PreferenceApplyStatus::NotPublished);
    CHECK_EQ(fixture.display.savePreflights, 2);

    fixture.emitResizeCallback(1200, 800);
    CHECK(!fixture.finishEventBatch(3.0));
    CHECK(!preferences.flushResizePersistence(100.0));
    const auto shutdown = preferences.flushResizePersistenceForShutdown();
    CHECK(shutdown.has_value());
    CHECK_EQ(shutdown->status, Rigel::PreferenceApplyStatus::NotPublished);
    CHECK_EQ(fixture.display.savePreflights, 2);

    Rigel::Preferences::detail::
        setUserPreferencesBeforePublicationHookForTesting({});
    fixture.manualResize(1300, 850, 101.0);
    const auto distinct = preferences.flushResizePersistence(101.25);
    CHECK(distinct.has_value());
    CHECK_EQ(distinct->status, Rigel::PreferenceApplyStatus::Applied);
    CHECK_EQ(preferences.requested().display.windowedSize,
             (Rigel::Preferences::WindowedSize{1300, 850}));
}

TEST_CASE(ApplicationPreferences_ShutdownConsumesOneResizeRetry) {
    DisplayFixture fixture;
    Rigel::Preferences::UserPreferences requested;
    auto preferences = fixture.owner(requested);
    preferences.initializeDisplay(fixture.runtime, false);
    fixture.connectResizeCallback(preferences);
    fixture.manualResize(1200, 800, 1.0);
    Rigel::Preferences::detail::
        setUserPreferencesBeforePublicationHookForTesting(
            &countAndFailBeforePublication);

    const auto failed = preferences.flushResizePersistenceForShutdown();

    CHECK(failed.has_value());
    CHECK_EQ(failed->status, Rigel::PreferenceApplyStatus::NotPublished);
    CHECK_EQ(fixture.display.savePreflights, 2);
    const auto terminal = preferences.flushResizePersistenceForShutdown();
    CHECK(terminal.has_value());
    CHECK_EQ(terminal->status, Rigel::PreferenceApplyStatus::NotPublished);
    CHECK_EQ(fixture.display.savePreflights, 2);
    CHECK_EQ(preferences.requested().display.windowedSize,
             requested.display.windowedSize);
}

TEST_CASE(ApplicationPreferences_ResizePreparationFailureIsTerminalForSize) {
    DisplayFixture fixture;
    const std::string original = maximumRetainedDocument();
    writeDocument(fixture.path, original);
    Rigel::ApplicationPreferences preferences(
        fixture.path,
        Rigel::Core::FramePacer::Clock{&now, &sleepUntil});
    preferences.load();
    preferences.initializeDisplay(fixture.runtime, false);
    fixture.connectResizeCallback(preferences);
    Rigel::Preferences::detail::
        setUserPreferencesAfterSavePreflightHookForTesting(&countPreflight);
    fixture.manualResize(1200, 800, 1.0);

    const auto failed = preferences.flushResizePersistence(1.25);

    CHECK(failed.has_value());
    CHECK_EQ(
        failed->status, Rigel::PreferenceApplyStatus::PersistenceBlocked);
    CHECK_EQ(fixture.display.savePreflights, 0);
    fixture.emitResizeCallback(1200, 800);
    CHECK(!fixture.finishEventBatch(2.0));
    CHECK(!preferences.flushResizePersistence(100.0));
    const auto shutdown = preferences.flushResizePersistenceForShutdown();
    CHECK(shutdown.has_value());
    CHECK_EQ(
        shutdown->status, Rigel::PreferenceApplyStatus::PersistenceBlocked);
    CHECK_EQ(fixture.display.savePreflights, 0);
    CHECK_EQ(readDocument(fixture.path), original);
}

TEST_CASE(ApplicationPreferences_BlockedResizeIsReportedWithoutRetrying) {
    DisplayFixture fixture;
    const std::string newer =
        "schema_version: 2\nfuture: preserve-until-explicit-replacement\n";
    writeDocument(fixture.path, newer);
    Rigel::ApplicationPreferences preferences(
        fixture.path,
        Rigel::Core::FramePacer::Clock{&now, &sleepUntil});
    preferences.load();
    preferences.initializeDisplay(fixture.runtime, false);
    fixture.connectResizeCallback(preferences);
    fixture.manualResize(1200, 800, 1.0);
    Rigel::Preferences::detail::
        setUserPreferencesAfterSavePreflightHookForTesting(&countPreflight);

    const auto blocked = preferences.flushResizePersistence(1.25);

    CHECK(blocked.has_value());
    CHECK_EQ(
        blocked->status,
        Rigel::PreferenceApplyStatus::PersistenceBlocked);
    CHECK_EQ(fixture.display.savePreflights, 0);
    CHECK_EQ(readDocument(fixture.path), newer);
    CHECK_EQ(preferences.requested().display.windowedSize,
             (Rigel::Preferences::WindowedSize{800, 600}));
    CHECK_EQ(preferences.effectiveDisplay().windowedSize,
             (Rigel::Preferences::WindowedSize{1200, 800}));
    CHECK(!preferences.flushResizePersistence(2.25));
    CHECK(!preferences.flushResizePersistence(100.0));
    CHECK_EQ(fixture.display.savePreflights, 0);

    const auto finalReport =
        preferences.flushResizePersistenceForShutdown();
    CHECK(finalReport.has_value());
    CHECK_EQ(
        finalReport->status,
        Rigel::PreferenceApplyStatus::PersistenceBlocked);
    CHECK_EQ(fixture.display.savePreflights, 0);
    CHECK_EQ(readDocument(fixture.path), newer);
}

TEST_CASE(ApplicationPreferences_ExplicitApplySupersedesPendingManualResize) {
    DisplayFixture fixture;
    Rigel::Preferences::UserPreferences requested;
    auto preferences = fixture.owner(requested);
    preferences.initializeDisplay(fixture.runtime, false);
    fixture.connectResizeCallback(preferences);

    fixture.manualResize(1100, 750, 1.0);
    auto candidate = requested.display;
    candidate.windowedSize = {1280, 720};

    const auto applied =
        preferences.applyDisplay(
            fixture.runtime, candidate, Rigel::WindowedSizeIntent::Changed);

    CHECK_EQ(applied.status, Rigel::PreferenceApplyStatus::Applied);
    CHECK_EQ(fixture.display.bounds.width, 1280);
    CHECK_EQ(fixture.display.bounds.height, 720);
    CHECK_EQ(preferences.effectiveDisplay(), candidate);
    CHECK_EQ(preferences.requested().display, candidate);
    CHECK_EQ(
        Rigel::Preferences::UserPreferencesStore(fixture.path).load().display,
        candidate);
    CHECK(!preferences.flushResizePersistence(10.0));
    CHECK_EQ(preferences.effectiveDisplay(), candidate);
    CHECK_EQ(preferences.requested().display, candidate);
}

TEST_CASE(ApplicationPreferences_DisplayEditRetainsPendingManualResize) {
    DisplayFixture fixture;
    Rigel::Preferences::UserPreferences requested;
    auto preferences = fixture.owner(requested);
    preferences.initializeDisplay(fixture.runtime, false);
    fixture.connectResizeCallback(preferences);
    fixture.manualResize(1100, 750, 1.0);
    auto candidate = preferences.requested().display;
    candidate.vsync = false;

    const auto applied =
        preferences.applyDisplay(
            fixture.runtime, candidate, Rigel::WindowedSizeIntent::Unchanged);

    CHECK_EQ(applied.status, Rigel::PreferenceApplyStatus::Applied);
    CHECK_EQ(fixture.display.bounds.width, 1100);
    CHECK_EQ(fixture.display.bounds.height, 750);
    candidate.windowedSize = {1100, 750};
    CHECK_EQ(preferences.effectiveDisplay(), candidate);
    CHECK_EQ(preferences.requested().display, candidate);
    CHECK_EQ(
        Rigel::Preferences::UserPreferencesStore(fixture.path).load().display,
        candidate);
    CHECK(!preferences.flushResizePersistence(10.0));
}

TEST_CASE(ApplicationPreferences_ExplicitApplyCanRestoreSavedWindowedSize) {
    DisplayFixture fixture;
    Rigel::Preferences::UserPreferences requested;
    auto preferences = fixture.owner(requested);
    preferences.initializeDisplay(fixture.runtime, false);
    fixture.connectResizeCallback(preferences);
    fixture.manualResize(1100, 750, 1.0);
    const auto candidate = preferences.requested().display;

    const auto applied =
        preferences.applyDisplay(
            fixture.runtime, candidate, Rigel::WindowedSizeIntent::Changed);

    CHECK_EQ(applied.status, Rigel::PreferenceApplyStatus::Applied);
    CHECK_EQ(fixture.display.bounds.width, 800);
    CHECK_EQ(fixture.display.bounds.height, 600);
    CHECK_EQ(preferences.effectiveDisplay(), candidate);
    CHECK_EQ(preferences.requested().display, candidate);
    CHECK_EQ(
        Rigel::Preferences::UserPreferencesStore(fixture.path).load().display,
        candidate);
    CHECK(!preferences.flushResizePersistence(10.0));
}

TEST_CASE(ApplicationPreferences_BorderlessEditRetainsPendingWindowedSize) {
    DisplayFixture fixture;
    Rigel::Preferences::UserPreferences requested;
    auto preferences = fixture.owner(requested);
    preferences.initializeDisplay(fixture.runtime, false);
    fixture.connectResizeCallback(preferences);
    fixture.manualResize(1100, 750, 1.0);
    auto candidate = preferences.requested().display;
    candidate.mode = Rigel::Preferences::DisplayMode::Borderless;

    const auto applied =
        preferences.applyDisplay(
            fixture.runtime, candidate, Rigel::WindowedSizeIntent::Unchanged);

    CHECK_EQ(applied.status, Rigel::PreferenceApplyStatus::Applied);
    CHECK_EQ(fixture.display.bounds,
             (Rigel::GlfwRuntime::Rectangle{1920, 0, 2560, 1440}));
    candidate.windowedSize = {1100, 750};
    CHECK_EQ(preferences.effectiveDisplay(), candidate);
    CHECK_EQ(preferences.requested().display, candidate);
    CHECK_EQ(
        Rigel::Preferences::UserPreferencesStore(fixture.path).load().display,
        candidate);
    CHECK(!preferences.flushResizePersistence(10.0));
}

TEST_CASE(ApplicationPreferences_ShutdownFlushesInsideResizeDebounceWindow) {
    DisplayFixture fixture;
    Rigel::Preferences::UserPreferences requested;
    auto preferences = fixture.owner(requested);
    preferences.initializeDisplay(fixture.runtime, false);
    fixture.connectResizeCallback(preferences);
    fixture.manualResize(1024, 768, 1.0);

    const auto flushed = preferences.flushResizePersistenceForShutdown();

    CHECK(flushed.has_value());
    CHECK_EQ(flushed->status, Rigel::PreferenceApplyStatus::Applied);
    CHECK_EQ(preferences.requested().display.windowedSize,
             (Rigel::Preferences::WindowedSize{1024, 768}));
}

TEST_CASE(ApplicationPreferences_FovPublicationFailureRestoresProjection) {
    DisplayFixture fixture;
    Rigel::Preferences::UserPreferences requested;
    auto preferences = fixture.owner(requested);
    Rigel::Render::FrameRenderer renderer;
    renderer.setVerticalFovDegrees(requested.camera.verticalFovDegrees);
    Rigel::Preferences::detail::
        setUserPreferencesBeforePublicationHookForTesting(
            &failBeforePublication);

    const auto result = preferences.applyVerticalFov(renderer, 90.0);

    CHECK_EQ(result.status, Rigel::PreferenceApplyStatus::NotPublished);
    CHECK_EQ(preferences.requested(), requested);
    CHECK_EQ(preferences.effectiveDisplay(), requested.display);
    CHECK_EQ(preferences.effectiveVerticalFovDegrees(),
             requested.camera.verticalFovDegrees);
    CHECK_EQ(
        Rigel::Render::FrameRendererTestAccess::verticalFovDegrees(renderer),
        requested.camera.verticalFovDegrees);
    CHECK(!Rigel::Render::FrameRendererTestAccess::temporalHistoryValid(
        renderer));
}

TEST_CASE(ApplicationPreferences_StartupInputUsesLoadedGlobalRequest) {
    DisplayFixture fixture;
    Rigel::Preferences::UserPreferences requested;
    requested.input.mouseSensitivity = 0.35;
    requested.input.invertY = true;
    requested.input.bindings[
        Rigel::Preferences::UserAction::MoveForward] = {"UP"};
    auto preferences = fixture.owner(requested);
    const std::string before = readDocument(fixture.path);
    InputFixture controls;

    preferences.initializeInput(controls.input, *controls.defaults);
    controls.input.beginFrame();
    controls.input.handleKeyEvent(GLFW_KEY_UP, GLFW_PRESS);
    controls.input.beginFrame();

    CHECK_EQ(preferences.effectiveInput(), requested.input);
    CHECK(controls.input.isActionPressed("move_forward"));
    CHECK_EQ(readDocument(fixture.path), before);
}

TEST_CASE(ApplicationPreferences_InputApplyIsImmediateAndMapSwapsAtBoundary) {
    DisplayFixture fixture;
    Rigel::Preferences::UserPreferences requested;
    auto preferences = fixture.owner(requested);
    InputFixture controls;
    preferences.initializeInput(controls.input, *controls.defaults);
    controls.input.beginFrame();

    auto candidate = requested.input;
    candidate.mouseSensitivity = 0.5;
    candidate.invertY = true;
    candidate.bindings[Rigel::Preferences::UserAction::MoveForward] =
        {"UP", "MOUSE_4"};
    candidate.bindings[Rigel::Preferences::UserAction::PlaceBlock] = {};
    controls.input.handleKeyEvent(GLFW_KEY_UP, GLFW_PRESS);

    const auto result = preferences.applyInput(
        controls.input, *controls.defaults, candidate);

    CHECK_EQ(result.status, Rigel::PreferenceApplyStatus::Applied);
    CHECK_EQ(preferences.requested().input, candidate);
    CHECK_EQ(preferences.effectiveInput(), candidate);
    CHECK(!controls.input.isActionPressed("move_forward"));
    controls.input.beginFrame();
    CHECK(controls.input.isActionPressed("move_forward"));
    CHECK(!controls.input.isActionJustPressed("move_forward"));
    controls.input.handleMouseButtonEvent(
        GLFW_MOUSE_BUTTON_RIGHT, GLFW_PRESS);
    controls.input.beginFrame();
    CHECK(!controls.input.isActionPressed("place_block"));
    CHECK_EQ(
        Rigel::Preferences::UserPreferencesStore(fixture.path).load().input,
        candidate);
}

TEST_CASE(ApplicationPreferences_InvalidInputEditRetainsPriorState) {
    DisplayFixture fixture;
    Rigel::Preferences::UserPreferences requested;
    auto preferences = fixture.owner(requested);
    InputFixture controls;
    preferences.initializeInput(controls.input, *controls.defaults);
    controls.input.beginFrame();
    const std::string before = readDocument(fixture.path);

    auto candidate = requested.input;
    candidate.mouseSensitivity = 0.7;
    candidate.bindings[Rigel::Preferences::UserAction::MoveForward] = {"87"};
    const auto result = preferences.applyInput(
        controls.input, *controls.defaults, candidate);

    CHECK_EQ(result.status, Rigel::PreferenceApplyStatus::Rejected);
    CHECK_EQ(preferences.requested(), requested);
    CHECK_EQ(preferences.effectiveInput(), requested.input);
    CHECK_EQ(readDocument(fixture.path), before);
    controls.input.handleKeyEvent(GLFW_KEY_W, GLFW_PRESS);
    controls.input.beginFrame();
    CHECK(controls.input.isActionPressed("move_forward"));
}

TEST_CASE(ApplicationPreferences_InputPublicationFailureRestoresPriorState) {
    DisplayFixture fixture;
    Rigel::Preferences::UserPreferences requested;
    auto preferences = fixture.owner(requested);
    InputFixture controls;
    preferences.initializeInput(controls.input, *controls.defaults);
    controls.input.beginFrame();
    const std::string before = readDocument(fixture.path);
    auto candidate = requested.input;
    candidate.mouseSensitivity = 0.7;
    candidate.invertY = true;
    candidate.bindings[Rigel::Preferences::UserAction::MoveForward] = {"UP"};
    Rigel::Preferences::detail::
        setUserPreferencesBeforePublicationHookForTesting(
            &failBeforePublication);
    controls.input.handleKeyEvent(GLFW_KEY_W, GLFW_PRESS);

    const auto result = preferences.applyInput(
        controls.input, *controls.defaults, candidate);

    CHECK_EQ(result.status, Rigel::PreferenceApplyStatus::NotPublished);
    CHECK_EQ(preferences.requested(), requested);
    CHECK_EQ(preferences.effectiveInput(), requested.input);
    CHECK_EQ(readDocument(fixture.path), before);
    controls.input.beginFrame();
    CHECK(controls.input.isActionPressed("move_forward"));
    CHECK(controls.input.isActionJustPressed("move_forward"));
}

TEST_CASE(ApplicationPreferences_UncertainInputPublicationKeepsCompleteCandidate) {
    DisplayFixture fixture;
    Rigel::Preferences::UserPreferences requested;
    auto preferences = fixture.owner(requested);
    InputFixture controls;
    preferences.initializeInput(controls.input, *controls.defaults);
    controls.input.beginFrame();
    auto candidate = requested.input;
    candidate.mouseSensitivity = 0.8;
    candidate.invertY = true;
    candidate.bindings[Rigel::Preferences::UserAction::MoveForward] = {"UP"};
    Rigel::Preferences::detail::
        setUserPreferencesAfterPublicationHookForTesting(
            &failAfterPublication);

    const auto result = preferences.applyInput(
        controls.input, *controls.defaults, candidate);

    CHECK_EQ(
        result.status,
        Rigel::PreferenceApplyStatus::PublishedDurabilityUncertain);
    CHECK_EQ(preferences.requested().input, candidate);
    CHECK_EQ(preferences.effectiveInput(), candidate);
    controls.input.beginFrame();
    controls.input.handleKeyEvent(GLFW_KEY_UP, GLFW_PRESS);
    controls.input.beginFrame();
    CHECK(controls.input.isActionPressed("move_forward"));
    CHECK_EQ(
        Rigel::Preferences::UserPreferencesStore(fixture.path).load().input,
        candidate);
}

TEST_CASE(ApplicationPreferences_ResetBindingsPreservesMouseControls) {
    DisplayFixture fixture;
    Rigel::Preferences::UserPreferences requested;
    requested.input.mouseSensitivity = 0.42;
    requested.input.invertY = true;
    requested.input.bindings[
        Rigel::Preferences::UserAction::MoveForward] = {"UP"};
    requested.input.bindings[
        Rigel::Preferences::UserAction::PlaceBlock] = {};
    auto preferences = fixture.owner(requested);
    InputFixture controls;
    preferences.initializeInput(controls.input, *controls.defaults);
    controls.input.beginFrame();

    const auto result = preferences.resetControlBindings(
        controls.input, *controls.defaults);

    CHECK_EQ(result.status, Rigel::PreferenceApplyStatus::Applied);
    CHECK_EQ(preferences.effectiveInput().mouseSensitivity, 0.42);
    CHECK(preferences.effectiveInput().invertY);
    CHECK(preferences.effectiveInput().bindings.empty());
    CHECK(preferences.requested().input.bindings.empty());
    controls.input.beginFrame();
    controls.input.handleKeyEvent(GLFW_KEY_W, GLFW_PRESS);
    controls.input.handleMouseButtonEvent(
        GLFW_MOUSE_BUTTON_RIGHT, GLFW_PRESS);
    controls.input.beginFrame();
    CHECK(controls.input.isActionPressed("move_forward"));
    CHECK(controls.input.isActionJustPressed("place_block"));
    CHECK_EQ(
        Rigel::Preferences::UserPreferencesStore(fixture.path).load().input,
        preferences.effectiveInput());
}
