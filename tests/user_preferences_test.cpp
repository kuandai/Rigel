#include "TestFramework.h"
#include "LogCapture.h"

#include "Rigel/Persistence/Storage.h"
#include "Rigel/Preferences/UserPreferences.h"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace Rigel::Preferences;

namespace Rigel::Preferences::detail {

void setUserPreferencesAfterSavePreflightHookForTesting(
    std::function<void()> hook);

void setUserPreferencesAfterPublicationHookForTesting(
    std::function<void()> hook);

bool tryPublishCooperatingUserPreferencesDocumentForTesting(
    const std::filesystem::path& path,
    const std::string& document);

} // namespace Rigel::Preferences::detail

namespace {

void writeDocument(const std::filesystem::path& path,
                   const std::string& document) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(document.data(), static_cast<std::streamsize>(document.size()));
    stream.close();
    if (!stream) {
        throw Rigel::Test::TestFailure(
            "Failed to write user preferences fixture");
    }
}

std::string readDocument(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return std::string(
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>());
}

std::vector<std::filesystem::path> stagingFiles(
    const std::filesystem::path& path) {
    std::vector<std::filesystem::path> result;
    if (!std::filesystem::exists(path.parent_path())) {
        return result;
    }
    const std::string prefix = path.filename().string() + ".tmp.";
    for (const auto& entry :
         std::filesystem::directory_iterator(path.parent_path())) {
        if (entry.path().filename().string().starts_with(prefix)) {
            result.push_back(entry.path());
        }
    }
    return result;
}

class ScopedEnvironment final {
public:
    ScopedEnvironment(std::string name, std::optional<std::string> value)
        : m_name(std::move(name)) {
        if (const char* previous = std::getenv(m_name.c_str())) {
            m_previous = previous;
        }
        set(std::move(value));
    }

    ~ScopedEnvironment() {
        set(m_previous);
    }

private:
    void set(const std::optional<std::string>& value) const {
#ifdef _WIN32
        _putenv_s(m_name.c_str(), value ? value->c_str() : "");
#else
        if (value) {
            setenv(m_name.c_str(), value->c_str(), 1);
        } else {
            unsetenv(m_name.c_str());
        }
#endif
    }

    std::string m_name;
    std::optional<std::string> m_previous;
};

class ScopedCurrentPath final {
public:
    explicit ScopedCurrentPath(const std::filesystem::path& path)
        : m_previous(std::filesystem::current_path()) {
        std::filesystem::current_path(path);
    }

    ~ScopedCurrentPath() {
        std::error_code error;
        std::filesystem::current_path(m_previous, error);
    }

private:
    std::filesystem::path m_previous;
};

} // namespace

TEST_CASE(UserPreferences_missing_file_uses_shipped_defaults_without_creation) {
    Rigel::Test::TemporaryDirectory directory("rigel_user_preferences");
    const auto path = directory.path() / "profile" / "user-preferences.yaml";
    UserPreferencesStore store(path);

    CHECK_EQ(store.load(), UserPreferences{});
    CHECK(!std::filesystem::exists(path));
    CHECK_THROWS(UserPreferencesStore("relative/user-preferences.yaml"));
}

TEST_CASE(UserPreferences_path_is_explicit_and_uses_platform_config_policy) {
    Rigel::Test::TemporaryDirectory directory("rigel_user_preferences_path");
    const auto explicitPath =
        directory.path() / "platform" / "user-preferences.yaml";
    writeDocument(
        directory.path() / "user-preferences.yaml",
        "schema_version: 1\ngraphics: {view_distance_chunks: 2}\n");
    writeDocument(
        directory.path() / "config" / "user-preferences.yaml",
        "schema_version: 1\ngraphics: {view_distance_chunks: 3}\n");
    writeDocument(
        directory.path() / "saves" / "world_0" / "user-preferences.yaml",
        "schema_version: 1\ngraphics: {view_distance_chunks: 4}\n");

    {
        ScopedCurrentPath cwd(directory.path());
        UserPreferencesStore store(explicitPath);
        CHECK_EQ(store.load().graphics.viewDistanceChunks, 12);
        CHECK(!std::filesystem::exists(explicitPath));
    }

#ifndef _WIN32
    const auto xdg = directory.path() / "xdg";
    const auto home = directory.path() / "home";
    ScopedEnvironment setHome("HOME", home.string());
    {
        ScopedEnvironment setXdg("XDG_CONFIG_HOME", xdg.string());
        CHECK_EQ(
            currentUserPreferencesPath(),
            xdg / "rigel" / "user-preferences.yaml");
    }
    {
        ScopedEnvironment relativeXdg("XDG_CONFIG_HOME", "relative-xdg");
        CHECK_EQ(
            currentUserPreferencesPath(),
            home / ".config" / "rigel" / "user-preferences.yaml");
    }
#endif
}

