#pragma once

#include <string>

namespace Rigel::Persistence {

struct PersistenceConfig {
    std::string format = "cr";
    bool crLz4Enabled = false;

    void applyYaml(const char* sourceName, const std::string& yaml);

private:
    void applyYamlUnchecked(const char* sourceName, const std::string& yaml);
};

} // namespace Rigel::Persistence
