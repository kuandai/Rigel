#pragma once

#include "GlfwRuntime.h"
#include "Rigel/Application.h"
#include "Rigel/Core/FramePacer.h"
#include "Rigel/Preferences/UserPreferences.h"

#include <filesystem>
#include <optional>
#include <string>
#include <utility>

namespace Rigel {

namespace Input {
struct InputCallbackContext;
}

namespace Render {
class FrameRenderer;
}

class ApplicationPreferences final {
public:
    struct StartupResult {
        bool usedSafeFallback = false;
        std::string message;
    };

    explicit ApplicationPreferences(std::filesystem::path path);
    ApplicationPreferences(
        std::filesystem::path path,
        Core::FramePacer::Clock clock);

    void load();
    StartupResult initializeDisplay(GlfwRuntime& runtime, bool benchmarkMode);

    PreferenceApplyResult applyDisplay(
        GlfwRuntime& runtime,
        const Preferences::DisplayPreferences& candidate);
    PreferenceApplyResult applyVerticalFov(
        Render::FrameRenderer& renderer,
        double candidateDegrees);

    void observeLogicalResize(int width, int height, double observedAt);
    std::optional<PreferenceApplyResult> flushResizePersistence(double now);
    std::optional<PreferenceApplyResult> flushResizePersistenceForShutdown();

    void waitForNextFrame() { m_framePacer.wait(); }
    void resetFramePacingSchedule() { m_framePacer.reset(); }
    double now() const { return m_framePacer.now(); }

    const Preferences::UserPreferences& requested() const {
        return m_requested;
    }
    const Preferences::DisplayPreferences& effectiveDisplay() const {
        return m_effectiveDisplay;
    }
    double effectiveVerticalFovDegrees() const {
        return m_effectiveVerticalFovDegrees.value();
    }

private:
    bool createDisplay(
        GlfwRuntime& runtime,
        const Preferences::DisplayPreferences& display,
        std::string& failure);
    bool restorePhysicalDisplay(
        GlfwRuntime& runtime,
        const GlfwRuntime::Rectangle& bounds,
        bool decorated,
        bool vsync,
        bool restoreWindow,
        bool restoreSwapInterval,
        std::string& failure);
    Preferences::DisplayPreferences effectiveDisplayFor(
        const Preferences::DisplayPreferences& requested) const;
    std::optional<PreferenceApplyResult> persistPendingResize(
        double now,
        bool ignoreDelay);

    Preferences::UserPreferencesStore m_store;
    Preferences::UserPreferences m_requested;
    Preferences::DisplayPreferences m_effectiveDisplay;
    std::optional<double> m_effectiveVerticalFovDegrees;
    Core::FramePacer m_framePacer;
    bool m_benchmarkMode = false;
    bool m_programmaticWindowChange = false;
    std::optional<std::pair<int, int>> m_windowedPosition;
    std::optional<Preferences::WindowedSize> m_pendingResize;
    std::optional<std::string> m_resizePersistenceBlock;
    double m_nextResizePersistenceAttempt = 0.0;
};

void registerApplicationPreferenceCallbacks(
    Input::InputCallbackContext& callbacks,
    ApplicationPreferences& preferences);

} // namespace Rigel