TEST_CASE(UserPreferences_boundary_values_and_sparse_bindings_round_trip) {
    Rigel::Test::TemporaryDirectory directory("rigel_user_preferences_bounds");
    const auto path = directory.path() / "user-preferences.yaml";
    UserPreferencesStore store(path);
    UserPreferences requested;
    requested.display.mode = DisplayMode::Borderless;
    requested.display.windowedSize = {1, 16384};
    requested.display.vsync = false;
    requested.display.fpsLimit = 30;
    requested.graphics.viewDistanceChunks = 2;
    requested.graphics.shadows = false;
    requested.camera.verticalFovDegrees = 50.0;
    requested.input.mouseSensitivity = 0.01;
    requested.input.invertY = true;
    requested.input.bindings[UserAction::MoveForward] = {"W", "UP"};
    requested.input.bindings[UserAction::PlaceBlock] = {};

    store.saveRequested(requested);
    CHECK_EQ(store.load(), requested);
    CHECK(stagingFiles(path).empty());

    requested.display.fpsLimit = 1000;
    requested.graphics.viewDistanceChunks = 16;
    requested.camera.verticalFovDegrees = 110.0;
    requested.input.mouseSensitivity = 1.0;
    store.saveRequested(requested);
    CHECK_EQ(store.load(), requested);
}

TEST_CASE(UserPreferences_invalid_leaves_and_sections_preserve_valid_siblings) {
    Rigel::Test::TemporaryDirectory directory("rigel_user_preferences_tolerant");
    const auto path = directory.path() / "user-preferences.yaml";
    writeDocument(path,
        "schema_version: 1\n"
        "future_root: true\n"
        "display:\n"
        "  mode: borderless\n"
        "  windowed_size: [1920, nope]\n"
        "  vsync: \"false\"\n"
        "  fps_limit: 144\n"
        "graphics:\n"
        "  view_distance_chunks: 16\n"
        "  shadows: maybe\n"
        "camera: []\n"
        "input:\n"
        "  mouse_sensitivity: 0.5\n"
        "  invert_y: true\n"
        "  bindings:\n"
        "    move_forward: [W, bad-token]\n"
        "    place_block: []\n"
        "    fly: [F]\n");
    UserPreferencesStore store(path);
    Rigel::Test::LogCapture logs("user-preferences-tolerant");

    const UserPreferences requested = store.load();

    CHECK_EQ(requested.display.mode, DisplayMode::Borderless);
    CHECK_EQ(requested.display.windowedSize, WindowedSize{});
    CHECK(requested.display.vsync);
    CHECK_EQ(requested.display.fpsLimit, std::optional<int>(144));
    CHECK_EQ(requested.graphics.viewDistanceChunks, 16);
    CHECK(requested.graphics.shadows);
    CHECK_NEAR(requested.camera.verticalFovDegrees, 60.0, 0.0001);
    CHECK_NEAR(requested.input.mouseSensitivity, 0.5, 0.0001);
    CHECK(requested.input.invertY);
    CHECK(!requested.input.bindings.contains(UserAction::MoveForward));
    CHECK_EQ(
        requested.input.bindings.at(UserAction::PlaceBlock),
        std::vector<std::string>{});
    CHECK_EQ(
        logs.output(),
        "Unknown user preference 'future_root' in '" + path.string() +
        "'; ignored\n"
        "Invalid user preference 'display.windowed_size' in '" +
        path.string() +
        "': expected two integers in [1, 16384], got sequence; using shipped default\n"
        "Invalid user preference 'display.vsync' in '" + path.string() +
        "': expected boolean, got 'false'; using shipped default\n"
        "Invalid user preference 'graphics.shadows' in '" + path.string() +
        "': expected boolean, got 'maybe'; using shipped default\n"
        "Invalid user preference section 'camera' in '" + path.string() +
        "': expected mapping, got sequence; using shipped defaults for section\n"
        "Invalid user preference 'input.bindings.move_forward' in '" +
        path.string() +
        "': expected list of at most 8 keyboard/mouse string tokens, got sequence; using shipped default\n"
        "Unknown user preference action 'fly' in '" + path.string() +
        "'; ignored\n");
}

