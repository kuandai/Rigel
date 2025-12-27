#include "Rigel/input/InputBindingsLoader.h"
#include "Rigel/input/InputBindings.h"

#include <GLFW/glfw3.h>
#include <spdlog/spdlog.h>
#include <cctype>
#include <string>
#include <unordered_map>

namespace Rigel {

namespace {
std::string normalizeKeyName(std::string_view name) {
    std::string out;
    out.reserve(name.size());
    for (char ch : name) {
        if (std::isspace(static_cast<unsigned char>(ch)) || ch == '-') {
            out.push_back('_');
        } else {
            out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
        }
    }
    while (!out.empty() && out.front() == '_') {
        out.erase(out.begin());
    }
    while (!out.empty() && out.back() == '_') {
        out.pop_back();
    }
    return out;
}

bool isDigits(std::string_view value) {
    if (value.empty()) {
        return false;
    }
    for (char ch : value) {
        if (!std::isdigit(static_cast<unsigned char>(ch))) {
            return false;
        }
    }
    return true;
}

std::optional<int> parseKeyName(std::string_view name) {
    std::string normalized = normalizeKeyName(name);
    if (normalized.empty()) {
        return std::nullopt;
    }
    if (normalized == "NONE" || normalized == "UNBOUND" ||
        normalized == "NULL" || normalized == "~") {
        return std::nullopt;
    }

    if (normalized.size() == 1) {
        char ch = normalized[0];
        if (ch >= 'A' && ch <= 'Z') {
            return GLFW_KEY_A + (ch - 'A');
        }
        if (ch >= '0' && ch <= '9') {
            return GLFW_KEY_0 + (ch - '0');
        }
    }

    if (normalized.size() >= 2 && normalized[0] == 'F') {
        std::string_view number = std::string_view(normalized).substr(1);
        if (isDigits(number)) {
            int fn = std::stoi(std::string(number));
            if (fn >= 1 && fn <= 25) {
                return GLFW_KEY_F1 + (fn - 1);
            }
        }
    }

    static const std::unordered_map<std::string, int> kKeyMap = {
        {"SPACE", GLFW_KEY_SPACE},
        {"TAB", GLFW_KEY_TAB},
        {"ENTER", GLFW_KEY_ENTER},
        {"RETURN", GLFW_KEY_ENTER},
        {"ESC", GLFW_KEY_ESCAPE},
        {"ESCAPE", GLFW_KEY_ESCAPE},
        {"BACKSPACE", GLFW_KEY_BACKSPACE},
        {"INSERT", GLFW_KEY_INSERT},
        {"DELETE", GLFW_KEY_DELETE},
        {"HOME", GLFW_KEY_HOME},
        {"END", GLFW_KEY_END},
        {"PAGE_UP", GLFW_KEY_PAGE_UP},
        {"PAGE_DOWN", GLFW_KEY_PAGE_DOWN},
        {"UP", GLFW_KEY_UP},
        {"DOWN", GLFW_KEY_DOWN},
        {"LEFT", GLFW_KEY_LEFT},
        {"RIGHT", GLFW_KEY_RIGHT},
        {"CAPS_LOCK", GLFW_KEY_CAPS_LOCK},
        {"SCROLL_LOCK", GLFW_KEY_SCROLL_LOCK},
        {"NUM_LOCK", GLFW_KEY_NUM_LOCK},
        {"PRINT_SCREEN", GLFW_KEY_PRINT_SCREEN},
        {"PAUSE", GLFW_KEY_PAUSE},
        {"LSHIFT", GLFW_KEY_LEFT_SHIFT},
        {"LEFT_SHIFT", GLFW_KEY_LEFT_SHIFT},
        {"RSHIFT", GLFW_KEY_RIGHT_SHIFT},
        {"RIGHT_SHIFT", GLFW_KEY_RIGHT_SHIFT},
        {"LCTRL", GLFW_KEY_LEFT_CONTROL},
        {"LEFT_CTRL", GLFW_KEY_LEFT_CONTROL},
        {"LEFT_CONTROL", GLFW_KEY_LEFT_CONTROL},
        {"RCTRL", GLFW_KEY_RIGHT_CONTROL},
        {"RIGHT_CTRL", GLFW_KEY_RIGHT_CONTROL},
        {"RIGHT_CONTROL", GLFW_KEY_RIGHT_CONTROL},
        {"LALT", GLFW_KEY_LEFT_ALT},
        {"LEFT_ALT", GLFW_KEY_LEFT_ALT},
        {"RALT", GLFW_KEY_RIGHT_ALT},
        {"RIGHT_ALT", GLFW_KEY_RIGHT_ALT},
        {"LSUPER", GLFW_KEY_LEFT_SUPER},
        {"LEFT_SUPER", GLFW_KEY_LEFT_SUPER},
        {"RSUPER", GLFW_KEY_RIGHT_SUPER},
        {"RIGHT_SUPER", GLFW_KEY_RIGHT_SUPER}
    };

    auto it = kKeyMap.find(normalized);
    if (it != kKeyMap.end()) {
        return it->second;
    }

    if (isDigits(normalized)) {
        int key = std::stoi(normalized);
        if (key >= 0 && key <= GLFW_KEY_LAST) {
            return key;
        }
    }

    return std::nullopt;
}
} // namespace

std::shared_ptr<Asset::AssetBase> InputBindingsLoader::load(const Asset::LoadContext& ctx) {
    auto bindings = std::make_shared<InputBindings>();

    if (!ctx.config.readable() || !ctx.config.has_child("bindings")) {
        return bindings;
    }

    ryml::ConstNodeRef bindingsNode = ctx.config["bindings"];
    if (!bindingsNode.is_map()) {
        spdlog::warn("Input bindings '{}' must be a map", ctx.id);
        return bindings;
    }

    for (ryml::ConstNodeRef entry : bindingsNode.children()) {
        if (!entry.is_keyval()) {
            continue;
        }
        std::string action(entry.key().data(), entry.key().size());
        std::string keyName;
        if (entry.has_val()) {
            keyName.assign(entry.val().data(), entry.val().size());
        }

        std::string normalized = normalizeKeyName(keyName);
        std::optional<int> key = parseKeyName(keyName);
        if (!key && !keyName.empty() &&
            normalized != "NONE" &&
            normalized != "UNBOUND" &&
            normalized != "NULL" &&
            normalized != "~") {
            spdlog::warn("Input binding '{}' has unknown key '{}'", action, keyName);
        }
        bindings->setBinding(action, key);
    }

    return bindings;
}

} // namespace Rigel
