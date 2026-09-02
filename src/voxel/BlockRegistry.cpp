#include "Rigel/Voxel/BlockRegistry.h"

#include <spdlog/spdlog.h>

#include <unordered_set>
#include <utility>

namespace Rigel::Voxel {

BlockRegistry::BlockRegistry() {
    // Register air as type 0
    BlockType air;
    air.identifier = "base:air";
    air.model = BlockModel::empty();
    air.isOpaque = false;
    air.collision = BlockCollisionShape::empty();
    air.layer = RenderLayer::Opaque;
    air.emittedLight = 0;
    air.lightAttenuation = 0;

    m_types.push_back(std::move(air));
    m_identifierMap["base:air"] = BlockID{0};

    spdlog::debug("BlockRegistry initialized with air (ID 0)");
}

BlockID BlockRegistry::registerBlock(const std::string& identifier, BlockType type) {
    if (m_frozen) {
        throw BlockRegistrationError("Block registry is frozen");
    }
    if (!type.model) {
        throw BlockRegistrationError(
            "Block registration requires explicit geometry: " + identifier);
    }
    if (!type.identifier.empty() && type.identifier != identifier) {
        throw BlockRegistrationError(
            "Block identifier mismatch: registry '" + identifier + "' vs type '" + type.identifier + "'"
        );
    }

    std::string actualId = identifier;

    // Check for duplicate
    if (m_identifierMap.find(actualId) != m_identifierMap.end()) {
        throw BlockRegistrationError(
            "Block identifier already registered: " + actualId
        );
    }

    // Check for overflow
    if (m_types.size() >= 65535) {
        throw BlockRegistrationError(
            "Maximum block type count exceeded (65535)"
        );
    }

    BlockID id{static_cast<uint16_t>(m_types.size())};

    type.identifier = actualId;

    m_types.push_back(std::move(type));
    m_identifierMap[actualId] = id;

    spdlog::debug("Registered block: {} (ID {})", actualId, id.type);

    return id;
}

void BlockRegistry::registerBlocks(
    std::vector<std::pair<std::string, BlockType>> blocks
) {
    if (m_frozen) {
        throw BlockRegistrationError("Block registry is frozen");
    }
    if (m_types.size() + blocks.size() > 65535) {
        throw BlockRegistrationError(
            "Maximum block type count exceeded (65535)");
    }

    std::unordered_set<std::string> candidateIdentifiers;
    for (const auto& [identifier, type] : blocks) {
        if (!type.identifier.empty() && type.identifier != identifier) {
            throw BlockRegistrationError(
                "Block identifier mismatch: registry '" + identifier +
                "' vs type '" + type.identifier + "'");
        }
        if (!type.model) {
            throw BlockRegistrationError(
                "Block registration requires explicit geometry: " + identifier);
        }
        if (hasIdentifier(identifier) ||
            !candidateIdentifiers.insert(identifier).second) {
            throw BlockRegistrationError(
                "Block identifier already registered: " + identifier);
        }
    }

    const size_t originalSize = m_types.size();
    m_types.reserve(originalSize + blocks.size());
    m_identifierMap.reserve(m_identifierMap.size() + blocks.size());
    try {
        for (auto& [identifier, type] : blocks) {
            registerBlock(identifier, std::move(type));
        }
    } catch (...) {
        for (const auto& [identifier, type] : blocks) {
            m_identifierMap.erase(identifier);
        }
        m_types.resize(originalSize);
        throw;
    }
}

void BlockRegistry::swap(BlockRegistry& other) noexcept {
    m_types.swap(other.m_types);
    m_identifierMap.swap(other.m_identifierMap);
    std::swap(m_frozen, other.m_frozen);
}

std::optional<BlockID> BlockRegistry::findByIdentifier(const std::string& identifier) const {
    auto it = m_identifierMap.find(identifier);
    if (it == m_identifierMap.end()) {
        return std::nullopt;
    }
    return it->second;
}

} // namespace Rigel::Voxel
