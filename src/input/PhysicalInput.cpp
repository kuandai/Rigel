#include "Rigel/input/PhysicalInput.h"

#include <GLFW/glfw3.h>

#include <array>
#include <cctype>
#include <charconv>
#include <string>

namespace Rigel::Input {
namespace {

std::string normalizeToken(std::string_view token) {
    if (token.empty() || token.size() > kMaximumBindingTokenBytes) {
        return {};
    }

    std::string normalized;
    normalized.reserve(token.size());
    for (const unsigned char byte : token) {
        if (std::isspace(byte) || byte == '-') {
            normalized.push_back('_');
        } else {
            normalized.push_back(
                static_cast<char>(std::toupper(byte)));
        }
    }
    while (!normalized.empty() && normalized.front() == '_') {
        normalized.erase(normalized.begin());
    }
    while (!normalized.empty() && normalized.back() == '_') {
        normalized.pop_back();
    }
    return normalized;
}

std::optional<int> decodeFunctionKey(std::string_view token) {
    if (token.size() < 2 || token.front() != 'F') {
        return std::nullopt;
    }
    int number = 0;
    const auto result = std::from_chars(
        token.data() + 1, token.data() + token.size(), number);
    if (result.ec != std::errc{} ||
        result.ptr != token.data() + token.size() ||
        number < 1 || number > 25) {
        return std::nullopt;
    }
    return GLFW_KEY_F1 + number - 1;
}

struct NamedKey {
    std::string_view name;
    int code;
};

constexpr std::array kNamedKeys{
    NamedKey{"SPACE", GLFW_KEY_SPACE},
    NamedKey{"TAB", GLFW_KEY_TAB},
    NamedKey{"ENTER", GLFW_KEY_ENTER},
    NamedKey{"RETURN", GLFW_KEY_ENTER},
    NamedKey{"ESC", GLFW_KEY_ESCAPE},
    NamedKey{"ESCAPE", GLFW_KEY_ESCAPE},
    NamedKey{"BACKSPACE", GLFW_KEY_BACKSPACE},
    NamedKey{"INSERT", GLFW_KEY_INSERT},
    NamedKey{"DELETE", GLFW_KEY_DELETE},
    NamedKey{"HOME", GLFW_KEY_HOME},
    NamedKey{"END", GLFW_KEY_END},
    NamedKey{"PAGE_UP", GLFW_KEY_PAGE_UP},
    NamedKey{"PAGE_DOWN", GLFW_KEY_PAGE_DOWN},
    NamedKey{"UP", GLFW_KEY_UP},
    NamedKey{"DOWN", GLFW_KEY_DOWN},
    NamedKey{"LEFT", GLFW_KEY_LEFT},
    NamedKey{"RIGHT", GLFW_KEY_RIGHT},
    NamedKey{"CAPS_LOCK", GLFW_KEY_CAPS_LOCK},
    NamedKey{"SCROLL_LOCK", GLFW_KEY_SCROLL_LOCK},
    NamedKey{"NUM_LOCK", GLFW_KEY_NUM_LOCK},
    NamedKey{"PRINT_SCREEN", GLFW_KEY_PRINT_SCREEN},
    NamedKey{"PAUSE", GLFW_KEY_PAUSE},
    NamedKey{"LSHIFT", GLFW_KEY_LEFT_SHIFT},
    NamedKey{"LEFT_SHIFT", GLFW_KEY_LEFT_SHIFT},
    NamedKey{"RSHIFT", GLFW_KEY_RIGHT_SHIFT},
    NamedKey{"RIGHT_SHIFT", GLFW_KEY_RIGHT_SHIFT},
    NamedKey{"LCTRL", GLFW_KEY_LEFT_CONTROL},
    NamedKey{"LEFT_CTRL", GLFW_KEY_LEFT_CONTROL},
    NamedKey{"LEFT_CONTROL", GLFW_KEY_LEFT_CONTROL},
    NamedKey{"RCTRL", GLFW_KEY_RIGHT_CONTROL},
    NamedKey{"RIGHT_CTRL", GLFW_KEY_RIGHT_CONTROL},
    NamedKey{"RIGHT_CONTROL", GLFW_KEY_RIGHT_CONTROL},
    NamedKey{"LALT", GLFW_KEY_LEFT_ALT},
    NamedKey{"LEFT_ALT", GLFW_KEY_LEFT_ALT},
    NamedKey{"RALT", GLFW_KEY_RIGHT_ALT},
    NamedKey{"RIGHT_ALT", GLFW_KEY_RIGHT_ALT},
    NamedKey{"LSUPER", GLFW_KEY_LEFT_SUPER},
    NamedKey{"LEFT_SUPER", GLFW_KEY_LEFT_SUPER},
    NamedKey{"RSUPER", GLFW_KEY_RIGHT_SUPER},
    NamedKey{"RIGHT_SUPER", GLFW_KEY_RIGHT_SUPER},
};

std::optional<int> decodeNamedKey(std::string_view token) {
    for (const auto& key : kNamedKeys) {
        if (token == key.name) {
            return key.code;
        }
    }
    return std::nullopt;
}

std::optional<int> decodeMouseButton(std::string_view token) {
    if (token == "MOUSE_LEFT") {
        return GLFW_MOUSE_BUTTON_LEFT;
    }
    if (token == "MOUSE_RIGHT") {
        return GLFW_MOUSE_BUTTON_RIGHT;
    }
    if (token == "MOUSE_MIDDLE") {
        return GLFW_MOUSE_BUTTON_MIDDLE;
    }
    constexpr std::string_view prefix = "MOUSE_";
    if (!token.starts_with(prefix) || token.size() != prefix.size() + 1 ||
        token.back() < '4' || token.back() > '8') {
        return std::nullopt;
    }
    return GLFW_MOUSE_BUTTON_4 + token.back() - '4';
}

} // namespace

std::optional<PhysicalInput> decodeBindingToken(std::string_view token) {
    if (token.size() > 1 &&
        (token.front() == '-' || token.front() == '+')) {
        return std::nullopt;
    }
    const std::string normalized = normalizeToken(token);
    if (normalized.empty()) {
        return std::nullopt;
    }

    if (const auto button = decodeMouseButton(normalized)) {
        return PhysicalInput{PhysicalInputType::MouseButton, *button};
    }

    if (normalized.size() == 1) {
        const char value = normalized.front();
        if (value >= 'A' && value <= 'Z') {
            return PhysicalInput{
                PhysicalInputType::Keyboard,
                GLFW_KEY_A + value - 'A'};
        }
        if (value >= '0' && value <= '9') {
            return PhysicalInput{
                PhysicalInputType::Keyboard,
                GLFW_KEY_0 + value - '0'};
        }
    }

    if (const auto functionKey = decodeFunctionKey(normalized)) {
        return PhysicalInput{PhysicalInputType::Keyboard, *functionKey};
    }
    if (const auto namedKey = decodeNamedKey(normalized)) {
        return PhysicalInput{PhysicalInputType::Keyboard, *namedKey};
    }
    return std::nullopt;
}

} // namespace Rigel::Input
