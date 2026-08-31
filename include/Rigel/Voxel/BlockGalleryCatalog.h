#pragma once

#include "Block.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Rigel::Voxel {

class BlockRegistry;
class BlockGalleryChunkGenerator;

struct BlockGalleryGridCoordinate {
    size_t column = 0;
    size_t row = 0;

    bool operator==(const BlockGalleryGridCoordinate&) const = default;
};

struct BlockGalleryGridDimensions {
    size_t columns = 0;
    size_t rows = 0;

    bool operator==(const BlockGalleryGridDimensions&) const = default;
};

struct BlockGalleryWorldPosition {
    int x = 0;
    int y = 0;
    int z = 0;

    bool operator==(const BlockGalleryWorldPosition&) const = default;
};

struct BlockGalleryCatalogEntry {
    std::string identifier;
    BlockID blockId;
    std::string family;
    size_t catalogIndex = 0;
    BlockGalleryGridCoordinate gridCoordinate;
    BlockGalleryWorldPosition specimenPosition;
};

struct BlockGalleryEmptyGeometryExclusion {
    std::string identifier;
    BlockID blockId;
};

enum class BlockGalleryCullingCaseKind : uint8_t {
    OpaqueFullCube,
    SameType,
    OpaqueCoverage,
};

enum class BlockGalleryDiagnosticPairPosition : uint8_t {
    First,
    Second,
};

/** One targetable cell in a two-block culling diagnostic pair. */
struct BlockGalleryCullingDiagnosticPlacement {
    BlockGalleryCullingCaseKind caseKind =
        BlockGalleryCullingCaseKind::OpaqueFullCube;
    std::string label;
    size_t caseOrdinal = 0;
    size_t caseCount = 0;
    BlockGalleryDiagnosticPairPosition pairPosition =
        BlockGalleryDiagnosticPairPosition::First;
    size_t pairOrdinal = 0;
    size_t pairCount = 0;
    std::string sourceIdentifier;
    BlockID sourceBlockId;
    BlockGalleryWorldPosition worldPosition;

    bool operator==(
        const BlockGalleryCullingDiagnosticPlacement&) const = default;
};

struct BlockGalleryCatalogDiagnostics {
    size_t loadedRegistrationCount = 0;
    size_t renderableCount = 0;
    size_t explicitEmptyGeometryCount = 0;
};

/**
 * Deterministic placement metadata for every renderable registration in a
 * frozen BlockRegistry. A registration is excluded only when its explicit
 * model has no cuboids; those registrations, including air, remain visible in
 * the diagnostics and exclusion list.
 */
class BlockGalleryCatalog final {
public:
    static constexpr int SpecimenSpacing = 4;
    static constexpr int SpecimenHeight = 1;

    explicit BlockGalleryCatalog(const BlockRegistry& registry);

    const std::vector<BlockGalleryCatalogEntry>& entries() const {
        return m_entries;
    }
    const std::vector<BlockGalleryEmptyGeometryExclusion>&
    emptyGeometryExclusions() const {
        return m_emptyGeometryExclusions;
    }
    const std::vector<BlockGalleryCullingDiagnosticPlacement>&
    cullingDiagnosticPlacements() const {
        return m_cullingDiagnosticPlacements;
    }
    const BlockGalleryCatalogDiagnostics& diagnostics() const {
        return m_diagnostics;
    }
    BlockGalleryGridDimensions gridDimensions() const {
        return m_gridDimensions;
    }

    const BlockGalleryCatalogEntry* findByIndex(size_t catalogIndex) const;
    const BlockGalleryCatalogEntry* findByBlockId(BlockID blockId) const;
    const BlockGalleryCatalogEntry* findByGridCoordinate(
        BlockGalleryGridCoordinate coordinate) const;
    const BlockGalleryCatalogEntry* findBySpecimenPosition(
        BlockGalleryWorldPosition position) const;
    const BlockGalleryCullingDiagnosticPlacement*
    findCullingDiagnosticByPosition(
        BlockGalleryWorldPosition position) const;

private:
    friend class BlockGalleryChunkGenerator;

    static constexpr size_t MissingIndex = static_cast<size_t>(-1);

    const BlockRegistry* m_sourceRegistry = nullptr;
    std::vector<BlockGalleryCatalogEntry> m_entries;
    std::vector<BlockGalleryEmptyGeometryExclusion> m_emptyGeometryExclusions;
    std::vector<BlockGalleryCullingDiagnosticPlacement>
        m_cullingDiagnosticPlacements;
    std::vector<size_t> m_blockIdToCatalogIndex;
    std::vector<size_t> m_gridToCatalogIndex;
    BlockGalleryCatalogDiagnostics m_diagnostics;
    BlockGalleryGridDimensions m_gridDimensions;
};

} // namespace Rigel::Voxel