TEST_CASE(UserPreferences_unknown_supported_nodes_survive_known_edits) {
    Rigel::Test::TemporaryDirectory directory("rigel_user_preferences_unknown");
    const auto path = directory.path() / "user-preferences.yaml";
    writeDocument(path,
        "schema_version: 1\n"
        "future_root: {enabled: true}\n"
        "graphics:\n"
        "  view_distance_chunks: 7\n"
        "  future_quality: ultra\n"
        "input:\n"
        "  bindings:\n"
        "    future_action: [F]\n");
    UserPreferencesStore store(path);
    UserPreferences requested;
    {
        Rigel::Test::LogCapture logs("user-preferences-unknown-load");
        requested = store.load();
        CHECK_EQ(Rigel::Test::countOccurrences(logs.output(), "ignored"), 3u);
    }
    requested.graphics.viewDistanceChunks = 9;
    {
        Rigel::Test::LogCapture logs("user-preferences-unknown-save");
        store.saveRequested(requested);
        CHECK_EQ(Rigel::Test::countOccurrences(logs.output(), "ignored"), 3u);
    }

    const std::string saved = readDocument(path);
    CHECK_NE(saved.find("future_root:"), std::string::npos);
    CHECK_NE(saved.find("future_quality: ultra"), std::string::npos);
    CHECK_NE(saved.find("future_action:"), std::string::npos);
    CHECK_EQ(store.load().graphics.viewDistanceChunks, 9);
}

TEST_CASE(UserPreferences_duplicate_unknown_keys_are_tolerated_and_preserved) {
    Rigel::Test::TemporaryDirectory directory(
        "rigel_user_preferences_unknown_duplicates");
    struct UnknownDuplicateCase {
        std::string name;
        std::string document;
        std::string warning;
        std::string duplicatedKey;
        std::vector<std::string> retainedFragments;
        size_t warningCount;
    };
    const std::vector<UnknownDuplicateCase> cases{
        {
            "root-subtree",
            "schema_version: 1\n"
            "graphics: {view_distance_chunks: 7}\n"
            "future_root:\n"
            "  variant: alpha\n"
            "  variant: beta\n",
            "Unknown user preference 'future_root'",
            "variant:",
            {"variant: alpha", "variant: beta"},
            1
        },
        {
            "binding-action",
            "schema_version: 1\n"
            "graphics: {view_distance_chunks: 7}\n"
            "input:\n"
            "  bindings:\n"
            "    future_action: [F]\n"
            "    future_action: [G]\n",
            "Unknown user preference action 'future_action'",
            "future_action:",
            {"future_action: [F]", "future_action: [G]"},
            2
        }
    };

    for (const UnknownDuplicateCase& duplicateCase : cases) {
        const auto path = directory.path() / duplicateCase.name /
            "user-preferences.yaml";
        writeDocument(path, duplicateCase.document);
        UserPreferencesStore store(path);
        UserPreferences requested;
        {
            Rigel::Test::LogCapture logs(
                "user-preferences-unknown-duplicate-load-" +
                duplicateCase.name);
            requested = store.load();
            CHECK_EQ(
                Rigel::Test::countOccurrences(
                    logs.output(), duplicateCase.warning),
                duplicateCase.warningCount);
        }
        CHECK_EQ(requested.graphics.viewDistanceChunks, 7);

        requested.graphics.viewDistanceChunks = 9;
        {
            Rigel::Test::LogCapture logs(
                "user-preferences-unknown-duplicate-save-" +
                duplicateCase.name);
            CHECK_NO_THROW(store.saveRequested(requested));
            CHECK_EQ(
                Rigel::Test::countOccurrences(
                    logs.output(), duplicateCase.warning),
                duplicateCase.warningCount);
        }

        const std::string saved = readDocument(path);
        CHECK_EQ(
            Rigel::Test::countOccurrences(saved, duplicateCase.duplicatedKey),
            2u);
        for (const std::string& fragment : duplicateCase.retainedFragments) {
            CHECK_NE(saved.find(fragment), std::string::npos);
        }
        {
            Rigel::Test::LogCapture logs(
                "user-preferences-unknown-duplicate-reload-" +
                duplicateCase.name);
            CHECK_EQ(store.load().graphics.viewDistanceChunks, 9);
        }
    }
}

