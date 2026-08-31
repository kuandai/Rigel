#include "LogCapture.h"
#include "TestFramework.h"

#include "Rigel/Voxel/BlockGalleryCatalog.h"
#include "Rigel/Voxel/BlockRegistry.h"

#include <algorithm>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace {
using namespace Rigel::Voxel;

BlockID addBlock(BlockRegistry& registry, const std::string& identifier) {
    BlockType type;
    type.identifier = identifier;
    return registry.registerBlock(identifier, std::move(type));
}

BlockID addEmptyBlock(BlockRegistry& registry, const std::string& identifier) {
    BlockType type;
    type.identifier = identifier;
    type.model = BlockModel::empty();
    return registry.registerBlock(identifier, std::move(type));
}

std::vector<std::string> identifiers(const BlockGalleryCatalog& catalog) {
    std::vector<std::string> result;
    for (const BlockGalleryCatalogEntry& entry : catalog.entries()) {
        result.push_back(entry.identifier);
    }
    return result;
}

void addNumberedFamily(
    BlockRegistry& registry,
    std::string_view family,
    size_t count
) {
    for (size_t index = 0; index < count; ++index) {
        addBlock(
            registry,
            std::string(family) + "[variant=" + std::to_string(index) + "]");
    }
}

} // namespace

TEST_CASE(BlockGalleryCatalog_OrdersParsedIdentifiersAndGroupsFamilies) {
    BlockRegistry registry;
    for (const std::string& identifier : {
             "zeta:brick",
             "alpha:stone[axis=y]",
             "alpha:stone[mode=a,axis=x]",
             "alpha:brick",
             "alpha:stone[axis=x,mode=b]",
             "alpha:stone[axis=x,mode=a]",
             "alpha:stone",
         }) {
        addBlock(registry, identifier);
    }
    registry.freeze();

    const BlockGalleryCatalog catalog(registry);
    CHECK_EQ(
        identifiers(catalog),
        (std::vector<std::string>{
            "alpha:brick",
            "alpha:stone",
            "alpha:stone[axis=x,mode=a]",
            "alpha:stone[mode=a,axis=x]",
            "alpha:stone[axis=x,mode=b]",
            "alpha:stone[axis=y]",
            "zeta:brick",
        }));

    CHECK_EQ(catalog.entries()[0].family, std::string("alpha:brick"));
    for (size_t index = 1; index < 6; ++index) {
        CHECK_EQ(catalog.entries()[index].family, std::string("alpha:stone"));
    }
    CHECK_EQ(catalog.entries()[6].family, std::string("zeta:brick"));
}

TEST_CASE(BlockGalleryCatalog_IsStableAcrossRegistrationOrder) {
    const std::vector<std::string> source = {
        "invented:panel[side=left,color=blue]",
        "invented:panel[color=blue,side=right]",
        "invented:lamp[lit=false]",
        "invented:lamp[lit=true]",
        "other:panel",
    };

    BlockRegistry forward;
    for (const std::string& identifier : source) {
        addBlock(forward, identifier);
    }
    forward.freeze();

    BlockRegistry reverse;
    for (auto iterator = source.rbegin(); iterator != source.rend(); ++iterator) {
        addBlock(reverse, *iterator);
    }
    reverse.freeze();

    const BlockGalleryCatalog first(forward);
    const BlockGalleryCatalog second(reverse);
    CHECK_EQ(first.gridDimensions(), second.gridDimensions());
    CHECK_EQ(first.entries().size(), second.entries().size());
    for (size_t index = 0; index < first.entries().size(); ++index) {
        const BlockGalleryCatalogEntry& left = first.entries()[index];
        const BlockGalleryCatalogEntry& right = second.entries()[index];
        CHECK_EQ(left.identifier, right.identifier);
        CHECK_EQ(left.family, right.family);
        CHECK_EQ(left.catalogIndex, right.catalogIndex);
        CHECK_EQ(left.gridCoordinate, right.gridCoordinate);
        CHECK_EQ(left.specimenPosition, right.specimenPosition);
    }
}

