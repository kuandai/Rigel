#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Rigel::Persistence {

struct ProviderConfig {
    std::string id;
    std::unordered_map<std::string, std::string> options;

    bool getBool(std::string_view key, bool fallback) const;
    std::string getString(std::string_view key, const std::string& fallback) const;
};

struct PersistenceConfig {
    std::string format = "cr";
    std::vector<ProviderConfig> providers;

    const ProviderConfig* findProvider(std::string_view id) const;
    void applyYaml(const char* sourceName, const std::string& yaml);
};

} // namespace Rigel::Persistence
