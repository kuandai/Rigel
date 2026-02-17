#include "TestFramework.h"

#include "Rigel/Voxel/Lod/SvoLodTypes.h"

using namespace Rigel::Voxel;

TEST_CASE(SvoLodTypes_ChunkToCellHandlesNegativeCoordinates) {
    const int span = 8;

    CHECK_EQ(chunkToLodCell({0, 0, 0}, span), (LodCellKey{0, 0, 0, 0}));
    CHECK_EQ(chunkToLodCell({7, 7, 7}, span), (LodCellKey{0, 0, 0, 0}));
    CHECK_EQ(chunkToLodCell({8, 0, 0}, span), (LodCellKey{0, 1, 0, 0}));
    CHECK_EQ(chunkToLodCell({-1, 0, 0}, span), (LodCellKey{0, -1, 0, 0}));
    CHECK_EQ(chunkToLodCell({-8, 0, 0}, span), (LodCellKey{0, -1, 0, 0}));
    CHECK_EQ(chunkToLodCell({-9, 0, 0}, span), (LodCellKey{0, -2, 0, 0}));
}

TEST_CASE(SvoLodTypes_TouchedCellsInteriorReturnsSingleCell) {
    auto cells = touchedLodCellsForChunk({3, 4, 5}, 8);

    CHECK_EQ(cells.size(), static_cast<size_t>(1));
    CHECK_EQ(cells[0], (LodCellKey{0, 0, 0, 0}));
}

TEST_CASE(SvoLodTypes_TouchedCellsCornerReturnsEightCells) {
    auto cells = touchedLodCellsForChunk({0, 0, 0}, 8);

    CHECK_EQ(cells.size(), static_cast<size_t>(8));
    CHECK_EQ(cells.front(), (LodCellKey{0, -1, -1, -1}));
    CHECK_EQ(cells.back(), (LodCellKey{0, 0, 0, 0}));

    auto opposite = touchedLodCellsForChunk({7, 7, 7}, 8);
    CHECK_EQ(opposite.size(), static_cast<size_t>(8));
    CHECK_EQ(opposite.front(), (LodCellKey{0, 0, 0, 0}));
    CHECK_EQ(opposite.back(), (LodCellKey{0, 1, 1, 1}));
}

TEST_CASE(SvoLodTypes_TouchedCellsEdgeCrossesOnlyRequiredAxes) {
    auto cells = touchedLodCellsForChunk({0, 3, 7}, 8);

    CHECK_EQ(cells.size(), static_cast<size_t>(4));
    CHECK_EQ(cells[0], (LodCellKey{0, -1, 0, 0}));
    CHECK_EQ(cells[1], (LodCellKey{0, -1, 0, 1}));
    CHECK_EQ(cells[2], (LodCellKey{0, 0, 0, 0}));
    CHECK_EQ(cells[3], (LodCellKey{0, 0, 0, 1}));
}