TEST_CASE(UserPreferences_serialized_size_limit_preserves_exact_limit_input) {
    Rigel::Test::TemporaryDirectory directory("rigel_user_preferences_size");
    const auto path = directory.path() / "user-preferences.yaml";
    constexpr size_t maximumDocumentBytes = 262144;
    const std::string prefix =
        "schema_version: 1\n"
        "graphics: {view_distance_chunks: 8}\n"
        "retained: ";
    const std::string suffix = "\n";
    CHECK(prefix.size() + suffix.size() < maximumDocumentBytes);
    const std::string original = prefix +
        std::string(
            maximumDocumentBytes - prefix.size() - suffix.size(), 'x') +
        suffix;
    CHECK_EQ(original.size(), maximumDocumentBytes);
    writeDocument(path, original);

    UserPreferencesStore store(path);
    UserPreferences requested;
    {
        Rigel::Test::LogCapture logs("user-preferences-size-load");
        requested = store.load();
        CHECK_EQ(
            Rigel::Test::countOccurrences(
                logs.output(), "Unknown user preference 'retained'"),
            1u);
    }
    CHECK_EQ(requested.graphics.viewDistanceChunks, 8);
    requested.graphics.viewDistanceChunks = 9;

    bool failedBeforePublication = false;
    try {
        Rigel::Test::LogCapture logs("user-preferences-size-save");
        store.saveRequested(requested);
    } catch (const Rigel::Persistence::AtomicFilePublicationError& error) {
        CHECK_EQ(
            error.state(),
            Rigel::Persistence::AtomicFilePublicationState::NotPublished);
        failedBeforePublication = true;
    }
    CHECK(failedBeforePublication);
    CHECK_EQ(readDocument(path), original);
    CHECK_EQ(std::filesystem::file_size(path), maximumDocumentBytes);
    CHECK(stagingFiles(path).empty());
}

TEST_CASE(UserPreferences_unsafe_documents_preserve_bytes_and_block_normal_save) {
    Rigel::Test::TemporaryDirectory directory("rigel_user_preferences_unsafe");
    const std::vector<std::pair<std::string, std::string>> cases{
        {"malformed", "schema_version: [\n"},
        {"missing-schema", "display: {vsync: false}\n"},
        {"invalid-schema", "schema_version: \"1\"\n"},
        {"older-schema", "schema_version: 0\nlegacy: preserved\n"},
        {"newer-schema", "schema_version: 2\nfuture: preserved\n"}
    };

    for (const auto& [name, original] : cases) {
        const auto path = directory.path() / name / "user-preferences.yaml";
        writeDocument(path, original);
        UserPreferencesStore store(path);
        Rigel::Test::LogCapture logs("user-preferences-unsafe-" + name);
        CHECK_EQ(store.load(), UserPreferences{});
        CHECK_NE(logs.output().find("preserving the file"), std::string::npos);
        CHECK_THROWS(store.saveRequested(UserPreferences{}));
        CHECK_EQ(readDocument(path), original);
        CHECK(stagingFiles(path).empty());
    }

    const auto oversized =
        directory.path() / "oversized" / "user-preferences.yaml";
    writeDocument(oversized, std::string(262145, 'x'));
    UserPreferencesStore oversizedStore(oversized);
    CHECK_EQ(oversizedStore.load(), UserPreferences{});
    CHECK_THROWS(oversizedStore.saveRequested(UserPreferences{}));
    CHECK_EQ(std::filesystem::file_size(oversized), 262145u);

#ifndef _WIN32
    const auto unreadable =
        directory.path() / "unreadable" / "user-preferences.yaml";
    writeDocument(unreadable, "schema_version: 1\n");
    std::filesystem::permissions(
        unreadable,
        std::filesystem::perms::none,
        std::filesystem::perm_options::replace);
    UserPreferencesStore unreadableStore(unreadable);
    CHECK_EQ(unreadableStore.load(), UserPreferences{});
    CHECK_THROWS(unreadableStore.saveRequested(UserPreferences{}));
    std::filesystem::permissions(
        unreadable,
        std::filesystem::perms::owner_read |
            std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace);
#endif

    const auto nonregular =
        directory.path() / "nonregular" / "user-preferences.yaml";
    std::filesystem::create_directories(nonregular);
    UserPreferencesStore nonregularStore(nonregular);
    CHECK_EQ(nonregularStore.load(), UserPreferences{});
    CHECK_THROWS(nonregularStore.saveRequested(UserPreferences{}));

    const auto replacePath =
        directory.path() / "newer-schema" / "user-preferences.yaml";
    UserPreferencesStore replaceStore(replacePath);
    replaceStore.load();
    UserPreferences replacement;
    replacement.graphics.viewDistanceChunks = 6;
    replaceStore.replaceWithRequested(replacement);
    replacement.graphics.viewDistanceChunks = 7;
    CHECK_NO_THROW(replaceStore.saveRequested(replacement));
    CHECK_EQ(replaceStore.load(), replacement);
}

