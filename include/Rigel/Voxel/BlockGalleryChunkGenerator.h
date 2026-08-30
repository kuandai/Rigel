#pragma once

#include "BlockGalleryCatalog.h"
#include "ChunkCoord.h"
#include "GeneratorDefinition.h"

#include <map>
#include <memory>
#include <optional>
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

struct BlockGalleryFloorBounds {
    int minX = 0;
    int maxX = -1;
    int minZ = 0;
    int maxZ = -1;

    bool empty() const { return minX > maxX || minZ > maxZ; }
    bool operator==(const BlockGalleryFloorBounds&) const = default;
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

    const std::shared_ptr<const BlockGalleryCatalog>& catalog() const {
        return m_catalog;
    }
    const std::vector<BlockGalleryBlockPlacement>& placements() const {
        return m_placements;
    }
    std::optional<BlockID> referenceFloorBlock() const {
        return m_referenceFloorBlock;
    }
    BlockGalleryFloorBounds floorBounds() const { return m_floorBounds; }
    BlockGalleryWorldPosition diagnosticOrigin() const {
        return m_diagnosticOrigin;
    }
    BlockGalleryOverview overview() const { return m_overview; }
    GeneratorDefinitionData::Bounds worldBounds() const { return {0, 2}; }

    bool containsChunk(ChunkCoord coord) const;
    void generate(ChunkCoord coord, ChunkBuffer& out) const;

private:
    void addPlacement(BlockGalleryBlockPlacement placement);

    std::shared_ptr<const BlockGalleryCatalog> m_catalog;
    std::vector<BlockGalleryBlockPlacement> m_placements;
    std::map<ChunkCoord, std::vector<BlockGalleryBlockPlacement>>
        m_placementsByChunk;
    std::optional<BlockID> m_referenceFloorBlock;
    BlockGalleryFloorBounds m_floorBounds;
    BlockGalleryWorldPosition m_diagnosticOrigin;
    BlockGalleryOverview m_overview;
};

PreparedGeneratorDefinitionSnapshot prepareBlockGalleryGeneratorIdentity(
    const BlockRegistry& registry,
    GeneratorDefinitionData::Bounds bounds);

} // namespace Rigel::Voxel
