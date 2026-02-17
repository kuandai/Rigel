#include "Rigel/Voxel/Lod/SvoLodTypes.h"

#include <algorithm>
#include <array>
#include <unordered_set>

namespace Rigel::Voxel {

namespace {

int positiveModulo(int value, int divisor) {
    int mod = value % divisor;
    return (mod < 0) ? (mod + divisor) : mod;
}

int floorDiv(int value, int divisor) {
    int quotient = value / divisor;
    int remainder = value % divisor;
    if (remainder != 0 && ((remainder < 0) != (divisor < 0))) {
        --quotient;
    }
    return quotient;
}

} // namespace

size_t LodCellKeyHash::operator()(const LodCellKey& key) const noexcept {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](uint64_t value) {
        h ^= value;
        h *= 1099511628211ull;
    };

    mix(static_cast<uint64_t>(static_cast<int64_t>(key.level)));
    mix(static_cast<uint64_t>(static_cast<int64_t>(key.x)));
    mix(static_cast<uint64_t>(static_cast<int64_t>(key.y)));
    mix(static_cast<uint64_t>(static_cast<int64_t>(key.z)));
    return static_cast<size_t>(h);
}

LodCellKey chunkToLodCell(ChunkCoord coord, int spanChunks, int lodLevel) {
    const int span = std::max(1, spanChunks);
    return LodCellKey{
        lodLevel,
        floorDiv(coord.x, span),
        floorDiv(coord.y, span),
        floorDiv(coord.z, span)
    };
}

std::vector<LodCellKey> touchedLodCellsForChunk(ChunkCoord coord, int spanChunks, int lodLevel) {
    const int span = std::max(1, spanChunks);
    const LodCellKey base = chunkToLodCell(coord, span, lodLevel);

    std::array<int, 3> xOffsets{0, 0, 0};
    std::array<int, 3> yOffsets{0, 0, 0};
    std::array<int, 3> zOffsets{0, 0, 0};
    size_t xCount = 1;
    size_t yCount = 1;
    size_t zCount = 1;

    const int localX = positiveModulo(coord.x, span);
    const int localY = positiveModulo(coord.y, span);
    const int localZ = positiveModulo(coord.z, span);

    if (localX == 0) {
        xOffsets[xCount++] = -1;
    }
    if (localX == span - 1) {
        xOffsets[xCount++] = 1;
    }
    if (localY == 0) {
        yOffsets[yCount++] = -1;
    }
    if (localY == span - 1) {
        yOffsets[yCount++] = 1;
    }
    if (localZ == 0) {
        zOffsets[zCount++] = -1;
    }
    if (localZ == span - 1) {
        zOffsets[zCount++] = 1;
    }

    std::unordered_set<LodCellKey, LodCellKeyHash> unique;
    for (size_t xi = 0; xi < xCount; ++xi) {
        for (size_t yi = 0; yi < yCount; ++yi) {
            for (size_t zi = 0; zi < zCount; ++zi) {
                unique.insert(LodCellKey{
                    base.level,
                    base.x + xOffsets[xi],
                    base.y + yOffsets[yi],
                    base.z + zOffsets[zi]
                });
            }
        }
    }

    std::vector<LodCellKey> out;
    out.reserve(unique.size());
    for (const auto& key : unique) {
        out.push_back(key);
    }
    std::sort(out.begin(), out.end(), [](const LodCellKey& a, const LodCellKey& b) {
        if (a.level != b.level) {
            return a.level < b.level;
        }
        if (a.x != b.x) {
            return a.x < b.x;
        }
        if (a.y != b.y) {
            return a.y < b.y;
        }
        return a.z < b.z;
    });
    return out;
}

} // namespace Rigel::Voxel