TEST_CASE(UserPreferences_duplicate_mapping_keys_require_explicit_replacement) {
    Rigel::Test::TemporaryDirectory directory(
        "rigel_user_preferences_duplicates");
    struct DuplicateCase {
        std::string name;
        std::string document;
        std::string path;
    };
    const std::vector<DuplicateCase> cases{
        {
            "schema-version",
            "schema_version: 1\n"
            "schema_version: 2\n"
            "future_root: preserved\n",
            "schema_version"
        },
        {
            "known-scalar",
            "schema_version: 1\n"
            "graphics:\n"
            "  view_distance_chunks: 8\n"
            "  view_distance_chunks: invalid\n",
            "graphics.view_distance_chunks"
        },
        {
            "known-section-with-unknown-field",
            "schema_version: 1\n"
            "graphics:\n"
            "  view_distance_chunks: 8\n"
            "graphics:\n"
            "  future_quality: ultra\n"
            "  shadows: false\n",
            "graphics"
        },
        {
            "binding-action",
            "schema_version: 1\n"
            "input:\n"
            "  bindings:\n"
            "    move_forward: [W]\n"
            "    move_forward: [invalid-token]\n",
            "input.bindings.move_forward"
        }
    };

    for (const DuplicateCase& duplicateCase : cases) {
        const auto path = directory.path() / duplicateCase.name /
            "user-preferences.yaml";
        writeDocument(path, duplicateCase.document);
        UserPreferencesStore store(path);
        {
            Rigel::Test::LogCapture logs(
                "user-preferences-duplicate-" + duplicateCase.name);
            CHECK_EQ(store.load(), UserPreferences{});
            CHECK_EQ(
                logs.output(),
                "User preferences file '" + path.string() +
                    "' cannot be loaded: duplicate mapping key at '" +
                    duplicateCase.path +
                    "'; using shipped defaults and preserving the file\n");
        }

        bool normalSaveBlocked = false;
        try {
            store.saveRequested(UserPreferences{});
        } catch (const UserPreferencesWriteBlocked&) {
            normalSaveBlocked = true;
        }
        CHECK(normalSaveBlocked);
        CHECK_EQ(readDocument(path), duplicateCase.document);
        CHECK(stagingFiles(path).empty());

        UserPreferences replacement;
        replacement.graphics.viewDistanceChunks = 6;
        store.replaceWithRequested(replacement);
        CHECK_EQ(store.load(), replacement);
        replacement.graphics.viewDistanceChunks = 7;
        CHECK_NO_THROW(store.saveRequested(replacement));
        CHECK_EQ(store.load(), replacement);
    }
}

