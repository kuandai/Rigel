#pragma once

#include "Rigel/Persistence/Types.h"

#include <glm/vec3.hpp>

#include <cstdint>
#include <span>

namespace Rigel::Entity {

using EntityPersistedEntity = Persistence::EntityPersistedEntity;
using EntityPersistedChunk = Persistence::EntityPersistedChunk;

std::vector<uint8_t> encodeEntityRegionPayload(
    const std::vector<EntityPersistedChunk>& chunks);

bool decodeEntityRegionPayload(std::span<const uint8_t> payload,
                               std::vector<EntityPersistedChunk>& outChunks);

} // namespace Rigel::Entity
