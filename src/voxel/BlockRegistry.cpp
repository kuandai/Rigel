#include "Rigel/Voxel/BlockRegistry.h"

#include <spdlog/spdlog.h>

namespace Rigel::Voxel {

BlockRegistry::BlockRegistry() {
    // Register air as type 0
    BlockType air;
    air.identifier = "rigel:air";
    air.model = "none";
    air.isOpaque = false;
    air.isSolid = false;
    air.layer = RenderLayer::Opaque;
    air.emittedLight = 0;
    air.lightAttenuation = 0;

    m_types.push_back(std::move(air));
    m_identifierMap["rigel:air"] = BlockID{0};

    spdlog::debug("BlockRegistry initialized with air (ID 0)");
}

BlockID BlockRegistry::registerBlock(const std::string& identifier, BlockType type) {
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

std::optional<BlockID> BlockRegistry::findByIdentifier(const std::string& identifier) const {
    auto it = m_identifierMap.find(identifier);
    if (it == m_identifierMap.end()) {
        return std::nullopt;
    }
    return it->second;
}

} // namespace Rigel::Voxel
