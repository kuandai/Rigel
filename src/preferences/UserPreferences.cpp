#include "Rigel/Preferences/UserPreferences.h"

#include "Rigel/Persistence/Storage.h"
#include "Rigel/Util/Ryml.h"

#include <ryml.hpp>
#include <ryml_std.hpp>
#include <spdlog/spdlog.h>

#include <array>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <memory>
#include <string_view>
#include <system_error>
#include <utility>

namespace Rigel::Preferences {
namespace {

constexpr size_t kMaximumUserPreferencesBytes = 256 * 1024;
constexpr size_t kMaximumBindingsPerAction = 8;
constexpr size_t kMaximumBindingTokenBytes = 64;

constexpr std::array<std::pair<UserAction, std::string_view>, 9>
    kUserActionNames{{
        {UserAction::MoveForward, "move_forward"},
        {UserAction::MoveBackward, "move_backward"},
        {UserAction::MoveLeft, "move_left"},
        {UserAction::MoveRight, "move_right"},
        {UserAction::Ascend, "ascend"},
        {UserAction::Descend, "descend"},
        {UserAction::Sprint, "sprint"},
        {UserAction::RemoveBlock, "remove_block"},
        {UserAction::PlaceBlock, "place_block"}
    }};

enum class DocumentKind {
    Missing,
    Supported,
    Unsafe
};

struct DocumentInspection {
    DocumentKind kind = DocumentKind::Unsafe;
    UserPreferences preferences;
    std::unique_ptr<ryml::Tree> tree;
    std::string problem;
};

class YamlParseError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

[[noreturn]] void throwYamlParseError(const char* message,
                                      size_t length,
                                      ryml::Location,
                                      void*) {
    throw YamlParseError(std::string(message, length));
}

std::unique_ptr<ryml::Tree> parseYaml(std::string_view document,
                                     std::string_view filename) {
    ryml::Callbacks callbacks = ryml::get_callbacks();
    callbacks.m_error = &throwYamlParseError;
    auto tree = std::make_unique<ryml::Tree>(callbacks);
    ryml::Parser::handler_type handler(callbacks);
    ryml::Parser parser(&handler);
    ryml::parse_in_arena(
        &parser,
        ryml::csubstr(filename.data(), filename.size()),
        ryml::csubstr(document.data(), document.size()),
        tree.get());
    return tree;
}

std::string sourceName(const std::filesystem::path& path) {
    return path.string();
}

std::optional<UserAction> parseUserAction(std::string_view name) {
    for (const auto& [action, actionName] : kUserActionNames) {
        if (name == actionName) {
            return action;
        }
    }
    return std::nullopt;
}

std::string_view userActionName(UserAction action) {
    for (const auto& [candidate, name] : kUserActionNames) {
        if (action == candidate) {
            return name;
        }
    }
    throw std::invalid_argument("Unknown user preference action");
}

std::string scalarText(ryml::ConstNodeRef node) {
    if (!node.readable() || !node.has_val() || node.is_container()) {
        return {};
    }
    return Util::toStdString(node.val());
}

std::string describedValue(ryml::ConstNodeRef node) {
    if (!node.readable()) {
        return "missing value";
    }
    if (node.is_map()) {
        return "mapping";
    }
    if (node.is_seq()) {
        return "sequence";
    }
    if (!node.has_val()) {
        return "non-scalar value";
    }
    return "'" + scalarText(node) + "'";
}

void warnInvalid(const std::filesystem::path& path,
                 std::string_view preferencePath,
                 std::string_view expectation,
                 ryml::ConstNodeRef value) {
    spdlog::warn(
        "Invalid user preference '{}' in '{}': expected {}, got {}; using shipped default",
        preferencePath,
        sourceName(path),
        expectation,
        describedValue(value));
}

void warnUnknownFields(ryml::ConstNodeRef node,
                       const std::filesystem::path& path,
                       std::string_view prefix,
                       std::initializer_list<std::string_view> known) {
    if (!node.readable() || !node.is_map()) {
        return;
    }
    for (const ryml::ConstNodeRef child : node.children()) {
        const std::string key = Util::toStdString(child.key());
        bool isKnown = false;
        for (const std::string_view candidate : known) {
            if (key == candidate) {
                isKnown = true;
                break;
            }
        }
        if (isKnown) {
            continue;
        }
        const std::string fullPath = prefix.empty()
            ? key
            : std::string(prefix) + "." + key;
        spdlog::warn(
            "Unknown user preference '{}' in '{}'; ignored",
            fullPath,
            sourceName(path));
    }
}

template <typename Integer>
std::optional<Integer> parseInteger(ryml::ConstNodeRef node) {
    if (!node.readable() || !node.has_val() || node.is_container() ||
        node.is_val_quoted()) {
        return std::nullopt;
    }
    const std::string text = scalarText(node);
    Integer value{};
    const auto parsed = std::from_chars(
        text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

std::optional<double> parseFiniteDouble(ryml::ConstNodeRef node) {
    if (!node.readable() || !node.has_val() || node.is_container() ||
        node.is_val_quoted()) {
        return std::nullopt;
    }
    const std::string text = scalarText(node);
    double value = 0.0;
    const auto parsed = std::from_chars(
        text.data(), text.data() + text.size(), value,
        std::chars_format::general);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
        !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

std::optional<bool> parseBool(ryml::ConstNodeRef node) {
    if (!node.readable() || !node.has_val() || node.is_container() ||
        node.is_val_quoted()) {
        return std::nullopt;
    }
    const std::string value = scalarText(node);
    if (value == "true") {
        return true;
    }
    if (value == "false") {
        return false;
    }
    return std::nullopt;
}

bool validBindingToken(std::string_view token) {
    if (token.empty() || token.size() > kMaximumBindingTokenBytes) {
        return false;
    }
    for (const unsigned char byte : token) {
        if ((byte >= 'A' && byte <= 'Z') ||
            (byte >= '0' && byte <= '9') || byte == '_') {
            continue;
        }
        return false;
    }
    return true;
}

ryml::ConstNodeRef childOrInvalid(ryml::ConstNodeRef node,
                                  std::string_view key) {
    const ryml::csubstr yamlKey(key.data(), key.size());
    if (!node.readable() || !node.has_child(yamlKey)) {
        return {};
    }
    return node[yamlKey];
}

std::optional<ryml::ConstNodeRef> readSection(
    ryml::ConstNodeRef root,
    std::string_view sectionName,
    const std::filesystem::path& path) {
    const ryml::ConstNodeRef section = childOrInvalid(root, sectionName);
    if (!section.readable()) {
        return std::nullopt;
    }
    if (!section.is_map()) {
        spdlog::warn(
            "Invalid user preference section '{}' in '{}': expected mapping, got {}; using shipped defaults for section",
            sectionName,
            sourceName(path),
            describedValue(section));
        return std::nullopt;
    }
    return section;
}

void parseDisplay(ryml::ConstNodeRef root,
                  const std::filesystem::path& path,
                  UserPreferences& preferences) {
    const auto section = readSection(root, "display", path);
    if (!section) {
        return;
    }
    warnUnknownFields(
        *section, path, "display",
        {"mode", "windowed_size", "vsync", "fps_limit"});

    if (const auto node = childOrInvalid(*section, "mode"); node.readable()) {
        const std::string value = scalarText(node);
        if (node.has_val() && !node.is_container() && value == "windowed") {
            preferences.display.mode = DisplayMode::Windowed;
        } else if (node.has_val() && !node.is_container() &&
                   value == "borderless") {
            preferences.display.mode = DisplayMode::Borderless;
        } else {
            warnInvalid(path, "display.mode", "'windowed' or 'borderless'", node);
        }
    }

    if (const auto node = childOrInvalid(*section, "windowed_size");
        node.readable()) {
        bool valid = node.is_seq() && node.num_children() == 2;
        int width = 0;
        int height = 0;
        if (valid) {
            const auto parsedWidth = parseInteger<int64_t>(node[0]);
            const auto parsedHeight = parseInteger<int64_t>(node[1]);
            valid = parsedWidth && parsedHeight &&
                *parsedWidth >= kMinimumWindowDimension &&
                *parsedWidth <= kMaximumWindowDimension &&
                *parsedHeight >= kMinimumWindowDimension &&
                *parsedHeight <= kMaximumWindowDimension;
            if (valid) {
                width = static_cast<int>(*parsedWidth);
                height = static_cast<int>(*parsedHeight);
            }
        }
        if (valid) {
            preferences.display.windowedSize = WindowedSize{width, height};
        } else {
            warnInvalid(
                path,
                "display.windowed_size",
                "two integers in [1, 16384]",
                node);
        }
    }

    if (const auto node = childOrInvalid(*section, "vsync"); node.readable()) {
        if (const auto value = parseBool(node)) {
            preferences.display.vsync = *value;
        } else {
            warnInvalid(path, "display.vsync", "boolean", node);
        }
    }

    if (const auto node = childOrInvalid(*section, "fps_limit");
        node.readable()) {
        const std::string value = scalarText(node);
        if (node.has_val() && !node.is_container() && value == "unlimited") {
            preferences.display.fpsLimit.reset();
        } else if (const auto parsed = parseInteger<int64_t>(node);
                   parsed && *parsed >= kMinimumFpsLimit &&
                   *parsed <= kMaximumFpsLimit) {
            preferences.display.fpsLimit = static_cast<int>(*parsed);
        } else {
            warnInvalid(
                path,
                "display.fps_limit",
                "'unlimited' or integer in [30, 1000]",
                node);
        }
    }
}

void parseGraphics(ryml::ConstNodeRef root,
                   const std::filesystem::path& path,
                   UserPreferences& preferences) {
    const auto section = readSection(root, "graphics", path);
    if (!section) {
        return;
    }
    warnUnknownFields(
        *section, path, "graphics", {"view_distance_chunks", "shadows"});

    if (const auto node = childOrInvalid(*section, "view_distance_chunks");
        node.readable()) {
        const auto value = parseInteger<int64_t>(node);
        if (value && *value >= kMinimumViewDistanceChunks &&
            *value <= kMaximumViewDistanceChunks) {
            preferences.graphics.viewDistanceChunks = static_cast<int>(*value);
        } else {
            warnInvalid(
                path,
                "graphics.view_distance_chunks",
                "integer in [2, 16]",
                node);
        }
    }

    if (const auto node = childOrInvalid(*section, "shadows");
        node.readable()) {
        if (const auto value = parseBool(node)) {
            preferences.graphics.shadows = *value;
        } else {
            warnInvalid(path, "graphics.shadows", "boolean", node);
        }
    }
}

void parseCamera(ryml::ConstNodeRef root,
                 const std::filesystem::path& path,
                 UserPreferences& preferences) {
    const auto section = readSection(root, "camera", path);
    if (!section) {
        return;
    }
    warnUnknownFields(*section, path, "camera", {"vertical_fov_degrees"});

    if (const auto node = childOrInvalid(*section, "vertical_fov_degrees");
        node.readable()) {
        const auto value = parseFiniteDouble(node);
        if (value && *value >= kMinimumVerticalFovDegrees &&
            *value <= kMaximumVerticalFovDegrees) {
            preferences.camera.verticalFovDegrees = *value;
        } else {
            warnInvalid(
                path,
                "camera.vertical_fov_degrees",
                "finite number in [50, 110]",
                node);
        }
    }
}

void parseBindings(ryml::ConstNodeRef bindings,
                   const std::filesystem::path& path,
                   UserPreferences& preferences) {
    if (!bindings.is_map()) {
        warnInvalid(path, "input.bindings", "mapping", bindings);
        return;
    }
    for (const ryml::ConstNodeRef entry : bindings.children()) {
        const std::string actionName = Util::toStdString(entry.key());
        const auto action = parseUserAction(actionName);
        if (!action) {
            spdlog::warn(
                "Unknown user preference action '{}' in '{}'; ignored",
                actionName,
                sourceName(path));
            continue;
        }

        bool valid = entry.is_seq() &&
            entry.num_children() <= kMaximumBindingsPerAction;
        std::vector<std::string> tokens;
        if (valid) {
            tokens.reserve(entry.num_children());
            for (const ryml::ConstNodeRef tokenNode : entry.children()) {
                const std::string token = scalarText(tokenNode);
                const bool stringTyped = tokenNode.has_val() &&
                    !tokenNode.is_container() &&
                    (tokenNode.is_val_quoted() ||
                     (token != "true" && token != "false" &&
                      token != "null" && token != "~"));
                if (!stringTyped || !validBindingToken(token)) {
                    valid = false;
                    break;
                }
                tokens.push_back(token);
            }
        }
        if (valid) {
            preferences.input.bindings[*action] = std::move(tokens);
        } else {
            warnInvalid(
                path,
                "input.bindings." + actionName,
                "list of at most 8 keyboard/mouse string tokens",
                entry);
        }
    }
}

void parseInput(ryml::ConstNodeRef root,
                const std::filesystem::path& path,
                UserPreferences& preferences) {
    const auto section = readSection(root, "input", path);
    if (!section) {
        return;
    }
    warnUnknownFields(
        *section, path, "input",
        {"mouse_sensitivity", "invert_y", "bindings"});

    if (const auto node = childOrInvalid(*section, "mouse_sensitivity");
        node.readable()) {
        const auto value = parseFiniteDouble(node);
        if (value && *value >= kMinimumMouseSensitivity &&
            *value <= kMaximumMouseSensitivity) {
            preferences.input.mouseSensitivity = *value;
        } else {
            warnInvalid(
                path,
                "input.mouse_sensitivity",
                "finite number in [0.01, 1]",
                node);
        }
    }

    if (const auto node = childOrInvalid(*section, "invert_y");
        node.readable()) {
        if (const auto value = parseBool(node)) {
            preferences.input.invertY = *value;
        } else {
            warnInvalid(path, "input.invert_y", "boolean", node);
        }
    }

    if (const auto node = childOrInvalid(*section, "bindings");
        node.readable()) {
        parseBindings(node, path, preferences);
    }
}

UserPreferences parseSupportedPreferences(ryml::ConstNodeRef root,
                                          const std::filesystem::path& path) {
    UserPreferences preferences;
    warnUnknownFields(
        root, path, {},
        {"schema_version", "display", "graphics", "camera", "input"});
    parseDisplay(root, path, preferences);
    parseGraphics(root, path, preferences);
    parseCamera(root, path, preferences);
    parseInput(root, path, preferences);
    return preferences;
}

DocumentInspection inspectDocument(const std::filesystem::path& path) {
    std::error_code statusError;
    const std::filesystem::file_status status =
        std::filesystem::symlink_status(path, statusError);
    if (statusError) {
        if (statusError == std::errc::no_such_file_or_directory) {
            return {DocumentKind::Missing, {}, nullptr, {}};
        }
        return {DocumentKind::Unsafe, {}, nullptr,
                "file status could not be read: " + statusError.message()};
    }
    if (status.type() == std::filesystem::file_type::not_found) {
        return {DocumentKind::Missing, {}, nullptr, {}};
    }
    if (!std::filesystem::is_regular_file(status)) {
        return {DocumentKind::Unsafe, {}, nullptr,
                "path is not a regular file"};
    }

    std::error_code sizeError;
    const uintmax_t size = std::filesystem::file_size(path, sizeError);
    if (sizeError) {
        return {DocumentKind::Unsafe, {}, nullptr,
                "file size could not be read: " + sizeError.message()};
    }
    if (size > kMaximumUserPreferencesBytes) {
        return {DocumentKind::Unsafe, {}, nullptr,
                "file exceeds the 262144-byte limit"};
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return {DocumentKind::Unsafe, {}, nullptr, "file is unreadable"};
    }
    std::string document(static_cast<size_t>(size), '\0');
    if (size != 0) {
        stream.read(document.data(), static_cast<std::streamsize>(size));
    }
    if (!stream || stream.peek() != std::char_traits<char>::eof()) {
        return {DocumentKind::Unsafe, {}, nullptr,
                "file changed or could not be read completely"};
    }

    std::unique_ptr<ryml::Tree> tree;
    try {
        const std::string filename = path.string();
        tree = parseYaml(document, filename);
    } catch (const std::exception& error) {
        return {DocumentKind::Unsafe, {}, nullptr,
                "YAML is malformed: " + std::string(error.what())};
    }

    const ryml::ConstNodeRef root = tree->rootref();
    if (!root.readable() || !root.is_map()) {
        return {DocumentKind::Unsafe, {}, nullptr,
                "document root is not a mapping"};
    }
    const ryml::ConstNodeRef schemaNode = childOrInvalid(root, "schema_version");
    if (!schemaNode.readable()) {
        return {DocumentKind::Unsafe, {}, nullptr,
                "schema_version is missing"};
    }
    const auto schemaVersion = parseInteger<int64_t>(schemaNode);
    if (!schemaVersion || *schemaVersion < 0 ||
        *schemaVersion > std::numeric_limits<uint32_t>::max()) {
        return {DocumentKind::Unsafe, {}, nullptr,
                "schema_version is not an unsigned integer"};
    }
    if (*schemaVersion != kUserPreferencesSchemaVersion) {
        return {DocumentKind::Unsafe, {}, nullptr,
                "schema_version " + std::to_string(*schemaVersion) +
                    " is not supported"};
    }

    UserPreferences preferences = parseSupportedPreferences(root, path);
    return {DocumentKind::Supported,
            std::move(preferences),
            std::move(tree),
            {}};
}

void validatePreferences(const UserPreferences& preferences) {
    if (preferences.schemaVersion != kUserPreferencesSchemaVersion) {
        throw std::invalid_argument("Unsupported user preferences schema version");
    }
    if (preferences.display.mode != DisplayMode::Windowed &&
        preferences.display.mode != DisplayMode::Borderless) {
        throw std::invalid_argument("Invalid requested display mode");
    }
    if (preferences.display.windowedSize.width < kMinimumWindowDimension ||
        preferences.display.windowedSize.width > kMaximumWindowDimension ||
        preferences.display.windowedSize.height < kMinimumWindowDimension ||
        preferences.display.windowedSize.height > kMaximumWindowDimension) {
        throw std::invalid_argument("Requested windowed size is out of range");
    }
    if (preferences.display.fpsLimit &&
        (*preferences.display.fpsLimit < kMinimumFpsLimit ||
         *preferences.display.fpsLimit > kMaximumFpsLimit)) {
        throw std::invalid_argument("Requested FPS limit is out of range");
    }
    if (preferences.graphics.viewDistanceChunks <
            kMinimumViewDistanceChunks ||
        preferences.graphics.viewDistanceChunks >
            kMaximumViewDistanceChunks) {
        throw std::invalid_argument("Requested view distance is out of range");
    }
    if (!std::isfinite(preferences.camera.verticalFovDegrees) ||
        preferences.camera.verticalFovDegrees < kMinimumVerticalFovDegrees ||
        preferences.camera.verticalFovDegrees > kMaximumVerticalFovDegrees) {
        throw std::invalid_argument("Requested vertical FOV is out of range");
    }
    if (!std::isfinite(preferences.input.mouseSensitivity) ||
        preferences.input.mouseSensitivity < kMinimumMouseSensitivity ||
        preferences.input.mouseSensitivity > kMaximumMouseSensitivity) {
        throw std::invalid_argument("Requested mouse sensitivity is out of range");
    }
    for (const auto& [action, tokens] : preferences.input.bindings) {
        static_cast<void>(userActionName(action));
        if (tokens.size() > kMaximumBindingsPerAction) {
            throw std::invalid_argument(
                "Requested input binding list is too long");
        }
        for (const std::string& token : tokens) {
            if (!validBindingToken(token)) {
                throw std::invalid_argument(
                    "Requested input binding token is invalid");
            }
        }
    }
}

std::string formatDouble(double value) {
    std::array<char, 64> buffer{};
    const auto result = std::to_chars(
        buffer.data(), buffer.data() + buffer.size(), value,
        std::chars_format::general,
        std::numeric_limits<double>::max_digits10);
    if (result.ec != std::errc{}) {
        throw std::runtime_error("Failed to serialize user preference number");
    }
    return std::string(buffer.data(), result.ptr);
}

void setScalar(ryml::NodeRef parent,
               std::string_view key,
               std::string_view value,
               bool quoted = false) {
    const ryml::csubstr yamlKey(key.data(), key.size());
    bool found = false;
    for (ryml::id_type position = 0;
         position < parent.num_children();) {
        const ryml::NodeRef child = parent[position];
        if (child.key() != yamlKey || !found) {
            found = found || child.key() == yamlKey;
            ++position;
            continue;
        }
        parent.remove_child(position);
    }
    ryml::NodeRef node = parent[yamlKey];
    node.clear_children();
    node.set_type(ryml::KEYVAL);
    node.set_val(node.to_arena(ryml::csubstr(value.data(), value.size())));
    node.set_val_style(quoted ? ryml::VAL_DQUO : ryml::VAL_PLAIN);
}

ryml::NodeRef ensureMap(ryml::NodeRef parent, std::string_view key) {
    const ryml::csubstr yamlKey(key.data(), key.size());
    bool found = false;
    for (ryml::id_type position = 0;
         position < parent.num_children();) {
        const ryml::NodeRef child = parent[position];
        if (child.key() != yamlKey || !found) {
            found = found || child.key() == yamlKey;
            ++position;
            continue;
        }
        parent.remove_child(position);
    }
    ryml::NodeRef node = parent[yamlKey];
    if (!node.readable() || !node.is_map()) {
        node.clear_children();
        node.set_type(ryml::KEYMAP);
    }
    return node;
}

void setWindowedSize(ryml::NodeRef display, const WindowedSize& size) {
    ryml::NodeRef node = display["windowed_size"];
    node.clear_children();
    node.set_type(ryml::KEYSEQ);
    for (const int dimension : {size.width, size.height}) {
        ryml::NodeRef child = node.append_child();
        child.set_type(ryml::VAL);
        const std::string value = std::to_string(dimension);
        child.set_val(child.to_arena(value));
        child.set_val_style(ryml::VAL_PLAIN);
    }
    node.set_container_style(ryml::FLOW_SL);
}

void setBindings(ryml::NodeRef input, const InputPreferences& preferences) {
    ryml::NodeRef bindings = ensureMap(input, "bindings");
    for (const auto& [action, name] : kUserActionNames) {
        static_cast<void>(action);
        const ryml::csubstr key(name.data(), name.size());
        while (bindings.has_child(key)) {
            bindings.remove_child(key);
        }
    }

    for (const auto& [action, tokens] : preferences.bindings) {
        const std::string_view name = userActionName(action);
        ryml::NodeRef entry = bindings[ryml::csubstr(name.data(), name.size())];
        entry.set_type(ryml::KEYSEQ);
        entry.set_container_style(ryml::FLOW_SL);
        for (const std::string& token : tokens) {
            ryml::NodeRef child = entry.append_child();
            child.set_type(ryml::VAL);
            child.set_val(child.to_arena(token));
            child.set_val_style(ryml::VAL_DQUO);
        }
    }
}

std::string serializePreferences(UserPreferences preferences,
                                 std::unique_ptr<ryml::Tree> retainedTree) {
    validatePreferences(preferences);
    if (!retainedTree) {
        retainedTree = parseYaml(
            "schema_version: 1\n", "user-preferences.yaml");
    }

    ryml::NodeRef root = retainedTree->rootref();
    setScalar(root, "schema_version", "1");

    ryml::NodeRef display = ensureMap(root, "display");
    setScalar(
        display,
        "mode",
        preferences.display.mode == DisplayMode::Windowed
            ? "windowed"
            : "borderless");
    setWindowedSize(display, preferences.display.windowedSize);
    setScalar(display, "vsync", preferences.display.vsync ? "true" : "false");
    setScalar(
        display,
        "fps_limit",
        preferences.display.fpsLimit
            ? std::to_string(*preferences.display.fpsLimit)
            : "unlimited");

    ryml::NodeRef graphics = ensureMap(root, "graphics");
    setScalar(
        graphics,
        "view_distance_chunks",
        std::to_string(preferences.graphics.viewDistanceChunks));
    setScalar(
        graphics,
        "shadows",
        preferences.graphics.shadows ? "true" : "false");

    ryml::NodeRef camera = ensureMap(root, "camera");
    setScalar(
        camera,
        "vertical_fov_degrees",
        formatDouble(preferences.camera.verticalFovDegrees));

    ryml::NodeRef input = ensureMap(root, "input");
    setScalar(
        input,
        "mouse_sensitivity",
        formatDouble(preferences.input.mouseSensitivity));
    setScalar(input, "invert_y", preferences.input.invertY ? "true" : "false");
    setBindings(input, preferences.input);

    return ryml::emitrs_yaml<std::string>(*retainedTree);
}

void writeAtomically(const std::filesystem::path& path,
                     const std::string& document) {
    try {
        Persistence::FilesystemBackend storage;
        auto session = storage.openWrite(path.string());
        session->writer().writeBytes(
            reinterpret_cast<const uint8_t*>(document.data()), document.size());
        session->commit();
    } catch (const Persistence::AtomicFilePublicationError&) {
        throw;
    } catch (const std::exception& error) {
        throw Persistence::AtomicFilePublicationError(
            Persistence::AtomicFilePublicationState::NotPublished,
            "Failed to stage atomic write to " + path.string() + ": " +
                error.what());
    }
}

[[noreturn]] void throwWriteBlocked(const std::filesystem::path& path,
                                    std::string_view problem) {
    throw UserPreferencesWriteBlocked(
        "Cannot save user preferences to '" + sourceName(path) +
        "': " + std::string(problem) +
        "; call replaceWithRequested() to explicitly replace the file");
}

} // namespace

std::filesystem::path currentUserPreferencesPath() {
#ifdef _WIN32
    if (const char* localAppData = std::getenv("LOCALAPPDATA")) {
        const std::filesystem::path root(localAppData);
        if (root.is_absolute()) {
            return root / "Rigel" / "user-preferences.yaml";
        }
    }
    throw std::runtime_error(
        "Cannot resolve an absolute user preferences path from LOCALAPPDATA");
#else
    if (const char* xdgConfigHome = std::getenv("XDG_CONFIG_HOME")) {
        const std::filesystem::path root(xdgConfigHome);
        if (!root.empty() && root.is_absolute()) {
            return root / "rigel" / "user-preferences.yaml";
        }
    }
    if (const char* home = std::getenv("HOME")) {
        const std::filesystem::path root(home);
        if (!root.empty() && root.is_absolute()) {
            return root / ".config" / "rigel" / "user-preferences.yaml";
        }
    }
    throw std::runtime_error(
        "Cannot resolve an absolute user preferences path from XDG_CONFIG_HOME or HOME");
#endif
}

UserPreferencesStore::UserPreferencesStore(std::filesystem::path path)
    : m_path(std::move(path)) {
    if (m_path.empty() || !m_path.is_absolute()) {
        throw std::invalid_argument(
            "UserPreferencesStore requires one explicit absolute file path");
    }
}

UserPreferencesStore UserPreferencesStore::forCurrentUser() {
    return UserPreferencesStore(currentUserPreferencesPath());
}

UserPreferencesState UserPreferencesStore::load() {
    DocumentInspection inspection = inspectDocument(m_path);
    if (inspection.kind == DocumentKind::Missing) {
        m_normalSaveBlocked = false;
        return {};
    }
    if (inspection.kind == DocumentKind::Unsafe) {
        m_normalSaveBlocked = true;
        spdlog::warn(
            "User preferences file '{}' cannot be loaded: {}; using shipped defaults and preserving the file",
            sourceName(m_path),
            inspection.problem);
        return {};
    }

    m_normalSaveBlocked = false;
    return {inspection.preferences, inspection.preferences};
}

void UserPreferencesStore::saveRequested(const UserPreferences& requested) {
    validatePreferences(requested);
    if (m_normalSaveBlocked) {
        throwWriteBlocked(m_path, "normal saves are blocked after an unsafe load");
    }

    DocumentInspection current = inspectDocument(m_path);
    if (current.kind == DocumentKind::Unsafe) {
        m_normalSaveBlocked = true;
        throwWriteBlocked(m_path, current.problem);
    }

    const std::string document = serializePreferences(
        requested,
        current.kind == DocumentKind::Supported
            ? std::move(current.tree)
            : nullptr);
    try {
        writeAtomically(m_path, document);
    } catch (const Persistence::AtomicFilePublicationError& error) {
        if (error.state() ==
            Persistence::AtomicFilePublicationState::PublishedDurabilityUncertain) {
            m_normalSaveBlocked = false;
        }
        throw;
    }
}

void UserPreferencesStore::saveRequested(const UserPreferencesState& state) {
    saveRequested(state.requested);
}

void UserPreferencesStore::replaceWithRequested(
    const UserPreferences& requested) {
    const std::string document = serializePreferences(requested, nullptr);
    try {
        writeAtomically(m_path, document);
        m_normalSaveBlocked = false;
    } catch (const Persistence::AtomicFilePublicationError& error) {
        if (error.state() ==
            Persistence::AtomicFilePublicationState::PublishedDurabilityUncertain) {
            m_normalSaveBlocked = false;
        }
        throw;
    }
}

} // namespace Rigel::Preferences