TEST_CASE(BlockGalleryCatalog_MapsDenseSerpentineRowsWithoutFamilyPadding) {
    BlockRegistry registry;
    addNumberedFamily(registry, "invented:a", 3);
    addNumberedFamily(registry, "invented:b", 9);
    addNumberedFamily(registry, "invented:c", 2);
    registry.freeze();

    const BlockGalleryCatalog catalog(registry);
    CHECK_EQ(
        catalog.gridDimensions(),
        (BlockGalleryGridDimensions{4, 4}));

    const std::vector<BlockGalleryGridCoordinate> expected = {
        {0, 0}, {1, 0}, {2, 0}, {3, 0},
        {3, 1}, {2, 1}, {1, 1}, {0, 1},
        {0, 2}, {1, 2}, {2, 2}, {3, 2},
        {3, 3}, {2, 3},
    };
    CHECK_EQ(catalog.entries().size(), expected.size());
    for (size_t index = 0; index < expected.size(); ++index) {
        CHECK_EQ(catalog.entries()[index].catalogIndex, index);
        CHECK_EQ(catalog.entries()[index].gridCoordinate, expected[index]);
        if (index != 0) {
            const BlockGalleryGridCoordinate previous = expected[index - 1];
            const BlockGalleryGridCoordinate current = expected[index];
            const size_t distance =
                std::max(previous.column, current.column) -
                    std::min(previous.column, current.column) +
                std::max(previous.row, current.row) -
                    std::min(previous.row, current.row);
            CHECK_EQ(distance, static_cast<size_t>(1));
        }
    }
    for (size_t row = 0; row < 3; ++row) {
        for (size_t column = 0; column < 4; ++column) {
            CHECK(catalog.findByGridCoordinate({column, row}));
        }
    }
    CHECK(!catalog.findByGridCoordinate({0, 3}));
    CHECK(!catalog.findByGridCoordinate({1, 3}));
    CHECK(catalog.findByGridCoordinate({2, 3}));
    CHECK(catalog.findByGridCoordinate({3, 3}));
}

TEST_CASE(BlockGalleryCatalog_PlacementAndLookupsRoundTripWithoutDuplicates) {
    BlockRegistry registry;
    for (size_t index = 0; index < 17; ++index) {
        addBlock(
            registry,
            "invented:specimen[state=" + std::to_string(index) + "]");
    }
    registry.freeze();

    const BlockGalleryCatalog catalog(registry);
    CHECK_EQ(
        catalog.gridDimensions(),
        (BlockGalleryGridDimensions{5, 4}));

    std::set<std::pair<size_t, size_t>> gridCoordinates;
    std::set<std::tuple<int, int, int>> specimenPositions;
    for (const BlockGalleryCatalogEntry& entry : catalog.entries()) {
        CHECK(gridCoordinates.emplace(
            entry.gridCoordinate.column, entry.gridCoordinate.row).second);
        CHECK(specimenPositions.emplace(
            entry.specimenPosition.x,
            entry.specimenPosition.y,
            entry.specimenPosition.z).second);
        CHECK_EQ(
            entry.specimenPosition.x,
            static_cast<int>(entry.gridCoordinate.column) *
                BlockGalleryCatalog::SpecimenSpacing);
        CHECK_EQ(
            entry.specimenPosition.z,
            static_cast<int>(entry.gridCoordinate.row) *
                BlockGalleryCatalog::SpecimenSpacing);
        CHECK_EQ(
            catalog.findByIndex(entry.catalogIndex),
            &entry);
        CHECK_EQ(catalog.findByBlockId(entry.blockId), &entry);
        CHECK_EQ(
            catalog.findByGridCoordinate(entry.gridCoordinate),
            &entry);
        CHECK_EQ(
            catalog.findBySpecimenPosition(entry.specimenPosition),
            &entry);
    }

    CHECK(!catalog.findByIndex(catalog.entries().size()));
    CHECK(!catalog.findByBlockId(BlockID{65000}));
    CHECK(!catalog.findBySpecimenPosition({1, 1, 0}));
    CHECK(!catalog.findBySpecimenPosition({0, 0, 0}));
    CHECK_EQ(
        catalog.entries()[15].gridCoordinate,
        (BlockGalleryGridCoordinate{4, 3}));
    CHECK_EQ(
        catalog.entries()[16].gridCoordinate,
        (BlockGalleryGridCoordinate{3, 3}));
    for (size_t column = 0; column < 3; ++column) {
        CHECK(!catalog.findByGridCoordinate({column, 3}));
    }
}

