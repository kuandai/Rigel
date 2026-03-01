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

TEST_CASE(BlockRegistry_SnapshotHash_StableForIdenticalContent) {
    BlockRegistry a;
    BlockType stoneA;
    stoneA.identifier = "rigel:stone";
    stoneA.model = "cube";
    stoneA.isOpaque = true;
    stoneA.isSolid = true;
    stoneA.cullSameType = false;
    stoneA.layer = RenderLayer::Opaque;
    stoneA.emittedLight = 0;
    stoneA.lightAttenuation = 15;
    stoneA.textures = FaceTextures::uniform("textures/blocks/stone.png");
    a.registerBlock(stoneA.identifier, stoneA);

    BlockRegistry b;
    BlockType stoneB = stoneA;
    b.registerBlock(stoneB.identifier, stoneB);

    CHECK_EQ(a.snapshotHash(), b.snapshotHash());
}

TEST_CASE(BlockRegistry_SnapshotHash_ChangesWhenSchemaFieldsChange) {
    BlockRegistry a;
    BlockType first;
    first.identifier = "rigel:test";
    first.model = "cube";
    first.isOpaque = true;
    first.isSolid = true;
    first.layer = RenderLayer::Opaque;
    first.lightAttenuation = 15;
    first.textures = FaceTextures::uniform("textures/blocks/a.png");
    a.registerBlock(first.identifier, first);

    BlockRegistry b;
    BlockType second = first;
    second.model = "cross";
    b.registerBlock(second.identifier, second);

    CHECK(a.snapshotHash() != b.snapshotHash());
}
