#include "ApplicationPreferences.h"

#include "Rigel/Persistence/Storage.h"
#include "Rigel/Render/FrameRenderer.h"
#include "Rigel/input/GameplayInput.h"

#include <algorithm>
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

bool validInputPreferences(
    const Preferences::InputPreferences& preferences) {
    if (!std::isfinite(preferences.mouseSensitivity) ||
        preferences.mouseSensitivity < Preferences::kMinimumMouseSensitivity ||
        preferences.mouseSensitivity > Preferences::kMaximumMouseSensitivity) {
        return false;
    }
    for (const auto& [action, tokens] : preferences.bindings) {
        const bool knownAction = std::any_of(
            Preferences::kUserActions.begin(),
            Preferences::kUserActions.end(),
            [action](const auto& candidate) {
                return candidate.first == action;
            });
        if (!knownAction ||
            tokens.size() > Preferences::kMaximumBindingsPerAction) {
            return false;
        }
    }
    return true;
}

PreferenceApplyResult publicationFailure(const std::exception& error) {
    if (dynamic_cast<const Preferences::UserPreferencesWriteBlocked*>(
            &error) ||
        dynamic_cast<const Preferences::UserPreferencesPreparationError*>(
            &error)) {
        return {PreferenceApplyStatus::PersistenceBlocked, error.what()};
    }
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
    m_effectiveInput = m_requested.input;
    m_logicalResizeDirty = false;
    m_pendingResize.reset();
    m_resizePersistenceTerminal.reset();
    m_resizePublicationRetriesRemaining = 0;
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

    const auto actual = runtime.windowBounds();
    if (!actual) {
        failure = runtime.lastError();
        runtime.destroyWindow();
        return false;
    }
    const auto actualDecorated = runtime.windowDecorated();
    if (!actualDecorated) {
        failure = runtime.lastError();
        runtime.destroyWindow();
        return false;
    }
    const bool correctSize =
        actual->width == width && actual->height == height;
    const bool correctPosition = !desktop ||
        (actual->x == desktop->x && actual->y == desktop->y);
    if (!correctSize || !correctPosition ||
        *actualDecorated != decorated) {
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
        m_windowedPosition = std::pair{actual->x, actual->y};
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
    std::string vsyncOffFallbackFailure;
    if (!createDisplay(runtime, safe, vsyncOffFallbackFailure)) {
        if (benchmarkMode) {
            throw std::runtime_error(
                "Requested benchmark display failed (" + requestedFailure +
                "); fixed safe benchmark display also failed (" +
                vsyncOffFallbackFailure + ")");
        }
        safe.vsync = true;
        std::string vsyncOnFallbackFailure;
        if (!createDisplay(runtime, safe, vsyncOnFallbackFailure)) {
            throw std::runtime_error(
                "Requested display failed (" + requestedFailure +
                "); fixed safe windowed fallback with VSync off failed (" +
                vsyncOffFallbackFailure +
                "); fixed safe windowed fallback with VSync on also failed (" +
                vsyncOnFallbackFailure + ")");
        }
    }

    m_effectiveDisplay = safe;
    m_framePacer.setLimit(safe.fpsLimit);
    return {
        true,
        "Requested display failed (" + requestedFailure +
            "); using fixed safe 800x600 windowed display with VSync " +
            (safe.vsync ? "on" : "off")};
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
        windowRestored = runtime.applyWindowConfiguration(bounds, decorated);
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
    const Preferences::DisplayPreferences& candidate,
    WindowedSizeIntent windowedSizeIntent) {
    Preferences::DisplayPreferences requestedCandidate = candidate;
    if (windowedSizeIntent == WindowedSizeIntent::Unchanged) {
        requestedCandidate.windowedSize = m_pendingResize.value_or(
            m_requested.display.windowedSize);
    }
    if (!validDisplayMode(requestedCandidate.mode) ||
        !validWindowedSize(requestedCandidate.windowedSize) ||
        !validFpsLimit(requestedCandidate.fpsLimit)) {
        return {
            PreferenceApplyStatus::Rejected,
            "display request is outside the supported window or FPS range"};
    }
    const Preferences::DisplayPreferences previousEffective =
        m_effectiveDisplay;
    const Preferences::DisplayPreferences nextEffective =
        effectiveDisplayFor(requestedCandidate);
    if (requestedCandidate == m_requested.display &&
        nextEffective == previousEffective) {
        m_pendingResize.reset();
        m_resizePersistenceTerminal.reset();
        m_resizePublicationRetriesRemaining = 0;
        return {};
    }
    const auto previousWindowedPosition = m_windowedPosition;
    auto nextWindowedPosition = m_windowedPosition;

    const bool physicalChange =
        nextEffective.mode != previousEffective.mode ||
        (nextEffective.mode == Preferences::DisplayMode::Windowed &&
         nextEffective.windowedSize != previousEffective.windowedSize);
    GlfwRuntime::Rectangle previousBounds;
    GlfwRuntime::Rectangle nextBounds;
    bool previousDecorated = false;
    if (physicalChange) {
        const auto previousBoundsResult = runtime.windowBounds();
        if (!previousBoundsResult) {
            return {PreferenceApplyStatus::Rejected, runtime.lastError()};
        }
        const auto previousDecoratedResult = runtime.windowDecorated();
        if (!previousDecoratedResult) {
            return {PreferenceApplyStatus::Rejected, runtime.lastError()};
        }
        previousBounds = *previousBoundsResult;
        previousDecorated = *previousDecoratedResult;
        nextBounds = previousBounds;
        if (nextEffective.mode == Preferences::DisplayMode::Borderless) {
            if (previousEffective.mode == Preferences::DisplayMode::Windowed) {
                nextWindowedPosition =
                    std::pair{previousBounds.x, previousBounds.y};
            }
            const auto desktop = runtime.currentDesktopBounds();
            if (!desktop) {
                return {PreferenceApplyStatus::Rejected, runtime.lastError()};
            }
            nextBounds = *desktop;
        } else {
            nextBounds.width = nextEffective.windowedSize.width;
            nextBounds.height = nextEffective.windowedSize.height;
            if (nextWindowedPosition) {
                nextBounds.x = nextWindowedPosition->first;
                nextBounds.y = nextWindowedPosition->second;
            } else {
                const auto desktop = runtime.currentDesktopBounds();
                if (!desktop) {
                    return {
                        PreferenceApplyStatus::Rejected,
                        runtime.lastError()};
                }
                nextBounds.x =
                    desktop->x + (desktop->width - nextBounds.width) / 2;
                nextBounds.y =
                    desktop->y + (desktop->height - nextBounds.height) / 2;
            }
        }
    }

    Preferences::UserPreferences nextRequested = m_requested;
    nextRequested.display = requestedCandidate;
    std::optional<Preferences::UserPreferencesStore::PreparedSave> prepared;
    try {
        prepared.emplace(m_store.prepareSave(nextRequested));
    } catch (const std::exception& error) {
        return publicationFailure(error);
    }

    if (physicalChange) {
        const bool applied = runtime.applyWindowConfiguration(
            nextBounds,
            nextEffective.mode == Preferences::DisplayMode::Windowed);
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
        m_windowedPosition = nextWindowedPosition;
    }

    if (nextEffective.vsync != previousEffective.vsync &&
        !runtime.setSwapInterval(nextEffective.vsync ? 1 : 0)) {
        std::string failure = runtime.lastError();
        const bool swapIntervalMayHaveChanged =
            runtime.swapIntervalUpdateMayHaveMutated();
        std::string rollbackFailure;
        restorePhysicalDisplay(
            runtime,
            previousBounds,
            previousDecorated,
            previousEffective.vsync,
            physicalChange,
            swapIntervalMayHaveChanged,
            rollbackFailure);
        if (!rollbackFailure.empty()) {
            throw std::runtime_error(
                failure + "; working display " + rollbackFailure);
        }
        m_windowedPosition = previousWindowedPosition;
        return {PreferenceApplyStatus::Rejected, std::move(failure)};
    }

    m_effectiveDisplay = nextEffective;
    m_framePacer.setLimit(nextEffective.fpsLimit);
    try {
        m_store.publishPrepared(std::move(*prepared));
        m_requested = std::move(nextRequested);
        m_pendingResize.reset();
        m_resizePersistenceTerminal.reset();
        m_resizePublicationRetriesRemaining = 0;
        return {};
    } catch (const std::exception& error) {
        PreferenceApplyResult result = publicationFailure(error);
        if (result.status ==
            PreferenceApplyStatus::PublishedDurabilityUncertain) {
            m_requested = std::move(nextRequested);
            m_pendingResize.reset();
            m_resizePersistenceTerminal.reset();
            m_resizePublicationRetriesRemaining = 0;
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
    std::optional<Preferences::UserPreferencesStore::PreparedSave> prepared;
    try {
        prepared.emplace(m_store.prepareSave(nextRequested));
    } catch (const std::exception& error) {
        return publicationFailure(error);
    }
    renderer.setVerticalFovDegrees(candidateDegrees);
    m_effectiveVerticalFovDegrees = candidateDegrees;
    try {
        m_store.publishPrepared(std::move(*prepared));
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

void ApplicationPreferences::initializeInput(
    Input::InputState& input,
    const Input::InputBindings& playerDefaults) {
    m_effectiveBindings =
        Input::compileInputBindings(playerDefaults, m_requested.input);
    m_effectiveInput = m_requested.input;
    input.setBindings(m_effectiveBindings);
}

PreferenceApplyResult ApplicationPreferences::applyInput(
    Input::InputState& input,
    const Input::InputBindings& playerDefaults,
    const Preferences::InputPreferences& candidate) {
    if (!validInputPreferences(candidate)) {
        return {
            PreferenceApplyStatus::Rejected,
            "input request is outside the supported sensitivity or binding limits"};
    }
    if (candidate == m_requested.input && candidate == m_effectiveInput) {
        return {};
    }

    std::shared_ptr<Input::InputBindings> compiled;
    try {
        compiled = Input::compileInputBindings(playerDefaults, candidate);
    } catch (const std::invalid_argument& error) {
        return {PreferenceApplyStatus::Rejected, error.what()};
    }
    Preferences::UserPreferences nextRequested = m_requested;
    nextRequested.input = candidate;
    std::optional<Preferences::UserPreferencesStore::PreparedSave> prepared;
    try {
        prepared.emplace(m_store.prepareSave(nextRequested));
    } catch (const std::exception& error) {
        return publicationFailure(error);
    }

    const Preferences::InputPreferences previousEffective = m_effectiveInput;
    m_effectiveInput = candidate;
    input.setBindings(compiled);
    try {
        m_store.publishPrepared(std::move(*prepared));
        m_requested = std::move(nextRequested);
        m_effectiveBindings = std::move(compiled);
        return {};
    } catch (const std::exception& error) {
        PreferenceApplyResult result = publicationFailure(error);
        if (result.status ==
            PreferenceApplyStatus::PublishedDurabilityUncertain) {
            m_requested = std::move(nextRequested);
            m_effectiveBindings = std::move(compiled);
            return result;
        }
        m_effectiveInput = previousEffective;
        input.setBindings(m_effectiveBindings);
        return result;
    }
}

PreferenceApplyResult ApplicationPreferences::resetControlBindings(
    Input::InputState& input,
    const Input::InputBindings& playerDefaults) {
    Preferences::InputPreferences candidate = m_requested.input;
    candidate.bindings.clear();
    return applyInput(input, playerDefaults, candidate);
}

void ApplicationPreferences::markLogicalResize() {
    m_logicalResizeDirty = true;
}

std::optional<PreferenceApplyResult>
ApplicationPreferences::consumeLogicalResize(
    GlfwRuntime& runtime,
    double observedAt) {
    if (!std::exchange(m_logicalResizeDirty, false) ||
        m_effectiveDisplay.mode != Preferences::DisplayMode::Windowed) {
        return std::nullopt;
    }

    const auto bounds = runtime.windowBounds();
    if (!bounds) {
        return PreferenceApplyResult{
            PreferenceApplyStatus::Rejected,
            "Failed to observe the resized window: " + runtime.lastError()};
    }
    const Preferences::WindowedSize observed{bounds->width, bounds->height};
    if (!validWindowedSize(observed)) {
        return PreferenceApplyResult{
            PreferenceApplyStatus::Rejected,
            "Resized window dimensions are outside the supported range"};
    }
    acceptLogicalResize(observed, observedAt);
    return std::nullopt;
}

void ApplicationPreferences::acceptLogicalResize(
    Preferences::WindowedSize observed,
    double observedAt) {
    if (m_effectiveDisplay.mode != Preferences::DisplayMode::Windowed ||
        !validWindowedSize(observed) ||
        observed == m_effectiveDisplay.windowedSize) {
        return;
    }
    m_effectiveDisplay.windowedSize = observed;
    m_pendingResize = observed;
    m_resizePersistenceTerminal.reset();
    m_resizePublicationRetriesRemaining = 1;
    m_nextResizePersistenceAttempt =
        observedAt + kResizePersistenceDelaySeconds;
}

std::optional<PreferenceApplyResult>
ApplicationPreferences::flushResizePersistence(double now) {
    return persistPendingResize(now, false);
}

std::optional<PreferenceApplyResult>
ApplicationPreferences::flushResizePersistenceForShutdown() {
    auto result = persistPendingResize(now(), true);
    if (result && result->status == PreferenceApplyStatus::NotPublished &&
        m_pendingResize && !m_resizePersistenceTerminal) {
        result = persistPendingResize(now(), true);
    }
    return result;
}

std::optional<PreferenceApplyResult>
ApplicationPreferences::persistPendingResize(double now, bool ignoreDelay) {
    if (!m_pendingResize ||
        (!ignoreDelay && now < m_nextResizePersistenceAttempt)) {
        return std::nullopt;
    }
    if (m_resizePersistenceTerminal) {
        if (!ignoreDelay) {
            return std::nullopt;
        }
        return m_resizePersistenceTerminal;
    }

    const Preferences::WindowedSize observed = *m_pendingResize;
    Preferences::UserPreferences nextRequested = m_requested;
    nextRequested.display.windowedSize = observed;
    std::optional<Preferences::UserPreferencesStore::PreparedSave> prepared;
    try {
        prepared.emplace(m_store.prepareSave(nextRequested));
    } catch (const std::exception& error) {
        PreferenceApplyResult result = publicationFailure(error);
        m_resizePersistenceTerminal = result;
        return result;
    }

    try {
        m_store.publishPrepared(std::move(*prepared));
        m_requested = std::move(nextRequested);
        m_pendingResize.reset();
        m_resizePersistenceTerminal.reset();
        m_resizePublicationRetriesRemaining = 0;
        return PreferenceApplyResult{};
    } catch (const std::exception& error) {
        PreferenceApplyResult result = publicationFailure(error);
        if (result.status ==
            PreferenceApplyStatus::PublishedDurabilityUncertain) {
            m_requested = std::move(nextRequested);
            m_pendingResize.reset();
            m_resizePersistenceTerminal.reset();
            m_resizePublicationRetriesRemaining = 0;
        } else if (result.status == PreferenceApplyStatus::NotPublished) {
            if (m_resizePublicationRetriesRemaining > 0) {
                --m_resizePublicationRetriesRemaining;
                m_nextResizePersistenceAttempt =
                    now + kResizePersistenceRetrySeconds;
            } else {
                m_resizePersistenceTerminal = result;
            }
        } else {
            m_resizePersistenceTerminal = result;
        }
        return result;
    }
}

void registerApplicationPreferenceCallbacks(
    Input::InputCallbackContext& callbacks,
    ApplicationPreferences& preferences) {
    callbacks.logicalResizeContext = &preferences;
    callbacks.logicalResize = [](void* context, int, int) {
        auto& owner = *static_cast<ApplicationPreferences*>(context);
        owner.markLogicalResize();
    };
}

} // namespace Rigel
