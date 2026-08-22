#pragma once

#include "Rigel/Entity/EntityPersistence.h"

#include <cstddef>
#include <span>
#include <string_view>

namespace Rigel::Entity::detail {

struct EntityRegionPayloadInfo {
    size_t encodedBytes = 0;
    size_t chunks = 0;
    size_t entities = 0;
};

enum class EntityRegionPayloadInspection {
    Valid,
    Invalid,
    ChunkLimitExceeded,
    EntityLimitExceeded,
};

EntityRegionPayloadInfo measureEntityRegionPayload(
    const std::vector<EntityPersistedChunk>& chunks);

size_t measurePersistedEntityBytes(
    std::string_view typeId,
    std::string_view modelId);

EntityRegionPayloadInspection inspectEntityRegionPayload(
    std::span<const uint8_t> payload,
    size_t maxChunks,
    size_t maxEntities,
    EntityRegionPayloadInfo& info);

} // namespace Rigel::Entity::detail
