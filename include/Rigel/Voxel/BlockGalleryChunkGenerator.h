#pragma once

#include "BlockGalleryCatalog.h"
#include "ChunkCoord.h"
#include "GeneratorDefinition.h"

#include <map>
#include <memory>
#include <vector>

namespace Rigel::Voxel {

class BlockRegistry;
struct ChunkBuffer;

enum class BlockGalleryPlacementKind {
    ReferenceFloor,
    Specimen,
    OpaqueCullingDiagnostic,
    SameTypeCullingDiagnostic,
    CoverageCullingDiagnostic,
};

struct BlockGalleryBlockPlacement {
    BlockGalleryWorldPosition position;
    BlockID blockId;
    BlockGalleryPlacementKind kind = BlockGalleryPlacementKind::Specimen;

    bool operator==(const BlockGalleryBlockPlacement&) const = default;
};

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
        std::shared_ptr<const BlockGalleryCatalog> catalog);

    std::vector<BlockGalleryBlockPlacement> placements() const;
    BlockGalleryOverview overview() const { return m_overview; }
    GeneratorDefinitionData::Bounds worldBounds() const { return {0, 2}; }

    bool containsChunk(ChunkCoord coord) const;
    void generate(ChunkCoord coord, ChunkBuffer& out) const;

private:
    void addPlacement(BlockGalleryBlockPlacement placement);

    std::shared_ptr<const BlockGalleryCatalog> m_catalog;
    std::map<ChunkCoord, std::vector<BlockGalleryBlockPlacement>>
        m_placementsByChunk;
    BlockGalleryOverview m_overview;
};

PreparedGeneratorDefinitionSnapshot prepareBlockGalleryGeneratorIdentity(
    const BlockRegistry& registry,
    GeneratorDefinitionData::Bounds bounds);

} // namespace Rigel::Voxel
