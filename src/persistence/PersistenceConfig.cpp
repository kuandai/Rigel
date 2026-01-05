#include "Rigel/Persistence/PersistenceConfig.h"

#include "Rigel/Util/Ryml.h"
#include "Rigel/Util/Yaml.h"

#include <algorithm>
#include <cctype>
#include <ryml.hpp>
#include <ryml_std.hpp>

namespace Rigel::Persistence {
namespace {

std::string toLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool parseBoolValue(const std::string& value, bool fallback) {
    std::string lowered = toLower(value);
    if (lowered == "true" || lowered == "yes" || lowered == "1") {
        return true;
    }
    if (lowered == "false" || lowered == "no" || lowered == "0") {
        return false;
    }
    return fallback;
}

ProviderConfig* findProviderMutable(PersistenceConfig& config, std::string_view id) {
    for (auto& provider : config.providers) {
        if (provider.id == id) {
            return &provider;
        }
    }
    return nullptr;
}

} // namespace

bool ProviderConfig::getBool(std::string_view key, bool fallback) const {
    auto it = options.find(std::string(key));
    if (it == options.end()) {
        return fallback;
    }
    return parseBoolValue(it->second, fallback);
}

std::string ProviderConfig::getString(std::string_view key, const std::string& fallback) const {
    auto it = options.find(std::string(key));
    if (it == options.end()) {
        return fallback;
    }
    return it->second;
}

const ProviderConfig* PersistenceConfig::findProvider(std::string_view id) const {
    for (const auto& provider : providers) {
        if (provider.id == id) {
            return &provider;
        }
    }
    return nullptr;
}

void PersistenceConfig::applyYaml(const char* sourceName, const std::string& yaml) {
    if (yaml.empty()) {
        return;
    }

    ryml::Tree tree = ryml::parse_in_arena(
        ryml::to_csubstr(sourceName),
        ryml::to_csubstr(yaml)
    );
    ryml::ConstNodeRef root = tree.rootref();
    ryml::ConstNodeRef persistenceNode = root;
    if (root.has_child("persistence")) {
        persistenceNode = root["persistence"];
    }
    if (!persistenceNode.readable()) {
        return;
    }

    format = Util::readString(persistenceNode, "format", format);

    if (persistenceNode.has_child("providers")) {
        ryml::ConstNodeRef providersNode = persistenceNode["providers"];
        if (providersNode.is_map()) {
            for (ryml::ConstNodeRef providerNode : providersNode.children()) {
                std::string id = Util::toStdString(providerNode.key());
                if (id.empty()) {
                    continue;
                }
                ProviderConfig* provider = findProviderMutable(*this, id);
                if (!provider) {
                    providers.push_back(ProviderConfig{ id, {} });
                    provider = &providers.back();
                }
                if (providerNode.is_map()) {
                    for (ryml::ConstNodeRef optionNode : providerNode.children()) {
                        std::string key = Util::toStdString(optionNode.key());
                        std::string value;
                        if (optionNode.has_val()) {
                            optionNode >> value;
                        }
                        if (!key.empty()) {
                            provider->options[key] = value;
                        }
                    }
                } else if (providerNode.has_val()) {
                    std::string value;
                    providerNode >> value;
                    provider->options["value"] = value;
                }
            }
        }
    }
}

} // namespace Rigel::Persistence
