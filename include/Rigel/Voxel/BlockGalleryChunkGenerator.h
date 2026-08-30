#pragma once

#include "BlockGalleryCatalog.h"
#include "ChunkCoord.h"
#include "GeneratorDefinition.h"

#include <map>
#include <vector>

namespace Rigel::Voxel {

class BlockRegistry;
struct ChunkBuffer;

struct BlockGalleryOverview {
    float centerX = 0.0f;
    float centerZ = 0.0f;
    float cameraDistance = 24.0f;
    float cameraHeight = 18.0f;

    bool operator==(const BlockGalleryOverview&) const = default;
};

/**
 * Deterministic voxel placement for a block gallery. The output is consumed
 * by WorldGenerator and therefore enters the ordinary chunk lifecycle.
 */
class BlockGalleryChunkGenerator final {
public:
    BlockGalleryChunkGenerator(
        const BlockRegistry& registry,
        const BlockGalleryCatalog& catalog);

    BlockGalleryOverview overview() const { return m_overview; }
    GeneratorDefinitionData::Bounds worldBounds() const { return {0, 2}; }
    void validateGeneratorBounds(
        GeneratorDefinitionData::Bounds bounds) const;

    bool containsChunk(ChunkCoord coord) const;
    void generate(ChunkCoord coord, ChunkBuffer& out) const;

private:
    friend class WorldGenerator;

    struct BlockPlacement {
        BlockGalleryWorldPosition position;
        BlockID blockId;

        bool operator==(const BlockPlacement&) const = default;
    };

    void addPlacement(BlockPlacement placement);
    bool matchesRuntimeBehavior(
        const BlockGalleryChunkGenerator& other) const;

    std::map<ChunkCoord, std::vector<BlockPlacement>> m_placementsByChunk;
    BlockGalleryOverview m_overview;
};

PreparedGeneratorDefinitionSnapshot prepareBlockGalleryGeneratorIdentity(
    const BlockRegistry& registry,
    GeneratorDefinitionData::Bounds bounds);

} // namespace Rigel::Voxel
