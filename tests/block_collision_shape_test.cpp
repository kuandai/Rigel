#include "TestFramework.h"

#include "Rigel/Voxel/BlockCollisionShape.h"
#include "Rigel/Voxel/BlockRegistry.h"

#include <limits>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using namespace Rigel::Voxel;

namespace {

std::vector<BlockCollisionBox> distinctCollisionBoxes(size_t count) {
    std::vector<BlockCollisionBox> boxes;
    boxes.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        boxes.push_back({
            {-0.25f + static_cast<float>(index) * 0.01f, 0.0f, 0.0f},
            {1.25f, 1.0f, 1.0f},
        });
    }
    return boxes;
}

} // namespace

static_assert(std::is_same_v<
              decltype(std::declval<const BlockCollisionShape&>().boxes()),
              std::span<const BlockCollisionBox>>);
static_assert(!std::is_assignable_v<
              decltype(std::declval<const BlockCollisionShape&>().boxes()[0]),
              BlockCollisionBox>);
static_assert(std::is_nothrow_move_constructible_v<BlockCollisionShape>);
static_assert(std::is_nothrow_move_assignable_v<BlockCollisionShape>);

TEST_CASE(BlockCollisionShape_EmptyAndFullCubeUseCanonicalQueries) {
    const BlockCollisionShape empty = BlockCollisionShape::empty();
    CHECK(empty.isEmpty());
    CHECK(empty.boxes().empty());

    const BlockCollisionShape full = BlockCollisionShape::fullCube();
    CHECK(full.isFullCube());
    CHECK_EQ(full.boxes().size(), static_cast<size_t>(1));
    CHECK_EQ(
        full.boxes().front().min,
        (std::array<float, 3>{0.0f, 0.0f, 0.0f}));
    CHECK_EQ(
        full.boxes().front().max,
        (std::array<float, 3>{1.0f, 1.0f, 1.0f}));
    CHECK_EQ(full.boxes().data(), BlockCollisionShape::fullCube().boxes().data());

    const BlockCollisionShape legacyDefault;
    CHECK(legacyDefault.isFullCube());
    CHECK_EQ(
        legacyDefault.provenance(),
        BlockCollisionShape::Provenance::Authored);
}

TEST_CASE(BlockCollisionShape_CustomBoxesAreImmutableSnapshots) {
    std::vector<BlockCollisionBox> authored = {
        {{-0.25f, 0.0f, 0.25f}, {0.5f, 0.5f, 0.75f}},
        {{0.5f, 0.25f, 0.0f}, {1.25f, 1.0f, 1.0f}},
    };
    const BlockCollisionShape shape = BlockCollisionShape::boxes(authored);
    authored[0].min[0] = 0.0f;

    CHECK(shape.isBoxes());
    CHECK_EQ(shape.boxes().size(), static_cast<size_t>(2));
    CHECK_EQ(shape.boxes()[0].min[0], -0.25f);
    CHECK_EQ(shape.boxes()[1].max[0], 1.25f);

    const BlockCollisionShape copy = shape;
    CHECK_EQ(copy.boxes().data(), shape.boxes().data());

    BlockRegistry registry;
    CHECK(registry.getType(BlockRegistry::airId()).collision.isEmpty());
    BlockType block;
    block.collision = shape;
    const BlockID blockId = registry.registerBlock("test:custom_shape", block);
    block.collision = BlockCollisionShape::empty();

    const BlockCollisionShape& registered = registry.getType(blockId).collision;
    CHECK(registered.isBoxes());
    CHECK_EQ(registered.boxes().data(), shape.boxes().data());
    CHECK_EQ(registered.boxes()[0].min[0], -0.25f);
}

