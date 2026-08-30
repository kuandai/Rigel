#pragma once
#include "Rigel/LaunchOptions.h"
#include "Rigel/Preferences/UserPreferences.h"

#include <memory>
#include <optional>
#include <string>

namespace Rigel {

class ApplicationTestAccess;

enum class PreferenceApplyStatus {
    Applied,
    Rejected,
    NotPublished,
    PublishedDurabilityUncertain,
    PersistenceBlocked,
};

struct PreferenceApplyResult {
    PreferenceApplyStatus status = PreferenceApplyStatus::Applied;
    std::string message;
};

enum class WindowedSizeIntent {
    Unchanged,
    Changed,
};

class Application {
public:
    Application();
    explicit Application(LaunchOptions launchOptions);
    ~Application();

    void run();
    void close();

    PreferenceApplyResult applyDisplayPreferences(
        const Preferences::DisplayPreferences& preferences,
        WindowedSizeIntent windowedSizeIntent);
    PreferenceApplyResult applyVerticalFov(double verticalFovDegrees);
    PreferenceApplyResult applyShadows(bool enabled);
    // Queues a main-thread request for the next active-world frame boundary.
    PreferenceApplyResult applyViewDistance(int viewDistanceChunks);
    PreferenceApplyResult applyInputPreferences(
        const Preferences::InputPreferences& preferences);
    PreferenceApplyResult resetControlBindings();

    const Preferences::UserPreferences& requestedPreferences() const;
    const Preferences::DisplayPreferences& effectiveDisplayPreferences() const;
    double effectiveVerticalFovDegrees() const;
    bool effectiveShadowsEnabled() const;
    int effectiveViewDistanceChunks() const;
    const Preferences::InputPreferences& effectiveInputPreferences() const;

private:
    enum class Initialization {
        Run,
        Skip,
    };

    struct Impl;
    explicit Application(std::unique_ptr<Impl> impl);
    Application(std::unique_ptr<Impl> impl, Initialization initialization);

    void initialize();
    std::optional<PreferenceApplyResult>
    consumePendingViewDistanceAtFrameBoundary();

    friend class ApplicationTestAccess;
    std::unique_ptr<Impl> m_impl;
};

}
