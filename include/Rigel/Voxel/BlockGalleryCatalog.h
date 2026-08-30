#pragma once

#include "Block.h"

#include <cstddef>
#include <string>
#include <vector>

namespace Rigel::Voxel {

class BlockRegistry;

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

private:
    static constexpr size_t MissingIndex = static_cast<size_t>(-1);

    std::vector<BlockGalleryCatalogEntry> m_entries;
    std::vector<BlockGalleryEmptyGeometryExclusion> m_emptyGeometryExclusions;
    std::vector<size_t> m_blockIdToCatalogIndex;
    std::vector<size_t> m_gridToCatalogIndex;
    BlockGalleryCatalogDiagnostics m_diagnostics;
    BlockGalleryGridDimensions m_gridDimensions;
};

} // namespace Rigel::Voxel
