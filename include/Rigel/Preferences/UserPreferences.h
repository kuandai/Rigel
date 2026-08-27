#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace Rigel::Preferences {

inline constexpr uint32_t kUserPreferencesSchemaVersion = 1;
inline constexpr int kMinimumWindowDimension = 1;
inline constexpr int kMaximumWindowDimension = 16384;
inline constexpr int kMinimumFpsLimit = 30;
inline constexpr int kMaximumFpsLimit = 1000;
inline constexpr int kMinimumViewDistanceChunks = 2;
inline constexpr int kMaximumViewDistanceChunks = 16;
inline constexpr double kMinimumVerticalFovDegrees = 50.0;
inline constexpr double kMaximumVerticalFovDegrees = 110.0;
inline constexpr double kMinimumMouseSensitivity = 0.01;
inline constexpr double kMaximumMouseSensitivity = 1.0;

enum class DisplayMode {
    Windowed,
    Borderless
};

enum class UserAction {
    MoveForward,
    MoveBackward,
    MoveLeft,
    MoveRight,
    Ascend,
    Descend,
    Sprint,
    RemoveBlock,
    PlaceBlock
};

struct WindowedSize {
    int width = 800;
    int height = 600;

    bool operator==(const WindowedSize&) const = default;
};

struct DisplayPreferences {
    DisplayMode mode = DisplayMode::Windowed;
    WindowedSize windowedSize;
    bool vsync = true;
    std::optional<int> fpsLimit;

    bool operator==(const DisplayPreferences&) const = default;
};

struct GraphicsPreferences {
    int viewDistanceChunks = 12;
    bool shadows = true;

    bool operator==(const GraphicsPreferences&) const = default;
};

struct CameraPreferences {
    double verticalFovDegrees = 60.0;

    bool operator==(const CameraPreferences&) const = default;
};

struct InputPreferences {
    double mouseSensitivity = 0.12;
    bool invertY = false;
    std::map<UserAction, std::vector<std::string>> bindings;

    bool operator==(const InputPreferences&) const = default;
};

struct UserPreferences {
    uint32_t schemaVersion = kUserPreferencesSchemaVersion;
    DisplayPreferences display;
    GraphicsPreferences graphics;
    CameraPreferences camera;
    InputPreferences input;

    bool operator==(const UserPreferences&) const = default;
};

class UserPreferencesWriteBlocked final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class UserPreferencesPreparationError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

std::filesystem::path currentUserPreferencesPath();

class UserPreferencesStore final {
public:
    class PreparedSave final {
    public:
        PreparedSave(PreparedSave&&) noexcept;
        PreparedSave& operator=(PreparedSave&&) noexcept;
        ~PreparedSave();

        PreparedSave(const PreparedSave&) = delete;
        PreparedSave& operator=(const PreparedSave&) = delete;

    private:
        struct Impl;

        explicit PreparedSave(std::unique_ptr<Impl> impl);

        std::unique_ptr<Impl> m_impl;

        friend class UserPreferencesStore;
    };

    explicit UserPreferencesStore(std::filesystem::path path);

    static UserPreferencesStore forCurrentUser();

    UserPreferences load();

    PreparedSave prepareSave(const UserPreferences& requested);
    void publishPrepared(PreparedSave prepared);
    void saveRequested(const UserPreferences& requested);

    // Explicitly discards an unreadable, malformed, or unsupported document
    // and its unknown fields. This is the only operation that clears a normal
    // save block without first loading a supported schema.
    void replaceWithRequested(const UserPreferences& requested);

private:
    std::filesystem::path m_path;
    bool m_normalSaveBlocked = false;
};

} // namespace Rigel::Preferences
