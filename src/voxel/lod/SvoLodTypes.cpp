#include "Rigel/Voxel/Lod/SvoLodTypes.h"

#include "Rigel/Voxel/BlockRegistry.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <unordered_set>
#include <vector>

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

int ceilPow2(int value) {
    int v = std::max(1, value);
    int p = 1;
    while (p < v) {
        p <<= 1;
    }
    return p;
}

struct ChunkClass {
    LodNodeKind kind = LodNodeKind::Empty;
    LodMaterialClass materialClass = LodMaterialClass::None;
};

size_t gridIndex(int x, int y, int z, int dim) {
    return static_cast<size_t>(x) +
           static_cast<size_t>(y) * static_cast<size_t>(dim) +
           static_cast<size_t>(z) * static_cast<size_t>(dim) * static_cast<size_t>(dim);
}

ChunkClass classifyChunk(const LodChunkSnapshot& chunk,
                         LodBuildOutput& output,
                         const BlockRegistry* registry) {
    uint32_t nonAir = 0;
    uint32_t opaque = 0;
    uint32_t nonOpaque = 0;

    for (const BlockState& state : chunk.blocks) {
        if (state.isAir()) {
            continue;
        }
        ++nonAir;
        ++output.nonAirVoxelCount;

        bool isOpaque = true;
        if (registry) {
            isOpaque = registry->getType(state.id).isOpaque;
        }
        if (isOpaque) {
            ++opaque;
            ++output.opaqueVoxelCount;
        } else {
            ++nonOpaque;
            ++output.nonOpaqueVoxelCount;
        }
    }

    if (nonAir == 0) {
        return ChunkClass{LodNodeKind::Empty, LodMaterialClass::None};
    }
    if (opaque == nonAir) {
        return ChunkClass{LodNodeKind::Solid, LodMaterialClass::Opaque};
    }
    if (nonOpaque == nonAir) {
        return ChunkClass{LodNodeKind::Solid, LodMaterialClass::NonOpaque};
    }
    return ChunkClass{LodNodeKind::Mixed, LodMaterialClass::Mixed};
}

struct BuildResult {
    bool hasNode = false;
    LodNodeKind kind = LodNodeKind::Empty;
    LodMaterialClass materialClass = LodMaterialClass::None;
    uint32_t nodeIndex = LodSvoNode::INVALID_INDEX;
};

BuildResult buildNodeRecursive(const std::vector<ChunkClass>& grid,
                               int dim,
                               int x,
                               int y,
                               int z,
                               int size,
                               std::vector<LodSvoNode>& outNodes,
                               LodBuildOutput& output) {
    if (size == 1) {
        const ChunkClass& value = grid[gridIndex(x, y, z, dim)];
        if (value.kind == LodNodeKind::Empty) {
            return BuildResult{};
        }

        LodSvoNode node;
        node.kind = value.kind;
        node.materialClass = value.materialClass;
        outNodes.push_back(node);
        ++output.nodeCount;
        ++output.leafCount;
        if (value.kind == LodNodeKind::Mixed) {
            ++output.mixedNodeCount;
        }

        return BuildResult{true, value.kind, value.materialClass,
                           static_cast<uint32_t>(outNodes.size() - 1)};
    }

    const int half = size / 2;
    std::array<BuildResult, 8> children{};
    uint8_t childMask = 0;
    bool anyMixed = false;
    bool allSameMaterial = true;
    LodMaterialClass referenceMaterial = LodMaterialClass::None;
    bool haveReferenceMaterial = false;

    for (int child = 0; child < 8; ++child) {
        const int ox = (child & 1) ? half : 0;
        const int oy = (child & 2) ? half : 0;
        const int oz = (child & 4) ? half : 0;
        children[child] = buildNodeRecursive(grid,
                                             dim,
                                             x + ox,
                                             y + oy,
                                             z + oz,
                                             half,
                                             outNodes,
                                             output);

        if (!children[child].hasNode) {
            continue;
        }
        childMask |= static_cast<uint8_t>(1u << child);
        anyMixed = anyMixed || (children[child].kind == LodNodeKind::Mixed);

        const LodMaterialClass childMaterial = children[child].materialClass;
        if (!haveReferenceMaterial) {
            referenceMaterial = childMaterial;
            haveReferenceMaterial = true;
        } else if (referenceMaterial != childMaterial) {
            allSameMaterial = false;
        }
    }

    if (childMask == 0) {
        return BuildResult{};
    }

    LodSvoNode node;
    node.kind = LodNodeKind::Mixed;
    node.materialClass = (allSameMaterial && !anyMixed && haveReferenceMaterial)
        ? referenceMaterial
        : LodMaterialClass::Mixed;
    node.childMask = childMask;
    for (int child = 0; child < 8; ++child) {
        if (children[child].hasNode) {
            node.children[child] = children[child].nodeIndex;
        }
    }

    outNodes.push_back(node);
    ++output.nodeCount;
    ++output.mixedNodeCount;

    return BuildResult{true, node.kind, node.materialClass,
                       static_cast<uint32_t>(outNodes.size() - 1)};
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

LodBuildOutput buildLodBuildOutput(const LodBuildInput& input, const BlockRegistry* registry) {
    LodBuildOutput output;
    output.key = input.key;
    output.revision = input.revision;
    output.sampledChunks = static_cast<uint32_t>(input.chunks.size());

    const int span = std::max(1, input.spanChunks);
    const int dim = ceilPow2(span);
    std::vector<ChunkClass> grid(static_cast<size_t>(dim) * static_cast<size_t>(dim) *
                                     static_cast<size_t>(dim),
                                 ChunkClass{});

    const int baseX = input.key.x * span;
    const int baseY = input.key.y * span;
    const int baseZ = input.key.z * span;

    for (const LodChunkSnapshot& chunk : input.chunks) {
        const int lx = chunk.coord.x - baseX;
        const int ly = chunk.coord.y - baseY;
        const int lz = chunk.coord.z - baseZ;
        if (lx < 0 || ly < 0 || lz < 0 || lx >= span || ly >= span || lz >= span) {
            continue;
        }
        grid[gridIndex(lx, ly, lz, dim)] = classifyChunk(chunk, output, registry);
    }

    output.nodes.reserve(static_cast<size_t>(dim) * static_cast<size_t>(dim) *
                         static_cast<size_t>(dim));
    BuildResult root = buildNodeRecursive(grid, dim, 0, 0, 0, dim, output.nodes, output);
    if (root.hasNode) {
        output.rootNode = root.nodeIndex;
    }
    output.empty = (output.nonAirVoxelCount == 0 || output.rootNode == LodSvoNode::INVALID_INDEX);
    return output;
}

} // namespace Rigel::Voxel
