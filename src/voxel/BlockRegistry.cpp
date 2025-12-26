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
    // Use type.identifier since it contains the moved-in value
    // (the identifier parameter may be a reference to a moved-from string)
    // Make a copy since we'll move type later
    std::string actualId = type.identifier.empty() ? identifier : type.identifier;

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

    // Ensure identifier is set
    if (type.identifier.empty()) {
        type.identifier = identifier;
    }

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