TEST_CASE(BlockGalleryCatalog_ReportsOnlyExplicitEmptyGeometryExclusions) {
    BlockRegistry registry;
    const BlockID lastEmptyId = addEmptyBlock(registry, "invented:z_void");
    const BlockID firstEmptyId = addEmptyBlock(registry, "invented:a_void");
    const BlockID solidId = addBlock(registry, "invented:solid");
    registry.freeze();
    Rigel::Test::LogCapture logs("block-gallery-catalog-diagnostics");

    const BlockGalleryCatalog catalog(registry);
    CHECK_EQ(
        catalog.diagnostics().loadedRegistrationCount,
        static_cast<size_t>(4));
    CHECK_EQ(catalog.diagnostics().renderableCount, static_cast<size_t>(1));
    CHECK_EQ(
        catalog.diagnostics().explicitEmptyGeometryCount,
        static_cast<size_t>(3));
    CHECK_EQ(catalog.emptyGeometryExclusions().size(), static_cast<size_t>(3));

    CHECK_EQ(
        catalog.emptyGeometryExclusions()[0].identifier,
        std::string("base:air"));
    CHECK_EQ(
        catalog.emptyGeometryExclusions()[1].identifier,
        std::string("invented:a_void"));
    CHECK_EQ(
        catalog.emptyGeometryExclusions()[2].identifier,
        std::string("invented:z_void"));
    CHECK(!catalog.findByBlockId(BlockRegistry::airId()));
    CHECK(!catalog.findByBlockId(firstEmptyId));
    CHECK(!catalog.findByBlockId(lastEmptyId));
    CHECK(catalog.findByBlockId(solidId));
    CHECK(logs.output().find("loaded=4") != std::string::npos);
    CHECK(logs.output().find("specimens=1") != std::string::npos);
    CHECK(logs.output().find("excluded_explicit_empty_geometry=3") !=
          std::string::npos);
    CHECK(logs.output().find("grid=1x1") != std::string::npos);
}

TEST_CASE(BlockGalleryCatalog_ReportsZeroRenderableRegistry) {
    BlockRegistry registry;
    registry.freeze();
    Rigel::Test::LogCapture logs("empty-block-gallery-catalog");

    const BlockGalleryCatalog catalog(registry);
    CHECK(catalog.entries().empty());
    CHECK_EQ(
        catalog.gridDimensions(),
        (BlockGalleryGridDimensions{0, 0}));
    CHECK_EQ(catalog.diagnostics().renderableCount, static_cast<size_t>(0));
    CHECK_EQ(
        catalog.diagnostics().explicitEmptyGeometryCount,
        static_cast<size_t>(1));
    CHECK(logs.output().find("no renderable registrations") !=
          std::string::npos);
    CHECK(logs.output().find("loaded=1") != std::string::npos);
    CHECK(logs.output().find("specimens=0") != std::string::npos);
    CHECK(logs.output().find("excluded_explicit_empty_geometry=1") !=
          std::string::npos);
    CHECK(logs.output().find("grid=0x0") != std::string::npos);
}

TEST_CASE(BlockGalleryCatalog_RequiresCompletedRegistration) {
    BlockRegistry registry;
    CHECK_THROWS((void)BlockGalleryCatalog(registry));
}
