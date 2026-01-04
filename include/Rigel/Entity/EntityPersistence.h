#pragma once

#include "EntityId.h"

#include <Rigel/Voxel/ChunkCoord.h>

#include <glm/vec3.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Rigel::Entity {

struct EntityPersistedEntity {
    std::string typeId;
    EntityId id;
    glm::vec3 position{0.0f};
    glm::vec3 velocity{0.0f};
    glm::vec3 viewDirection{0.0f, 0.0f, -1.0f};
    std::string modelId;
};

struct EntityPersistedChunk {
    Voxel::ChunkCoord coord;
    std::vector<EntityPersistedEntity> entities;
};

std::vector<uint8_t> encodeEntityRegionPayload(
    const std::vector<EntityPersistedChunk>& chunks);

bool decodeEntityRegionPayload(std::span<const uint8_t> payload,
                               std::vector<EntityPersistedChunk>& outChunks);

} // namespace Rigel::Entity
