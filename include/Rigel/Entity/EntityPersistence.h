#pragma once

#include "Rigel/Persistence/Types.h"
#include "Rigel/Voxel/ChunkCoord.h"

#include <glm/vec3.hpp>

#include <cstdint>
#include <span>

namespace Rigel::Entity {

using EntityPersistedEntity = Persistence::EntityPersistedEntity;
using EntityPersistedChunk = Persistence::EntityPersistedChunk;

struct PersistenceRegionCoord {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
};

PersistenceRegionCoord persistenceRegionForChunk(Voxel::ChunkCoord coord);

std::vector<uint8_t> encodeEntityRegionPayload(
    const std::vector<EntityPersistedChunk>& chunks);

bool decodeEntityRegionPayload(std::span<const uint8_t> payload,
                               std::vector<EntityPersistedChunk>& outChunks);

} // namespace Rigel::Entity
