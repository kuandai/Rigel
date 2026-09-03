#include "Rigel/Voxel/BlockRegistry.h"

#include "BlockModelGeometry.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <utility>

namespace Rigel::Voxel {
namespace {

std::optional<BlockModelBounds> orientedVisualExtents(
    const BlockModelInstance& instance
) {
    if (!instance || instance->isEmpty()) {
        return std::nullopt;
    }

    std::optional<BlockModelBounds> result;
    for (const BlockModelCuboid& cuboid : instance->cuboids()) {
        const bool hasSurface = std::any_of(
            cuboid.faces.begin(), cuboid.faces.end(),
            [](const auto& face) { return face.has_value(); });
        if (!hasSurface) {
            continue;
        }

        const BlockModelBounds bounds = detail::orientedBounds(
            cuboid.bounds, instance.orientation);
        for (size_t axis = 0; axis < 3; ++axis) {
            if (!std::isfinite(bounds.min[axis]) ||
                !std::isfinite(bounds.max[axis]) ||
                bounds.min[axis] > bounds.max[axis]) {
                throw BlockRegistrationError(
                    "Block model bounds must be finite and ordered");
            }
        }

        if (!result) {
            result = bounds;
            continue;
        }
        for (size_t axis = 0; axis < 3; ++axis) {
            result->min[axis] = std::min(
                result->min[axis], bounds.min[axis]);
            result->max[axis] = std::max(
                result->max[axis], bounds.max[axis]);
        }
    }
    return result;
}

void includeExtents(
    std::optional<BlockModelBounds>& aggregate,
    const std::optional<BlockModelBounds>& addition
) {
    if (!addition) {
        return;
    }
    if (!aggregate) {
        aggregate = addition;
        return;
    }
    for (size_t axis = 0; axis < 3; ++axis) {
        aggregate->min[axis] = std::min(
            aggregate->min[axis], addition->min[axis]);
        aggregate->max[axis] = std::max(
            aggregate->max[axis], addition->max[axis]);
    }
}

} // namespace

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

    const std::optional<BlockModelBounds> visualExtents =
        orientedVisualExtents(type.model);

    BlockID id{static_cast<uint16_t>(m_types.size())};

    type.identifier = actualId;

    m_types.push_back(std::move(type));
    m_identifierMap[actualId] = id;
    includeExtents(m_modelExtents, visualExtents);

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
    const std::optional<BlockModelBounds> originalModelExtents =
        m_modelExtents;
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
        m_modelExtents = originalModelExtents;
        throw;
    }
}

void BlockRegistry::swap(BlockRegistry& other) noexcept {
    m_types.swap(other.m_types);
    m_identifierMap.swap(other.m_identifierMap);
    m_modelExtents.swap(other.m_modelExtents);
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
