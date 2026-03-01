#include "Rigel/Voxel/BlockRegistry.h"

#include <spdlog/spdlog.h>

namespace Rigel::Voxel {

namespace {

constexpr uint64_t kFNV64Offset = 14695981039346656037ull;
constexpr uint64_t kFNV64Prime = 1099511628211ull;

void hashBytes(uint64_t& hash, const void* data, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) {
        hash ^= static_cast<uint64_t>(bytes[i]);
        hash *= kFNV64Prime;
    }
}

template <typename T>
void hashPod(uint64_t& hash, const T& value) {
    hashBytes(hash, &value, sizeof(T));
}

void hashString(uint64_t& hash, const std::string& value) {
    hashBytes(hash, value.data(), value.size());
    static constexpr uint8_t separator = 0xff;
    hashBytes(hash, &separator, sizeof(separator));
}

} // namespace

BlockRegistry::BlockRegistry() {
    // Register air as type 0
    BlockType air;
    air.identifier = "base:air";
    air.model = "none";
    air.isOpaque = false;
    air.isSolid = false;
    air.layer = RenderLayer::Opaque;
    air.emittedLight = 0;
    air.lightAttenuation = 0;

    m_types.push_back(std::move(air));
    m_identifierMap["base:air"] = BlockID{0};

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

uint64_t BlockRegistry::snapshotHash() const {
    uint64_t hash = kFNV64Offset;
    hashString(hash, "Rigel.BlockRegistry.v1");

    const uint64_t blockCount = static_cast<uint64_t>(m_types.size());
    hashPod(hash, blockCount);

    for (const auto& type : m_types) {
        hashString(hash, type.identifier);
        hashString(hash, type.model);

        const uint8_t isOpaque = type.isOpaque ? 1u : 0u;
        const uint8_t isSolid = type.isSolid ? 1u : 0u;
        const uint8_t cullSameType = type.cullSameType ? 1u : 0u;
        const uint8_t layer = static_cast<uint8_t>(type.layer);
        hashPod(hash, isOpaque);
        hashPod(hash, isSolid);
        hashPod(hash, cullSameType);
        hashPod(hash, layer);
        hashPod(hash, type.emittedLight);
        hashPod(hash, type.lightAttenuation);

        for (const auto& face : type.textures.faces) {
            hashString(hash, face);
        }
    }

    return hash;
}

} // namespace Rigel::Voxel
