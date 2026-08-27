#pragma once

#include "GlfwRuntime.h"
#include "Rigel/Application.h"
#include "Rigel/Core/FramePacer.h"
#include "Rigel/Preferences/UserPreferences.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace Rigel {

namespace Input {
class InputBindings;
class InputState;
struct InputCallbackContext;
}

namespace Render {
class FrameRenderer;
}

namespace Persistence {
class AsyncChunkLoader;
}

namespace Voxel {
class WorldView;
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
        const Preferences::DisplayPreferences& candidate,
        WindowedSizeIntent windowedSizeIntent);
    PreferenceApplyResult applyDisplay(
        GlfwRuntime& runtime,
        const Preferences::DisplayPreferences& candidate) {
        return applyDisplay(
            runtime, candidate, WindowedSizeIntent::Changed);
    }
    PreferenceApplyResult applyVerticalFov(
        Render::FrameRenderer& renderer,
        double candidateDegrees);
    void initializeViewDistance(
        Voxel::WorldView& view,
        Persistence::AsyncChunkLoader& loader);
    PreferenceApplyResult applyViewDistance(
        Voxel::WorldView& view,
        Persistence::AsyncChunkLoader& loader,
        int candidateChunks);
    void initializeInput(
        Input::InputState& input,
        const Input::InputBindings& playerDefaults);
    PreferenceApplyResult applyInput(
        Input::InputState& input,
        const Input::InputBindings& playerDefaults,
        const Preferences::InputPreferences& candidate);
    PreferenceApplyResult resetControlBindings(
        Input::InputState& input,
        const Input::InputBindings& playerDefaults);

    void markLogicalResize();
    std::optional<PreferenceApplyResult> consumeLogicalResize(
        GlfwRuntime& runtime,
        double observedAt);
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
    int effectiveViewDistanceChunks() const {
        return m_effectiveViewDistanceChunks;
    }
    const Preferences::InputPreferences& effectiveInput() const {
        return m_effectiveInput;
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
    void acceptLogicalResize(
        Preferences::WindowedSize observed,
        double observedAt);

    Preferences::UserPreferencesStore m_store;
    Preferences::UserPreferences m_requested;
    Preferences::DisplayPreferences m_effectiveDisplay;
    std::optional<double> m_effectiveVerticalFovDegrees;
    int m_effectiveViewDistanceChunks = 12;
    Preferences::InputPreferences m_effectiveInput;
    std::shared_ptr<const Input::InputBindings> m_effectiveBindings;
    Core::FramePacer m_framePacer;
    bool m_benchmarkMode = false;
    bool m_logicalResizeDirty = false;
    std::optional<std::pair<int, int>> m_windowedPosition;
    std::optional<Preferences::WindowedSize> m_pendingResize;
    std::optional<PreferenceApplyResult> m_resizePersistenceTerminal;
    unsigned int m_resizePublicationRetriesRemaining = 0;
    double m_nextResizePersistenceAttempt = 0.0;

    friend class ApplicationTestAccess;
};

void registerApplicationPreferenceCallbacks(
    Input::InputCallbackContext& callbacks,
    ApplicationPreferences& preferences);

} // namespace Rigel
