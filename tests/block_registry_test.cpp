#include "TestFramework.h"

#include "Rigel/Voxel/BlockRegistry.h"

using namespace Rigel::Voxel;

TEST_CASE(BlockRegistry_RegisterAndLookup) {
    BlockRegistry registry;
    CHECK_EQ(registry.size(), static_cast<size_t>(1));

    BlockType stone;
    stone.identifier = "rigel:stone";
    auto stoneId = registry.registerBlock(stone.identifier, stone);

    CHECK(!stoneId.isAir());
    CHECK_EQ(registry.size(), static_cast<size_t>(2));

    auto lookup = registry.findByIdentifier("rigel:stone");
    CHECK(lookup.has_value());
    CHECK_EQ(lookup->type, stoneId.type);
}

TEST_CASE(BlockRegistry_DuplicateThrows) {
    BlockRegistry registry;
    BlockType stone;
    stone.identifier = "rigel:stone";
    registry.registerBlock(stone.identifier, stone);

    CHECK_THROWS(registry.registerBlock(stone.identifier, stone));
}
