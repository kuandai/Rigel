#include "Rigel/Persistence/Providers.h"

#include "Rigel/Voxel/Block.h"
#include "Rigel/Voxel/BlockRegistry.h"

namespace Rigel::Persistence {

namespace {

std::optional<std::string> legacyAlias(std::string_view id) {
    constexpr std::string_view kLegacyNs = "rigel:";
    constexpr std::string_view kBaseNs = "base:";
    if (id.rfind(kLegacyNs, 0) == 0) {
        return std::string(kBaseNs) + std::string(id.substr(kLegacyNs.size()));
    }
    if (id.rfind(kBaseNs, 0) == 0) {
        return std::string(kLegacyNs) + std::string(id.substr(kBaseNs.size()));
    }
    return std::nullopt;
}

} // namespace

std::optional<Voxel::BlockID> BlockRegistryProvider::resolveRuntimeId(std::string_view externalId) const {
    if (!m_registry || externalId.empty()) {
        return std::nullopt;
    }

    std::string key(externalId);
    if (auto direct = m_registry->findByIdentifier(key)) {
        return direct;
    }

    if (auto alias = resolveAlias(externalId)) {
        if (auto aliased = m_registry->findByIdentifier(*alias)) {
            return aliased;
        }
    }

    if (auto legacy = legacyAlias(externalId)) {
        if (auto fallback = m_registry->findByIdentifier(*legacy)) {
            return fallback;
        }
    }

    return std::nullopt;
}

std::optional<std::string> BlockRegistryProvider::resolveExternalId(Voxel::BlockID runtimeId) const {
    if (!m_registry || runtimeId.type >= m_registry->size()) {
        return std::nullopt;
    }
    return m_registry->getType(runtimeId).identifier;
}

std::optional<std::string> BlockRegistryProvider::resolveAlias(std::string_view externalId) const {
    auto it = m_aliasToCanonical.find(std::string(externalId));
    if (it == m_aliasToCanonical.end()) {
        return std::nullopt;
    }
    return it->second;
}

Voxel::BlockID BlockRegistryProvider::placeholderRuntimeId() const {
    if (auto found = resolveRuntimeId(m_placeholderIdentifier)) {
        return *found;
    }
    return Voxel::BlockRegistry::airId();
}

void BlockRegistryProvider::addAlias(std::string externalId, std::string canonicalId) {
    if (externalId.empty() || canonicalId.empty()) {
        return;
    }
    m_aliasToCanonical[std::move(externalId)] = std::move(canonicalId);
}

void BlockRegistryProvider::setPlaceholderIdentifier(std::string identifier) {
    if (identifier.empty()) {
        return;
    }
    m_placeholderIdentifier = std::move(identifier);
}

} // namespace Rigel::Persistence
