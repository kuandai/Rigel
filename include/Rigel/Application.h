#pragma once
#include "Rigel/Preferences/UserPreferences.h"

#include <memory>
#include <string>

namespace Rigel {

class ApplicationTestAccess;

enum class PreferenceApplyStatus {
    Applied,
    Rejected,
    NotPublished,
    PublishedDurabilityUncertain,
};

struct PreferenceApplyResult {
    PreferenceApplyStatus status = PreferenceApplyStatus::Applied;
    std::string message;
};

class Application {
public:
    Application();
    ~Application();

    void run();
    void close();

    PreferenceApplyResult applyDisplayPreferences(
        const Preferences::DisplayPreferences& preferences);
    PreferenceApplyResult applyVerticalFov(double verticalFovDegrees);

    const Preferences::UserPreferences& requestedPreferences() const;
    const Preferences::DisplayPreferences& effectiveDisplayPreferences() const;
    double effectiveVerticalFovDegrees() const;

private:
    enum class Initialization {
        Run,
        Skip,
    };

    struct Impl;
    explicit Application(std::unique_ptr<Impl> impl);
    Application(std::unique_ptr<Impl> impl, Initialization initialization);

    void initialize();

    friend class ApplicationTestAccess;
    std::unique_ptr<Impl> m_impl;
};

}