TEST_CASE(UserPreferences_invalid_candidates_do_not_touch_or_wait_for_storage) {
    Rigel::Test::TemporaryDirectory directory(
        "rigel_user_preferences_invalid_candidate");
    UserPreferences invalid;
    invalid.graphics.viewDistanceChunks = kMaximumViewDistanceChunks + 1;

    const auto savePath =
        directory.path() / "save" / "profile" / "user-preferences.yaml";
    UserPreferencesStore saveStore(savePath);
    CHECK_THROWS(saveStore.saveRequested(invalid));
    CHECK(!std::filesystem::exists(savePath.parent_path()));
    CHECK(!std::filesystem::exists(savePath));
    CHECK(!std::filesystem::exists(savePath.string() + ".lock"));
    CHECK(stagingFiles(savePath).empty());

    const auto replacePath =
        directory.path() / "replace" / "profile" / "user-preferences.yaml";
    UserPreferencesStore replaceStore(replacePath);
    CHECK_THROWS(replaceStore.replaceWithRequested(invalid));
    CHECK(!std::filesystem::exists(replacePath.parent_path()));
    CHECK(!std::filesystem::exists(replacePath));
    CHECK(!std::filesystem::exists(replacePath.string() + ".lock"));
    CHECK(stagingFiles(replacePath).empty());

    const auto lockedPath = directory.path() / "locked-user-preferences.yaml";
    UserPreferencesStore writer(lockedPath);
    UserPreferences valid;
    valid.graphics.viewDistanceChunks = 8;

    std::mutex mutex;
    std::condition_variable lockHeld;
    std::condition_variable continueWriter;
    bool isLockHeld = false;
    bool mayContinue = false;
    std::exception_ptr writerFailure;
    std::thread writerThread([&]() {
        detail::setUserPreferencesAfterSavePreflightHookForTesting([&]() {
            std::unique_lock lock(mutex);
            isLockHeld = true;
            lockHeld.notify_one();
            continueWriter.wait(lock, [&]() { return mayContinue; });
        });
        try {
            writer.saveRequested(valid);
        } catch (...) {
            writerFailure = std::current_exception();
        }
        detail::setUserPreferencesAfterSavePreflightHookForTesting({});
    });
    {
        std::unique_lock lock(mutex);
        lockHeld.wait(lock, [&]() { return isLockHeld; });
    }

    UserPreferencesStore lockedSaveStore(lockedPath);
    UserPreferencesStore lockedReplaceStore(lockedPath);
    auto invalidSave = std::async(std::launch::async, [&]() {
        CHECK_THROWS(lockedSaveStore.saveRequested(invalid));
    });
    auto invalidReplace = std::async(std::launch::async, [&]() {
        CHECK_THROWS(lockedReplaceStore.replaceWithRequested(invalid));
    });
    const bool saveReturnedWithoutLock =
        invalidSave.wait_for(std::chrono::seconds(1)) ==
        std::future_status::ready;
    const bool replaceReturnedWithoutLock =
        invalidReplace.wait_for(std::chrono::seconds(1)) ==
        std::future_status::ready;

    {
        std::lock_guard lock(mutex);
        mayContinue = true;
    }
    continueWriter.notify_one();
    writerThread.join();
    invalidSave.get();
    invalidReplace.get();
    if (writerFailure) {
        std::rethrow_exception(writerFailure);
    }
    CHECK(saveReturnedWithoutLock);
    CHECK(replaceReturnedWithoutLock);
}

TEST_CASE(UserPreferences_rechecks_external_replacement_before_saving) {
    Rigel::Test::TemporaryDirectory directory("rigel_user_preferences_replace");
    const auto path = directory.path() / "user-preferences.yaml";
    const std::string supported =
        "schema_version: 1\ngraphics: {view_distance_chunks: 8}\n";
    const std::string newer =
        "schema_version: 2\nfuture: must-survive\n";
    writeDocument(path, supported);
    UserPreferencesStore store(path);
    UserPreferences requested = store.load();
    requested.graphics.viewDistanceChunks = 10;
    writeDocument(path, newer);

    CHECK_THROWS(store.saveRequested(requested));
    CHECK_EQ(readDocument(path), newer);

    writeDocument(path, supported);
    CHECK_THROWS(store.saveRequested(requested));
    CHECK_EQ(readDocument(path), supported);
}