TEST_CASE(BlockCollisionShape_MoveAndCopyPreserveCollisionSemantics) {
    BlockCollisionShape source = BlockCollisionShape::boxes(
        {{{-0.25f, 0.0f, 0.25f}, {0.5f, 0.5f, 0.75f}},
         {{0.5f, 0.25f, 0.0f}, {1.25f, 1.0f, 1.0f}}},
        BlockCollisionShape::Provenance::Exact);
    const BlockCollisionShape copied = source;
    const BlockCollisionBox* const storage = source.boxes().data();

    BlockCollisionShape moved = std::move(source);
    CHECK(moved.isBoxes());
    CHECK_EQ(moved.provenance(), BlockCollisionShape::Provenance::Exact);
    CHECK_EQ(moved.boxes().data(), storage);
    CHECK_EQ(copied.boxes().data(), storage);
    CHECK(source.isEmpty());
    CHECK_EQ(source.kind(), BlockCollisionShape::Kind::Empty);
    CHECK(source.boxes().empty());

    BlockCollisionShape assigned = BlockCollisionShape::empty();
    assigned = std::move(moved);
    CHECK(assigned.isBoxes());
    CHECK_EQ(assigned.provenance(), BlockCollisionShape::Provenance::Exact);
    CHECK_EQ(assigned.boxes().data(), storage);
    CHECK(moved.isEmpty());
    CHECK_EQ(moved.kind(), BlockCollisionShape::Kind::Empty);
    CHECK(moved.boxes().empty());

    BlockCollisionShape copyAssigned;
    copyAssigned = assigned;
    CHECK_EQ(copyAssigned.boxes().data(), storage);
    CHECK_EQ(copyAssigned.boxes().size(), static_cast<size_t>(2));
    CHECK_EQ(copyAssigned.boxes()[0].min[0], -0.25f);
    CHECK_EQ(copyAssigned.boxes()[1].max[0], 1.25f);
}

TEST_CASE(BlockCollisionShape_MovedInputCannotAliasRegisteredBoxes) {
    std::vector<BlockCollisionBox> authored = {
        {{-0.25f, 0.0f, 0.25f}, {0.5f, 0.5f, 0.75f}},
    };
    BlockCollisionBox* retained = authored.data();
    const BlockCollisionShape shape =
        BlockCollisionShape::boxes(std::move(authored));

    BlockRegistry registry;
    BlockType block;
    block.collision = shape;
    const BlockID blockId =
        registry.registerBlock("test:moved_shape", std::move(block));

    retained->min[0] = 0.0f;
    retained->max[0] = 0.25f;

    const BlockCollisionShape& registered = registry.getType(blockId).collision;
    CHECK_EQ(registered.boxes()[0].min[0], -0.25f);
    CHECK_EQ(registered.boxes()[0].max[0], 0.5f);
}

TEST_CASE(BlockCollisionShape_ValidatesNormalizedBoxInvariants) {
    CHECK_THROWS(BlockCollisionShape::boxes({}));
    CHECK_THROWS(BlockCollisionShape::boxes({
        {{0.0f, 0.0f, 0.0f},
         {std::numeric_limits<float>::infinity(), 1.0f, 1.0f}}}));
    CHECK_THROWS(BlockCollisionShape::boxes({
        {{0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 1.0f}}}));
    CHECK_THROWS(BlockCollisionShape::boxes({
        {{0.5f, 0.0f, 0.0f}, {0.25f, 1.0f, 1.0f}}}));
    CHECK_THROWS(BlockCollisionShape::boxes({
        {{-0.2501f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}}}));
    CHECK_THROWS(BlockCollisionShape::boxes({
        {{0.0f, 0.0f, 0.0f}, {1.2501f, 1.0f, 1.0f}}}));

    const BlockCollisionBox repeated{
        {0.25f, 0.0f, 0.25f}, {0.75f, 0.5f, 0.75f}};
    CHECK_THROWS(BlockCollisionShape::boxes({repeated, repeated}));

    const BlockCollisionShape canonicalized = BlockCollisionShape::boxes({
        {{0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}}},
        BlockCollisionShape::Provenance::ConservativeFallback);
    CHECK(canonicalized.isFullCube());
    CHECK_EQ(
        canonicalized.provenance(),
        BlockCollisionShape::Provenance::ConservativeFallback);
}

TEST_CASE(BlockCollisionShape_BoundsImmutableBoxCardinality) {
    std::vector<BlockCollisionBox> boundary = distinctCollisionBoxes(
        BlockCollisionShape::MaximumBoxes);
    const BlockCollisionBox first = boundary.front();
    const BlockCollisionBox last = boundary.back();
    const BlockCollisionShape accepted =
        BlockCollisionShape::boxes(boundary);
    boundary.front().min[0] = 0.0f;
    boundary.back().max[0] = 0.5f;

    CHECK(accepted.isBoxes());
    CHECK_EQ(accepted.boxes().size(), BlockCollisionShape::MaximumBoxes);
    CHECK_EQ(accepted.boxes().front(), first);
    CHECK_EQ(accepted.boxes().back(), last);

    std::string diagnostic;
    try {
        BlockCollisionShape::boxes(distinctCollisionBoxes(
            BlockCollisionShape::MaximumBoxes + 1));
    } catch (const std::invalid_argument& error) {
        diagnostic = error.what();
    }
    CHECK(diagnostic.find("at most 16 boxes") != std::string::npos);
    CHECK(diagnostic.find("received 17") != std::string::npos);
}
