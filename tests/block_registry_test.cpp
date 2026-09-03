#include "TestFramework.h"

#include "Rigel/Voxel/BlockRegistry.h"

#include <limits>
#include <memory>
#include <vector>

using namespace Rigel::Voxel;

namespace {

BlockType modelType(
    const std::string& identifier,
    const BlockModelBounds& bounds
) {
    BlockModelCuboid cuboid;
    cuboid.bounds = bounds;
    cuboid.faces[static_cast<size_t>(Direction::PosX)] =
        BlockModelFace{.textureSlot = "invented"};

    BlockType type;
    type.identifier = identifier;
    type.model = std::make_shared<const BlockModel>(
        identifier + "_model",
        std::vector<std::string>{"invented"},
        std::vector<BlockModelCuboid>{cuboid});
    return type;
}

} // namespace

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

TEST_CASE(BlockRegistry_EnforcesTargetCandidateLimitAndBatchRollback) {
    BlockRegistry boundary;
    const std::string atLimit = "invented:target_limit_512";
    boundary.registerBlock(
        atLimit,
        modelType(atLimit, {{0.0f, 0.0f, 0.0f}, {5.0f, 5.0f, 5.0f}}));
    CHECK_EQ(boundary.size(), static_cast<size_t>(2));
    CHECK(boundary.hasIdentifier(atLimit));
    CHECK(boundary.modelExtents().has_value());
    if (!boundary.modelExtents()) {
        return;
    }
    for (size_t axis = 0; axis < 3; ++axis) {
        CHECK_EQ(boundary.modelExtents()->min[axis], 0.0f);
        CHECK_EQ(boundary.modelExtents()->max[axis], 5.0f);
    }

    const std::string aboveLimit = "invented:target_limit_513_plus";
    CHECK_THROWS(boundary.registerBlock(
        aboveLimit,
        modelType(
            aboveLimit,
            {{0.0f, 0.0f, 0.0f}, {6.0f, 5.0f, 5.0f}})));
    CHECK_EQ(boundary.size(), static_cast<size_t>(2));
    CHECK(!boundary.hasIdentifier(aboveLimit));
    CHECK(boundary.modelExtents().has_value());
    if (!boundary.modelExtents()) {
        return;
    }
    for (size_t axis = 0; axis < 3; ++axis) {
        CHECK_EQ(boundary.modelExtents()->min[axis], 0.0f);
        CHECK_EQ(boundary.modelExtents()->max[axis], 5.0f);
    }

    BlockRegistry batch;
    const std::string negative = "invented:batch_negative_extent";
    const std::string positive = "invented:batch_positive_extent";
    std::vector<std::pair<std::string, BlockType>> additions;
    additions.emplace_back(
        negative,
        modelType(
            negative,
            {{-2.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}}));
    additions.emplace_back(
        positive,
        modelType(
            positive,
            {{0.0f, 0.0f, 0.0f}, {4.0f, 5.0f, 5.0f}}));

    CHECK_THROWS(batch.registerBlocks(std::move(additions)));
    CHECK_EQ(batch.size(), static_cast<size_t>(1));
    CHECK(!batch.hasIdentifier(negative));
    CHECK(!batch.hasIdentifier(positive));
    CHECK(!batch.modelExtents().has_value());
}

TEST_CASE(BlockRegistry_AggregatesMeasuredOrientedVisualExtents) {
    BlockModelCuboid cuboid;
    cuboid.bounds = {
        {-0.25f, 0.0f, 0.0f}, {1.25f, 1.0f, 1.0f}};
    for (auto& modelFace : cuboid.faces) {
        modelFace = BlockModelFace{.textureSlot = "invented"};
    }
    const auto geometry = std::make_shared<const BlockModel>(
        "invented:oriented_overhang_model",
        std::vector<std::string>{"invented"},
        std::vector<BlockModelCuboid>{cuboid});

    BlockRegistry registry;
    const auto add = [&](const std::string& identifier,
                         BlockModelOrientation orientation) {
        BlockType type;
        type.model = BlockModelInstance(geometry);
        type.model.orientation = orientation;
        registry.registerBlock(identifier, std::move(type));
    };
    add("invented:x_overhang", BlockModelOrientation::Identity);
    add("invented:y_overhang", BlockModelOrientation::RotateZ90);
    add("invented:z_overhang", BlockModelOrientation::RotateY90);

    const auto& extents = registry.modelExtents();
    CHECK(extents.has_value());
    for (size_t axis = 0; axis < 3; ++axis) {
        CHECK_EQ(extents->min[axis], -0.25f);
        CHECK_EQ(extents->max[axis], 1.25f);
    }
}