TEST_CASE(UserPreferences_reload_unblocks_normal_save_after_external_recovery) {
    Rigel::Test::TemporaryDirectory directory(
        "rigel_user_preferences_external_recovery");
    const std::string unsafe =
        "schema_version: 2\nfuture: must-survive-until-recovery\n";
    const std::string supported =
        "schema_version: 1\ngraphics: {view_distance_chunks: 8}\n";

    const auto replacedPath =
        directory.path() / "replaced" / "user-preferences.yaml";
    writeDocument(replacedPath, unsafe);
    UserPreferencesStore replacedStore(replacedPath);
    CHECK_EQ(replacedStore.load(), UserPreferences{});
    CHECK_THROWS(replacedStore.saveRequested(UserPreferences{}));
    writeDocument(replacedPath, supported);
    UserPreferences replacedRequest = replacedStore.load();
    replacedRequest.graphics.viewDistanceChunks = 9;
    CHECK_NO_THROW(replacedStore.saveRequested(replacedRequest));
    CHECK_EQ(replacedStore.load(), replacedRequest);

    const auto removedPath =
        directory.path() / "removed" / "user-preferences.yaml";
    writeDocument(removedPath, unsafe);
    UserPreferencesStore removedStore(removedPath);
    CHECK_EQ(removedStore.load(), UserPreferences{});
    CHECK_THROWS(removedStore.saveRequested(UserPreferences{}));
    CHECK(std::filesystem::remove(removedPath));
    CHECK_EQ(removedStore.load(), UserPreferences{});
    UserPreferences removedRequest;
    removedRequest.graphics.viewDistanceChunks = 10;
    CHECK_NO_THROW(removedStore.saveRequested(removedRequest));
    CHECK_EQ(removedStore.load(), removedRequest);
}

TEST_CASE(UserPreferences_save_lock_excludes_newer_writer_after_preflight) {
    Rigel::Test::TemporaryDirectory directory("rigel_user_preferences_lock");
    const auto path = directory.path() / "user-preferences.yaml";
    writeDocument(
        path,
        "schema_version: 1\ngraphics: {view_distance_chunks: 8}\n");
    UserPreferencesStore store(path);
    UserPreferences requested = store.load();
    requested.graphics.viewDistanceChunks = 10;

    std::mutex mutex;
    std::condition_variable preflightReached;
    std::condition_variable continueSave;
    bool isAfterPreflight = false;
    bool mayContinue = false;
    std::exception_ptr saveFailure;
    std::thread saver([&]() {
        detail::setUserPreferencesAfterSavePreflightHookForTesting([&]() {
            std::unique_lock lock(mutex);
            isAfterPreflight = true;
            preflightReached.notify_one();
            continueSave.wait(lock, [&]() { return mayContinue; });
        });
        try {
            store.saveRequested(requested);
        } catch (...) {
            saveFailure = std::current_exception();
        }
        detail::setUserPreferencesAfterSavePreflightHookForTesting({});
    });

    {
        std::unique_lock lock(mutex);
        preflightReached.wait(lock, [&]() { return isAfterPreflight; });
    }

    const std::string newer =
        "schema_version: 2\nfuture: installed-by-newer-rigel\n";
    const bool publishedWhileSaveWasLocked =
        detail::tryPublishCooperatingUserPreferencesDocumentForTesting(
            path, newer);
    const std::string documentWhileSaveWasLocked = readDocument(path);

    {
        std::lock_guard lock(mutex);
        mayContinue = true;
    }
    continueSave.notify_one();
    saver.join();
    if (saveFailure) {
        std::rethrow_exception(saveFailure);
    }
    CHECK(!publishedWhileSaveWasLocked);
    CHECK_NE(
        documentWhileSaveWasLocked.find("schema_version: 1"),
        std::string::npos);
    CHECK_EQ(store.load().graphics.viewDistanceChunks, 10);

    CHECK(detail::tryPublishCooperatingUserPreferencesDocumentForTesting(
        path, newer));
    CHECK_EQ(readDocument(path), newer);
}

TEST_CASE(UserPreferences_prepublication_failure_preserves_last_valid_file) {
#ifdef _WIN32
    throw Rigel::Test::TestSkip(
        "Directory write permission behavior is platform-specific");
#else
    Rigel::Test::TemporaryDirectory directory("rigel_user_preferences_failure");
    const auto path = directory.path() / "profile" / "user-preferences.yaml";
    UserPreferencesStore store(path);
    UserPreferences requested;
    requested.graphics.viewDistanceChunks = 7;
    store.saveRequested(requested);
    const std::string previous = readDocument(path);
    requested.graphics.viewDistanceChunks = 11;

    std::filesystem::permissions(
        path.parent_path(),
        std::filesystem::perms::owner_read |
            std::filesystem::perms::owner_exec,
        std::filesystem::perm_options::replace);
    bool failed = false;
    try {
        store.saveRequested(requested);
    } catch (const Rigel::Persistence::AtomicFilePublicationError& error) {
        CHECK_EQ(
            error.state(),
            Rigel::Persistence::AtomicFilePublicationState::NotPublished);
        failed = true;
    }
    std::filesystem::permissions(
        path.parent_path(),
        std::filesystem::perms::owner_all,
        std::filesystem::perm_options::replace);

    CHECK(failed);
    CHECK_EQ(readDocument(path), previous);
    CHECK(stagingFiles(path).empty());
    CHECK_NO_THROW(store.saveRequested(requested));
    CHECK_EQ(store.load(), requested);
#endif
}

