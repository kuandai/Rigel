#pragma once

#include "Rigel/Persistence/Types.h"

namespace Rigel::Persistence::Backends::CR {

struct CRSubchunkCoord {
    int32_t rigelChunkX = 0;
    int32_t rigelChunkY = 0;
    int32_t rigelChunkZ = 0;
    int32_t subchunkIndex = 0;
};

struct CRLocalMapping {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
};

CRSubchunkCoord toRigelChunk(const ChunkKey& crChunkKey);
ChunkKey toCRChunk(const CRSubchunkCoord& rigel);
CRLocalMapping toRigelLocal(int32_t crLocalX, int32_t crLocalY, int32_t crLocalZ, int32_t subchunkIndex);

} // namespace Rigel::Persistence::Backends::CR
