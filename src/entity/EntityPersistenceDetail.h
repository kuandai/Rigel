#pragma once

#include "Rigel/Entity/EntityPersistence.h"

#include <cstddef>
#include <limits>
#include <span>

namespace Rigel::Entity::detail {

struct EntityRegionPayloadInfo {
    size_t encodedBytes = 0;
    size_t chunks = 0;
    size_t entities = 0;
    size_t stringBytes = 0;
};

enum class EntityRegionPayloadInspection {
    Valid,
    Invalid,
    ChunkLimitExceeded,
    EntityLimitExceeded,
};

EntityRegionPayloadInfo measureEntityRegionPayload(
    const std::vector<EntityPersistedChunk>& chunks);

EntityRegionPayloadInspection inspectEntityRegionPayload(
    std::span<const uint8_t> payload,
    size_t maxChunks,
    size_t maxEntities,
    EntityRegionPayloadInfo& info);

} // namespace Rigel::Entity::detail
