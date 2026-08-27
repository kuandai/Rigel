#include "ApplicationPreferences.h"

#include "Rigel/Persistence/Storage.h"
#include "Rigel/Render/FrameRenderer.h"
#include "Rigel/input/GameplayInput.h"

#include <cmath>
#include <exception>
#include <stdexcept>
#include <utility>

namespace Rigel {
namespace {

constexpr double kResizePersistenceDelaySeconds = 0.25;
constexpr double kResizePersistenceRetrySeconds = 1.0;
constexpr Preferences::WindowedSize kSafeWindowedSize{800, 600};

bool validWindowedSize(const Preferences::WindowedSize& size) {
    return size.width >= Preferences::kMinimumWindowDimension &&
           size.width <= Preferences::kMaximumWindowDimension &&
           size.height >= Preferences::kMinimumWindowDimension &&
           size.height <= Preferences::kMaximumWindowDimension;
}

bool validDisplayMode(Preferences::DisplayMode mode) {
    return mode == Preferences::DisplayMode::Windowed ||
           mode == Preferences::DisplayMode::Borderless;
}

bool validFpsLimit(const std::optional<int>& limit) {
    return !limit ||
           (*limit >= Preferences::kMinimumFpsLimit &&
            *limit <= Preferences::kMaximumFpsLimit);
}

PreferenceApplyResult publicationFailure(const std::exception& error) {
    const auto* publication =
        dynamic_cast<const Persistence::AtomicFilePublicationError*>(&error);
    if (publication &&
        publication->state() ==
            Persistence::AtomicFilePublicationState::
                PublishedDurabilityUncertain) {
        return {
            PreferenceApplyStatus::PublishedDurabilityUncertain,
            error.what()};
    }
    return {PreferenceApplyStatus::NotPublished, error.what()};
}

} // namespace

ApplicationPreferences::ApplicationPreferences(std::filesystem::path path)
    : m_store(std::move(path)) {
}

ApplicationPreferences::ApplicationPreferences(
    std::filesystem::path path,
    Core::FramePacer::Clock clock)
    : m_store(std::move(path))
    , m_framePacer(clock) {
}

void ApplicationPreferences::load() {
    m_requested = m_store.load();
    m_effectiveDisplay = m_requested.display;
    m_effectiveVerticalFovDegrees =
        m_requested.camera.verticalFovDegrees;
}

Preferences::DisplayPreferences ApplicationPreferences::effectiveDisplayFor(
    const Preferences::DisplayPreferences& requested) const {
    Preferences::DisplayPreferences effective = requested;
    if (m_benchmarkMode) {
        effective.vsync = false;
        effective.fpsLimit.reset();
    }
    return effective;
}

bool ApplicationPreferences::createDisplay(
    GlfwRuntime& runtime,
    const Preferences::DisplayPreferences& display,
    std::string& failure) {
    int width = display.windowedSize.width;
    int height = display.windowedSize.height;
    bool decorated = true;
    std::optional<std::pair<int, int>> position;
    std::optional<GlfwRuntime::Rectangle> desktop;
    if (display.mode == Preferences::DisplayMode::Borderless) {
        desktop = runtime.currentDesktopBounds();
        if (!desktop) {
            failure = runtime.lastError();
            return false;
        }
        width = desktop->width;
        height = desktop->height;
        decorated = false;
        position = std::pair{desktop->x, desktop->y};
    }

    if (!runtime.createWindow(width, height, "Rigel", decorated, position)) {
        failure = runtime.lastError().empty()
            ? "window creation returned no window"
            : runtime.lastError();
        return false;
    }

    const GlfwRuntime::Rectangle actual = runtime.windowBounds();
    const bool correctSize = actual.width == width && actual.height == height;
    const bool correctPosition = !desktop ||
        (actual.x == desktop->x && actual.y == desktop->y);
    if (!correctSize || !correctPosition ||
        runtime.windowDecorated() != decorated) {
        failure = "window manager did not create the requested display state";
        runtime.destroyWindow();
        return false;
    }
    if (!runtime.makeContextCurrent()) {
        failure = runtime.lastError();
        runtime.destroyWindow();
        return false;
    }
    if (!runtime.setSwapInterval(display.vsync ? 1 : 0)) {
        failure = runtime.lastError();
        runtime.destroyWindow();
        return false;
    }
    if (display.mode == Preferences::DisplayMode::Windowed) {
        m_windowedPosition = std::pair{actual.x, actual.y};
    }
    return true;
}

ApplicationPreferences::StartupResult
ApplicationPreferences::initializeDisplay(
    GlfwRuntime& runtime,
    bool benchmarkMode) {
    m_benchmarkMode = benchmarkMode;
    m_effectiveDisplay = effectiveDisplayFor(m_requested.display);

    std::string requestedFailure;
    if (createDisplay(runtime, m_effectiveDisplay, requestedFailure)) {
        m_framePacer.setLimit(m_effectiveDisplay.fpsLimit);
        return {};
    }

    runtime.destroyWindow();
    Preferences::DisplayPreferences safe;
    safe.mode = Preferences::DisplayMode::Windowed;
    safe.windowedSize = kSafeWindowedSize;
    safe.vsync = false;
    safe.fpsLimit.reset();
    std::string fallbackFailure;
    if (!createDisplay(runtime, safe, fallbackFailure)) {
        throw std::runtime_error(
            "Requested display failed (" + requestedFailure +
            "); fixed safe windowed fallback also failed (" +
            fallbackFailure + ")");
    }

    m_effectiveDisplay = safe;
    m_framePacer.setLimit(safe.fpsLimit);
    return {
        true,
        "Requested display failed (" + requestedFailure +
            "); using fixed safe 800x600 windowed display with VSync off"};
}

bool ApplicationPreferences::restorePhysicalDisplay(
    GlfwRuntime& runtime,
    const GlfwRuntime::Rectangle& bounds,
    bool decorated,
    bool vsync,
    bool restoreWindow,
    bool restoreSwapInterval,
    std::string& failure) {
    bool windowRestored = true;
    std::string windowFailure;
    if (restoreWindow) {
        m_programmaticWindowChange = true;
        windowRestored = runtime.applyWindowConfiguration(bounds, decorated);
        m_programmaticWindowChange = false;
        if (!windowRestored) {
            windowFailure = runtime.lastError();
        }
    }
    bool swapRestored = true;
    std::string swapFailure;
    if (restoreSwapInterval) {
        swapRestored = runtime.setSwapInterval(vsync ? 1 : 0);
        if (!swapRestored) {
            swapFailure = runtime.lastError();
        }
    }
    if (windowRestored && swapRestored) {
        return true;
    }
    failure = "rollback failed";
    if (!windowFailure.empty()) {
        failure += ": " + windowFailure;
    }
    if (!swapFailure.empty()) {
        failure += windowFailure.empty() ? ": " : "; ";
        failure += swapFailure;
    }
    return false;
}

PreferenceApplyResult ApplicationPreferences::applyDisplay(
    GlfwRuntime& runtime,
    const Preferences::DisplayPreferences& candidate) {
    if (!validDisplayMode(candidate.mode) ||
        !validWindowedSize(candidate.windowedSize) ||
        !validFpsLimit(candidate.fpsLimit)) {
        return {
            PreferenceApplyStatus::Rejected,
            "display request is outside the supported window or FPS range"};
    }
    const Preferences::DisplayPreferences previousEffective =
        m_effectiveDisplay;
    const GlfwRuntime::Rectangle previousBounds = runtime.windowBounds();
    const bool previousDecorated = runtime.windowDecorated();
    const Preferences::DisplayPreferences nextEffective =
        effectiveDisplayFor(candidate);
    if (candidate == m_requested.display &&
        nextEffective == previousEffective) {
        m_pendingResize.reset();
        return {};
    }
    const auto previousWindowedPosition = m_windowedPosition;

    bool physicalChange =
        nextEffective.mode != previousEffective.mode ||
        (nextEffective.mode == Preferences::DisplayMode::Windowed &&
         nextEffective.windowedSize != previousEffective.windowedSize);
    if (physicalChange) {
        GlfwRuntime::Rectangle nextBounds = previousBounds;
        if (nextEffective.mode == Preferences::DisplayMode::Borderless) {
            if (previousEffective.mode == Preferences::DisplayMode::Windowed) {
                m_windowedPosition =
                    std::pair{previousBounds.x, previousBounds.y};
            }
            const auto desktop = runtime.currentDesktopBounds();
            if (!desktop) {
                m_windowedPosition = previousWindowedPosition;
                return {PreferenceApplyStatus::Rejected, runtime.lastError()};
            }
            nextBounds = *desktop;
        } else {
            nextBounds.width = nextEffective.windowedSize.width;
            nextBounds.height = nextEffective.windowedSize.height;
            if (m_windowedPosition) {
                nextBounds.x = m_windowedPosition->first;
                nextBounds.y = m_windowedPosition->second;
            } else if (const auto desktop = runtime.currentDesktopBounds()) {
                nextBounds.x =
                    desktop->x + (desktop->width - nextBounds.width) / 2;
                nextBounds.y =
                    desktop->y + (desktop->height - nextBounds.height) / 2;
            }
        }

        m_programmaticWindowChange = true;
        const bool applied = runtime.applyWindowConfiguration(
            nextBounds,
            nextEffective.mode == Preferences::DisplayMode::Windowed);
        m_programmaticWindowChange = false;
        if (!applied) {
            std::string failure = runtime.lastError();
            std::string rollbackFailure;
            restorePhysicalDisplay(
                runtime,
                previousBounds,
                previousDecorated,
                previousEffective.vsync,
                true,
                false,
                rollbackFailure);
            if (!rollbackFailure.empty()) {
                throw std::runtime_error(
                    failure + "; working display " + rollbackFailure);
            }
            m_windowedPosition = previousWindowedPosition;
            return {PreferenceApplyStatus::Rejected, std::move(failure)};
        }
    }

    if (nextEffective.vsync != previousEffective.vsync &&
        !runtime.setSwapInterval(nextEffective.vsync ? 1 : 0)) {
        std::string failure = runtime.lastError();
        std::string rollbackFailure;
        restorePhysicalDisplay(
            runtime,
            previousBounds,
            previousDecorated,
            previousEffective.vsync,
            physicalChange,
            true,
            rollbackFailure);
        if (!rollbackFailure.empty()) {
            throw std::runtime_error(
                failure + "; working display " + rollbackFailure);
        }
        m_windowedPosition = previousWindowedPosition;
        return {PreferenceApplyStatus::Rejected, std::move(failure)};
    }

    Preferences::UserPreferences nextRequested = m_requested;
    nextRequested.display = candidate;
    m_effectiveDisplay = nextEffective;
    m_framePacer.setLimit(nextEffective.fpsLimit);
    try {
        m_store.saveRequested(nextRequested);
        m_requested = std::move(nextRequested);
        m_pendingResize.reset();
        return {};
    } catch (const std::exception& error) {
        PreferenceApplyResult result = publicationFailure(error);
        if (result.status ==
            PreferenceApplyStatus::PublishedDurabilityUncertain) {
            m_requested = std::move(nextRequested);
            m_pendingResize.reset();
            return result;
        }

        std::string rollbackFailure;
        restorePhysicalDisplay(
            runtime,
            previousBounds,
            previousDecorated,
            previousEffective.vsync,
            physicalChange,
            nextEffective.vsync != previousEffective.vsync,
            rollbackFailure);
        m_effectiveDisplay = previousEffective;
        m_framePacer.setLimit(previousEffective.fpsLimit);
        m_windowedPosition = previousWindowedPosition;
        if (!rollbackFailure.empty()) {
            throw std::runtime_error(
                result.message + "; working display " + rollbackFailure);
        }
        return result;
    }
}

PreferenceApplyResult ApplicationPreferences::applyVerticalFov(
    Render::FrameRenderer& renderer,
    double candidateDegrees) {
    if (!std::isfinite(candidateDegrees) ||
        candidateDegrees < Preferences::kMinimumVerticalFovDegrees ||
        candidateDegrees > Preferences::kMaximumVerticalFovDegrees) {
        return {
            PreferenceApplyStatus::Rejected,
            "vertical FOV is outside the supported 50 through 110 degree range"};
    }
    if (candidateDegrees == m_requested.camera.verticalFovDegrees) {
        return {};
    }

    const double previousEffective = m_effectiveVerticalFovDegrees.value();
    Preferences::UserPreferences nextRequested = m_requested;
    nextRequested.camera.verticalFovDegrees = candidateDegrees;
    renderer.setVerticalFovDegrees(candidateDegrees);
    m_effectiveVerticalFovDegrees = candidateDegrees;
    try {
        m_store.saveRequested(nextRequested);
        m_requested = std::move(nextRequested);
        return {};
    } catch (const std::exception& error) {
        PreferenceApplyResult result = publicationFailure(error);
        if (result.status ==
            PreferenceApplyStatus::PublishedDurabilityUncertain) {
            m_requested = std::move(nextRequested);
            return result;
        }
        renderer.setVerticalFovDegrees(previousEffective);
        m_effectiveVerticalFovDegrees = previousEffective;
        return result;
    }
}

void ApplicationPreferences::observeLogicalResize(
    int width,
    int height,
    double observedAt) {
    const Preferences::WindowedSize observed{width, height};
    if (m_programmaticWindowChange ||
        m_effectiveDisplay.mode != Preferences::DisplayMode::Windowed ||
        !validWindowedSize(observed)) {
        return;
    }
    m_effectiveDisplay.windowedSize = observed;
    m_pendingResize = observed;
    m_nextResizePersistenceAttempt =
        observedAt + kResizePersistenceDelaySeconds;
}

std::optional<PreferenceApplyResult>
ApplicationPreferences::flushResizePersistence(double now) {
    return persistPendingResize(now, false);
}

std::optional<PreferenceApplyResult>
ApplicationPreferences::flushResizePersistenceForShutdown() {
    return persistPendingResize(now(), true);
}

std::optional<PreferenceApplyResult>
ApplicationPreferences::persistPendingResize(double now, bool ignoreDelay) {
    if (!m_pendingResize ||
        (!ignoreDelay && now < m_nextResizePersistenceAttempt)) {
        return std::nullopt;
    }

    const Preferences::WindowedSize observed = *m_pendingResize;
    Preferences::UserPreferences nextRequested = m_requested;
    nextRequested.display.windowedSize = observed;
    try {
        m_store.saveRequested(nextRequested);
        m_requested = std::move(nextRequested);
        m_pendingResize.reset();
        return PreferenceApplyResult{};
    } catch (const std::exception& error) {
        PreferenceApplyResult result = publicationFailure(error);
        if (result.status ==
            PreferenceApplyStatus::PublishedDurabilityUncertain) {
            m_requested = std::move(nextRequested);
            m_pendingResize.reset();
        } else {
            m_nextResizePersistenceAttempt =
                now + kResizePersistenceRetrySeconds;
        }
        return result;
    }
}

void registerApplicationPreferenceCallbacks(
    Input::InputCallbackContext& callbacks,
    ApplicationPreferences& preferences) {
    callbacks.logicalResizeContext = &preferences;
    callbacks.logicalResize = [](void* context, int width, int height) {
        auto& owner = *static_cast<ApplicationPreferences*>(context);
        owner.observeLogicalResize(width, height, owner.now());
    };
}

} // namespace Rigel
