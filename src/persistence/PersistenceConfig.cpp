#include "Rigel/Persistence/PersistenceConfig.h"

#include "Rigel/Persistence/Backends/CR/CRSettings.h"
#include "Rigel/Util/Ryml.h"
#include "Rigel/Util/Yaml.h"

#include <ryml.hpp>
#include <ryml_std.hpp>
#include <utility>

namespace Rigel::Persistence {

void PersistenceConfig::applyYaml(const char* sourceName, const std::string& yaml) {
    PersistenceConfig candidate = *this;
    candidate.applyYamlUnchecked(sourceName, yaml);
    *this = std::move(candidate);
}

void PersistenceConfig::applyYamlUnchecked(
    const char* sourceName,
    const std::string& yaml) {
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
    if (root.has_child("persistence")) {
        Util::warnUnknownKeys(root, sourceName, "", {"persistence"});
    }
    Util::warnUnknownKeys(
        persistenceNode,
        sourceName,
        "persistence",
        {"format", "providers"}
    );

    format = Util::readString(persistenceNode, "format", format);

    if (persistenceNode.has_child("providers")) {
        ryml::ConstNodeRef providersNode = persistenceNode["providers"];
        if (providersNode.is_map()) {
            constexpr const char* crId =
                Backends::CR::kCRSettingsProviderId;
            Util::warnUnknownKeys(
                providersNode,
                sourceName,
                "persistence.providers",
                {crId});
            if (providersNode.has_child(crId)) {
                const ryml::ConstNodeRef crNode = providersNode[crId];
                Util::warnUnknownKeys(
                    crNode,
                    sourceName,
                    "persistence.providers.rigel:persistence.cr",
                    {"lz4"});
                crLz4Enabled = Util::readBool(
                    crNode,
                    "lz4",
                    crLz4Enabled,
                    sourceName,
                    "persistence.providers.rigel:persistence.cr");
            }
        }
    }
}

} // namespace Rigel::Persistence
