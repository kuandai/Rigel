#include "TestFramework.h"

#include "Rigel/Voxel/BlockRegistry.h"

#include <limits>
#include <memory>
#include <vector>

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

TEST_CASE(BlockRegistry_RejectsUnboundedTargetCandidateVolume) {
    BlockRegistry registry;
    BlockModelCuboid cuboid;
    cuboid.bounds = {
        {-std::numeric_limits<float>::max(), 0.0f, 0.0f},
        {std::numeric_limits<float>::max(), 1.0f, 1.0f}};
    cuboid.faces[static_cast<size_t>(Direction::PosX)] =
        BlockModelFace{.textureSlot = "invented"};

    BlockType extreme;
    extreme.identifier = "invented:extreme_overhang";
    extreme.model = std::make_shared<const BlockModel>(
        "invented:extreme_overhang_model",
        std::vector<std::string>{"invented"},
        std::vector<BlockModelCuboid>{cuboid});

    CHECK_THROWS(registry.registerBlock(extreme.identifier, extreme));
    CHECK_EQ(registry.size(), static_cast<size_t>(1));
    CHECK(!registry.modelExtents().has_value());
}
