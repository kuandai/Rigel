#include "Rigel/Persistence/Backends/CR/CRChunkMapping.h"

namespace Rigel::Persistence::Backends::CR {

namespace {

int32_t floorDiv(int32_t value, int32_t divisor) {
    int32_t q = value / divisor;
    int32_t r = value % divisor;
    if (r < 0) {
        q -= 1;
    }
    return q;
}

int32_t floorMod(int32_t value, int32_t divisor) {
    int32_t r = value % divisor;
    if (r < 0) {
        r += divisor;
    }
    return r;
}

} // namespace

CRSubchunkCoord toRigelChunk(const ChunkKey& crChunkKey) {
    CRSubchunkCoord out;
    out.rigelChunkX = floorDiv(crChunkKey.x, 2);
    out.rigelChunkY = floorDiv(crChunkKey.y, 2);
    out.rigelChunkZ = floorDiv(crChunkKey.z, 2);

    int32_t sx = floorMod(crChunkKey.x, 2);
    int32_t sy = floorMod(crChunkKey.y, 2);
    int32_t sz = floorMod(crChunkKey.z, 2);
    out.subchunkIndex = sx + (sy << 1) + (sz << 2);
    return out;
}

ChunkKey toCRChunk(const CRSubchunkCoord& rigel) {
    ChunkKey out;
    int32_t sx = rigel.subchunkIndex & 1;
    int32_t sy = (rigel.subchunkIndex >> 1) & 1;
    int32_t sz = (rigel.subchunkIndex >> 2) & 1;
    out.x = rigel.rigelChunkX * 2 + sx;
    out.y = rigel.rigelChunkY * 2 + sy;
    out.z = rigel.rigelChunkZ * 2 + sz;
    return out;
}

CRLocalMapping toRigelLocal(int32_t crLocalX, int32_t crLocalY, int32_t crLocalZ, int32_t subchunkIndex) {
    CRLocalMapping out;
    int32_t sx = subchunkIndex & 1;
    int32_t sy = (subchunkIndex >> 1) & 1;
    int32_t sz = (subchunkIndex >> 2) & 1;
    out.x = crLocalX + sx * 16;
    out.y = crLocalY + sy * 16;
    out.z = crLocalZ + sz * 16;
    return out;
}

} // namespace Rigel::Persistence::Backends::CR