TEST_CASE(UserPreferences_failed_explicit_replace_keeps_unsafe_save_block) {
#ifdef _WIN32
    throw Rigel::Test::TestSkip(
        "Directory write permission behavior is platform-specific");
#else
    Rigel::Test::TemporaryDirectory directory(
        "rigel_user_preferences_failed_replace");
    const auto path = directory.path() / "profile" / "user-preferences.yaml";
    const std::string original =
        "schema_version: 2\nfuture: preserved-until-replace\n";
    writeDocument(path, original);
    writeDocument(std::filesystem::path(path.string() + ".lock"), {});
    UserPreferencesStore store(path);
    {
        Rigel::Test::LogCapture logs("user-preferences-failed-replace-load");
        CHECK_EQ(store.load(), UserPreferences{});
    }

    UserPreferences replacement;
    replacement.graphics.viewDistanceChunks = 6;
    std::filesystem::permissions(
        path.parent_path(),
        std::filesystem::perms::owner_read |
            std::filesystem::perms::owner_exec,
        std::filesystem::perm_options::replace);
    bool definitelyNotPublished = false;
    try {
        store.replaceWithRequested(replacement);
    } catch (const Rigel::Persistence::AtomicFilePublicationError& error) {
        CHECK_EQ(
            error.state(),
            Rigel::Persistence::AtomicFilePublicationState::NotPublished);
        definitelyNotPublished = true;
    }
    std::filesystem::permissions(
        path.parent_path(),
        std::filesystem::perms::owner_all,
        std::filesystem::perm_options::replace);

    CHECK(definitelyNotPublished);
    CHECK_EQ(readDocument(path), original);
    CHECK(stagingFiles(path).empty());
    bool normalSaveBlocked = false;
    try {
        store.saveRequested(replacement);
    } catch (const UserPreferencesWriteBlocked&) {
        normalSaveBlocked = true;
    }
    CHECK(normalSaveBlocked);
    CHECK_EQ(readDocument(path), original);

    store.replaceWithRequested(replacement);
    replacement.graphics.viewDistanceChunks = 7;
    CHECK_NO_THROW(store.saveRequested(replacement));
    CHECK_EQ(store.load(), replacement);
#endif
}

TEST_CASE(UserPreferences_replace_published_uncertainty_clears_save_block) {
    Rigel::Test::TemporaryDirectory directory(
        "rigel_user_preferences_replace_uncertain");
    const auto path = directory.path() / "user-preferences.yaml";
    writeDocument(path, "schema_version: 2\nfuture: preserved-until-replace\n");
    UserPreferencesStore store(path);
    CHECK_EQ(store.load(), UserPreferences{});
    CHECK_THROWS(store.saveRequested(UserPreferences{}));

    UserPreferences replacement;
    replacement.graphics.viewDistanceChunks = 6;
    detail::setUserPreferencesAfterPublicationHookForTesting([]() {
        throw Rigel::Persistence::AtomicFilePublicationError(
            Rigel::Persistence::AtomicFilePublicationState::
                PublishedDurabilityUncertain,
            "injected post-publication durability uncertainty");
    });
    bool reportedUncertainty = false;
    try {
        store.replaceWithRequested(replacement);
    } catch (const Rigel::Persistence::AtomicFilePublicationError& error) {
        CHECK_EQ(
            error.state(),
            Rigel::Persistence::AtomicFilePublicationState::
                PublishedDurabilityUncertain);
        reportedUncertainty = true;
    }
    detail::setUserPreferencesAfterPublicationHookForTesting({});

    CHECK(reportedUncertainty);
    CHECK_EQ(UserPreferencesStore(path).load(), replacement);
    CHECK(stagingFiles(path).empty());
    replacement.graphics.viewDistanceChunks = 7;
    CHECK_NO_THROW(store.saveRequested(replacement));
    CHECK_EQ(store.load(), replacement);
}
