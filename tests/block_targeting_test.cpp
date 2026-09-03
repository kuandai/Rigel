#include "TestFramework.h"

#include "Rigel/Voxel/BlockRegistry.h"
#include "Rigel/Voxel/BlockTargeting.h"
#include "Rigel/Voxel/RayAabb.h"
#include "Rigel/Voxel/World.h"
#include "Rigel/Voxel/WorldResources.h"

#include <array>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace Rigel::Voxel;

BlockModelFace face() {
    return BlockModelFace{.textureSlot = "invented"};
}

BlockModelCuboid cuboid(
    BlockModelBounds bounds,
    std::initializer_list<Direction> faces = {
        Direction::PosX, Direction::NegX,
        Direction::PosY, Direction::NegY,
        Direction::PosZ, Direction::NegZ}
) {
    BlockModelCuboid result;
    result.bounds = bounds;
    for (Direction direction : faces) {
        result.faces[static_cast<size_t>(direction)] = face();
    }
    return result;
}

std::shared_ptr<const BlockModel> model(
    std::string identifier,
    std::vector<BlockModelCuboid> cuboids
) {
    return std::make_shared<const BlockModel>(
        std::move(identifier),
        std::vector<std::string>{"invented"},
        std::move(cuboids));
}

struct TargetingFixture {
    WorldResources resources;
    World world{resources};
    size_t nextIdentifier = 0;

    BlockID add(
        std::shared_ptr<const BlockModel> geometry,
        BlockModelOrientation orientation = BlockModelOrientation::Identity,
        BlockCollisionShape collision = BlockCollisionShape::fullCube()
    ) {
        BlockType type;
        const std::string identifier =
            "invented:target_" + std::to_string(nextIdentifier++);
        type.identifier = identifier;
        type.model = BlockModelInstance(std::move(geometry));
        type.model.orientation = orientation;
        type.collision = std::move(collision);
        return resources.registry().registerBlock(
            identifier, std::move(type));
    }

    BlockID addFullCube(
        BlockCollisionShape collision = BlockCollisionShape::fullCube()
    ) {
        return add(BlockModel::fullCube(),
                   BlockModelOrientation::Identity,
                   std::move(collision));
    }
};

glm::ivec3 normal(Direction direction) {
    switch (direction) {
        case Direction::PosX: return {1, 0, 0};
        case Direction::NegX: return {-1, 0, 0};
        case Direction::PosY: return {0, 1, 0};
        case Direction::NegY: return {0, -1, 0};
        case Direction::PosZ: return {0, 0, 1};
        case Direction::NegZ: return {0, 0, -1};
    }
    return {};
}

void checkPosition(const glm::vec3& actual, const glm::vec3& expected) {
    CHECK_NEAR(actual.x, expected.x, 0.00001f);
    CHECK_NEAR(actual.y, expected.y, 0.00001f);
    CHECK_NEAR(actual.z, expected.z, 0.00001f);
}

} // namespace

TEST_CASE(BlockTargeting_FullCubeReturnsExactSurfaceData) {
    TargetingFixture fixture;
    const BlockID cube = fixture.addFullCube();
    fixture.world.setBlock(-2, 1, 0, BlockState{cube});

    const auto target = raycastBlock(
        fixture.world,
        glm::vec3{1.5f, 1.25f, 0.5f},
        glm::vec3{-4.0f, 0.0f, 0.0f},
        8.0f);

    CHECK(target.has_value());
    CHECK_EQ(target->block, (glm::ivec3{-2, 1, 0}));
    CHECK_EQ(target->normal, (glm::ivec3{1, 0, 0}));
    CHECK_EQ(target->face, Direction::PosX);
    CHECK_EQ(target->state.id, cube);
    CHECK_EQ(target->cuboidIndex, static_cast<size_t>(0));
    CHECK_NEAR(target->distance, 2.5f, 0.00001f);
    checkPosition(target->position, {-1.0f, 1.25f, 0.5f});
}

TEST_CASE(BlockTargeting_FullCubeTracksNearParallelTangentMotion) {
    const glm::vec3 origin{-8.0f, 0.99995f, 0.5f};
    const glm::vec3 direction{
        1.0f, BlockRayIntersectionTolerance, 0.0f};

    TargetingFixture fullCubeFixture;
    const BlockID fullCube = fullCubeFixture.addFullCube();
    fullCubeFixture.world.setBlock(0, 0, 0, BlockState{fullCube});
    const auto fullCubeHit = raycastBlock(
        fullCubeFixture.world, origin, direction, 8.0f);

    TargetingFixture declaredFacesFixture;
    const BlockID declaredFaces = declaredFacesFixture.add(model(
        "invented:declared_cube",
        {cuboid({{0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}})}));
    declaredFacesFixture.world.setBlock(
        0, 0, 0, BlockState{declaredFaces});
    const auto declaredFacesHit = raycastBlock(
        declaredFacesFixture.world, origin, direction, 8.0f);

    CHECK(!fullCubeHit);
    CHECK(!declaredFacesHit);
}

TEST_CASE(BlockTargeting_DeclaredFaceFollowsShallowIncidence) {
    TargetingFixture fixture;
    const BlockID shallowFace = fixture.add(model(
        "invented:shallow_face",
        {cuboid(
            {{0.0f, 0.0f, 0.0f}, {1.0f, 0.5f, 1.0f}},
            {Direction::NegY})}));
    fixture.world.setBlock(0, 0, 0, BlockState{shallowFace});

    const float shallowMotion =
        BlockRayIntersectionTolerance * 0.5f;
    const auto target = raycastBlock(
        fixture.world,
        {-0.5f, -shallowMotion * 0.5f, 0.5f},
        {1.0f, shallowMotion, 0.0f},
        1.0f);

    CHECK(target.has_value());
    CHECK_EQ(target->state.id, shallowFace);
    CHECK_EQ(target->face, Direction::NegY);
    CHECK_EQ(target->normal, (glm::ivec3{0, -1, 0}));
    CHECK_NEAR(target->distance, 0.5f, 0.00001f);
    checkPosition(target->position, {0.0f, 0.0f, 0.5f});
}

TEST_CASE(BlockTargeting_DdaAccumulatesEveryNonzeroAxis) {
    TargetingFixture fixture;
    const float shallowMotion =
        BlockRayIntersectionTolerance * 0.5f;
    const BlockID inset = fixture.add(model(
        "invented:inset_after_shallow_boundary",
        {cuboid(
            {{0.0f, BlockRayIntersectionTolerance * 2.0f, 0.25f},
             {1.0f, 0.5f, 0.75f}},
            {Direction::NegX})}));
    fixture.world.setBlock(4, 1, 0, BlockState{inset});

    const auto target = raycastBlock(
        fixture.world,
        {-0.5f, 1.0f - shallowMotion * 0.5f, 0.5f},
        {1.0f, shallowMotion, 0.0f},
        5.0f);

    CHECK(target.has_value());
    CHECK_EQ(target->state.id, inset);
    CHECK_EQ(target->block, (glm::ivec3{4, 1, 0}));
    CHECK_EQ(target->face, Direction::NegX);
    CHECK_NEAR(target->distance, 4.5f, 0.00001f);
    CHECK_NEAR(
        target->position.y,
        1.0f + BlockRayIntersectionTolerance * 2.0f,
        0.00001f);
}

TEST_CASE(BlockTargeting_InsideOriginSelectsNearestForwardExit) {
    TargetingFixture fixture;
    const BlockID cube = fixture.addFullCube();
    fixture.world.setBlock(3, -1, 5, BlockState{cube});

    const auto target = raycastBlock(
        fixture.world,
        glm::vec3{3.25f, -0.25f, 5.75f},
        glm::vec3{4.0f, 0.0f, 0.0f},
        0.75f);

    CHECK(target.has_value());
    CHECK_EQ(target->block, (glm::ivec3{3, -1, 5}));
    CHECK_EQ(target->normal, (glm::ivec3{1, 0, 0}));
    CHECK_EQ(target->face, Direction::PosX);
    CHECK_NEAR(target->distance, 0.75f, 0.00001f);
    checkPosition(target->position, {4.0f, -0.25f, 5.75f});

    CHECK(!raycastBlock(
        fixture.world,
        glm::vec3{3.25f, -0.25f, 5.75f},
        glm::vec3{1.0f, 0.0f, 0.0f},
        0.7f));
}

TEST_CASE(BlockTargeting_BoundaryOriginsHaveDeterministicSurfaceHits) {
    TargetingFixture fixture;
    const BlockID cube = fixture.addFullCube();
    fixture.world.setBlock(3, -1, 5, BlockState{cube});

    for (float direction : {-1.0f, 1.0f}) {
        const auto target = raycastBlock(
            fixture.world,
            glm::vec3{3.0f, -0.25f, 5.75f},
            glm::vec3{direction, 0.0f, 0.0f},
            0.0f);
        CHECK(target.has_value());
        CHECK_EQ(target->face, Direction::NegX);
        CHECK_EQ(target->normal, (glm::ivec3{-1, 0, 0}));
        CHECK_EQ(target->distance, 0.0f);
        checkPosition(target->position, {3.0f, -0.25f, 5.75f});
    }
}

TEST_CASE(BlockTargeting_EdgeTiesAcrossNegativeOwnersAreStable) {
    TargetingFixture fixture;
    const BlockID cube = fixture.addFullCube();
    fixture.world.setBlock(-1, 0, -1, BlockState{cube});
    fixture.world.setBlock(0, -1, -1, BlockState{cube});

    const auto target = raycastBlock(
        fixture.world, {0.5f, 0.5f, -0.5f}, {-1.0f, -1.0f, 0.0f}, 2.0f);

    CHECK(target.has_value());
    CHECK_EQ(target->block, (glm::ivec3{-1, 0, -1}));
    CHECK_EQ(target->face, Direction::PosX);
    CHECK_EQ(target->normal, (glm::ivec3{1, 0, 0}));
    CHECK_NEAR(target->distance, 0.70710677f, 0.00001f);
    checkPosition(target->position, {0.0f, 0.0f, -0.5f});
}

TEST_CASE(BlockTargeting_IntersectsSlabsPostsAndStairs) {
    TargetingFixture fixture;
    const BlockID slab = fixture.add(model(
        "invented:slab_model",
        {cuboid({{0.0f, 0.0f, 0.0f}, {1.0f, 0.5f, 1.0f}})}));
    const BlockID post = fixture.add(model(
        "invented:post_model",
        {cuboid({{0.375f, 0.0f, 0.375f},
                 {0.625f, 1.0f, 0.625f}})}));
    const BlockID stairs = fixture.add(model(
        "invented:stairs_model",
        {
            cuboid({{0.0f, 0.0f, 0.0f}, {1.0f, 0.5f, 1.0f}}),
            cuboid({{0.5f, 0.5f, 0.0f}, {1.0f, 1.0f, 1.0f}}),
        }));
    fixture.world.setBlock(0, 0, 0, BlockState{slab});
    fixture.world.setBlock(2, 0, 0, BlockState{post});
    fixture.world.setBlock(4, 0, 0, BlockState{stairs});

    const auto slabHit = raycastBlock(
        fixture.world, {0.25f, 2.0f, 0.25f}, {0.0f, -1.0f, 0.0f}, 3.0f);
    CHECK(slabHit.has_value());
    CHECK_EQ(slabHit->state.id, slab);
    CHECK_EQ(slabHit->face, Direction::PosY);
    CHECK_NEAR(slabHit->distance, 1.5f, 0.00001f);

    const auto postHit = raycastBlock(
        fixture.world, {1.0f, 0.75f, 0.5f}, {1.0f, 0.0f, 0.0f}, 3.0f);
    CHECK(postHit.has_value());
    CHECK_EQ(postHit->state.id, post);
    CHECK_EQ(postHit->face, Direction::NegX);
    CHECK_NEAR(postHit->position.x, 2.375f, 0.00001f);

    const auto lowerStep = raycastBlock(
        fixture.world, {4.25f, 2.0f, 0.5f}, {0.0f, -1.0f, 0.0f}, 3.0f);
    CHECK(lowerStep.has_value());
    CHECK_EQ(lowerStep->state.id, stairs);
    CHECK_EQ(lowerStep->cuboidIndex, static_cast<size_t>(0));
    CHECK_NEAR(lowerStep->position.y, 0.5f, 0.00001f);

    const auto upperStep = raycastBlock(
        fixture.world, {4.75f, 2.0f, 0.5f}, {0.0f, -1.0f, 0.0f}, 3.0f);
    CHECK(upperStep.has_value());
    CHECK_EQ(upperStep->state.id, stairs);
    CHECK_EQ(upperStep->cuboidIndex, static_cast<size_t>(1));
    CHECK_NEAR(upperStep->position.y, 1.0f, 0.00001f);
}

TEST_CASE(BlockTargeting_OccupiedEmptySpaceReachesBackgroundSurface) {
    TargetingFixture fixture;
    const BlockID slab = fixture.add(model(
        "invented:pass_through_slab",
        {cuboid({{0.0f, 0.0f, 0.0f}, {1.0f, 0.5f, 1.0f}})}));
    const BlockID gap = fixture.add(model(
        "invented:pass_through_gap",
        {
            cuboid({{0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 0.25f}}),
            cuboid({{0.0f, 0.0f, 0.75f}, {1.0f, 1.0f, 1.0f}}),
        }));
    const BlockID background = fixture.addFullCube();
    fixture.world.setBlock(0, 0, 0, BlockState{slab});
    fixture.world.setBlock(2, 0, 0, BlockState{background});
    fixture.world.setBlock(5, 0, 0, BlockState{gap});
    fixture.world.setBlock(7, 0, 0, BlockState{background});

    const auto throughSlab = raycastBlock(
        fixture.world, {-1.0f, 0.75f, 0.5f}, {1.0f, 0.0f, 0.0f}, 4.0f);
    CHECK(throughSlab.has_value());
    CHECK_EQ(throughSlab->state.id, background);
    CHECK_EQ(throughSlab->block, (glm::ivec3{2, 0, 0}));
    CHECK_NEAR(throughSlab->distance, 3.0f, 0.00001f);

    const auto throughGap = raycastBlock(
        fixture.world, {4.0f, 0.5f, 0.5f}, {1.0f, 0.0f, 0.0f}, 4.0f);
    CHECK(throughGap.has_value());
    CHECK_EQ(throughGap->state.id, background);
    CHECK_EQ(throughGap->block, (glm::ivec3{7, 0, 0}));
    CHECK_NEAR(throughGap->distance, 3.0f, 0.00001f);
}

TEST_CASE(BlockTargeting_MultiCuboidSelectionIsNearestThenIndexStable) {
    TargetingFixture fixture;
    const BlockID separated = fixture.add(model(
        "invented:separated_model",
        {
            cuboid({{0.75f, 0.0f, 0.0f}, {0.9f, 1.0f, 1.0f}}),
            cuboid({{0.2f, 0.0f, 0.0f}, {0.3f, 1.0f, 1.0f}}),
        }));
    const BlockModelBounds overlapBounds{
        {0.25f, 0.0f, 0.0f}, {0.75f, 1.0f, 1.0f}};
    const BlockID overlapping = fixture.add(model(
        "invented:overlapping_model",
        {cuboid(overlapBounds), cuboid(overlapBounds)}));
    fixture.world.setBlock(0, 0, 0, BlockState{separated});
    fixture.world.setBlock(2, 0, 0, BlockState{overlapping});

    const auto separatedHit = raycastBlock(
        fixture.world, {-1.0f, 0.5f, 0.5f}, {1.0f, 0.0f, 0.0f}, 2.0f);
    CHECK(separatedHit.has_value());
    CHECK_EQ(separatedHit->state.id, separated);
    CHECK_EQ(separatedHit->cuboidIndex, static_cast<size_t>(1));
    CHECK_NEAR(separatedHit->position.x, 0.2f, 0.00001f);

    const auto overlapHit = raycastBlock(
        fixture.world, {1.0f, 0.5f, 0.5f}, {1.0f, 0.0f, 0.0f}, 2.0f);
    CHECK(overlapHit.has_value());
    CHECK_EQ(overlapHit->state.id, overlapping);
    CHECK_EQ(overlapHit->cuboidIndex, static_cast<size_t>(0));
}

TEST_CASE(BlockTargeting_MissingEntryFaceDoesNotSelectBackFace) {
    TargetingFixture fixture;
    const BlockID open = fixture.add(model(
        "invented:open_model",
        {cuboid(
            {{0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}},
            {Direction::NegZ})}));
    const BlockID cube = fixture.addFullCube();
    fixture.world.setBlock(0, 0, 0, BlockState{open});
    fixture.world.setBlock(0, 0, -2, BlockState{cube});

    const auto target = raycastBlock(
        fixture.world, {0.5f, 0.5f, 2.0f}, {0.0f, 0.0f, -1.0f}, 5.0f);
    CHECK(target.has_value());
    CHECK_EQ(target->state.id, cube);
    CHECK_EQ(target->block, (glm::ivec3{0, 0, -2}));
    CHECK_NEAR(target->distance, 3.0f, 0.00001f);
}

TEST_CASE(BlockTargeting_ZeroThicknessTwoSidedSurfaceIsSelectable) {
    TargetingFixture fixture;
    const BlockID plane = fixture.add(
        model(
            "invented:two_sided_plane",
            {cuboid(
                {{0.5f, 0.0f, 0.0f}, {0.5f, 1.0f, 1.0f}},
                {Direction::PosX, Direction::NegX})}),
        BlockModelOrientation::Identity,
        BlockCollisionShape::empty());
    fixture.world.setBlock(0, 0, 0, BlockState{plane});

    CHECK(fixture.resources.registry().getType(plane).collision.isEmpty());
    const auto fromNegative = raycastBlock(
        fixture.world, {-1.0f, 0.5f, 0.5f}, {1.0f, 0.0f, 0.0f}, 3.0f);
    CHECK(fromNegative.has_value());
    CHECK_EQ(fromNegative->face, Direction::NegX);
    CHECK_EQ(fromNegative->normal, (glm::ivec3{-1, 0, 0}));
    CHECK_NEAR(fromNegative->distance, 1.5f, 0.00001f);

    const auto fromPositive = raycastBlock(
        fixture.world, {2.0f, 0.5f, 0.5f}, {-1.0f, 0.0f, 0.0f}, 3.0f);
    CHECK(fromPositive.has_value());
    CHECK_EQ(fromPositive->face, Direction::PosX);
    CHECK_EQ(fromPositive->normal, (glm::ivec3{1, 0, 0}));
}

TEST_CASE(BlockTargeting_EmptyModelsNeverHit) {
    TargetingFixture fixture;
    const BlockID empty = fixture.add(BlockModel::empty());
    const BlockID cube = fixture.addFullCube();
    fixture.world.setBlock(0, 0, 0, BlockState{empty});
    fixture.world.setBlock(2, 0, 0, BlockState{cube});

    const auto target = raycastBlock(
        fixture.world, {-1.0f, 0.5f, 0.5f}, {1.0f, 0.0f, 0.0f}, 5.0f);
    CHECK(target.has_value());
    CHECK_EQ(target->state.id, cube);
    CHECK_EQ(target->block, (glm::ivec3{2, 0, 0}));
}

TEST_CASE(BlockTargeting_UsesEverySupportedOrthogonalOrientation) {
    struct OrientationCase {
        BlockModelOrientation orientation;
        BlockModelBounds bounds;
        Direction face;
    };
    constexpr std::array cases = {
        OrientationCase{
            BlockModelOrientation::Identity,
            {{0.1f, 0.2f, 0.3f}, {0.4f, 0.6f, 0.8f}},
            Direction::PosX},
        OrientationCase{
            BlockModelOrientation::RotateX90,
            {{0.1f, 0.3f, 0.4f}, {0.4f, 0.8f, 0.8f}},
            Direction::PosX},
        OrientationCase{
            BlockModelOrientation::RotateX270,
            {{0.1f, 0.2f, 0.2f}, {0.4f, 0.7f, 0.6f}},
            Direction::PosX},
        OrientationCase{
            BlockModelOrientation::RotateY90,
            {{0.2f, 0.2f, 0.1f}, {0.7f, 0.6f, 0.4f}},
            Direction::PosZ},
        OrientationCase{
            BlockModelOrientation::RotateY180,
            {{0.6f, 0.2f, 0.2f}, {0.9f, 0.6f, 0.7f}},
            Direction::NegX},
        OrientationCase{
            BlockModelOrientation::RotateY270,
            {{0.3f, 0.2f, 0.6f}, {0.8f, 0.6f, 0.9f}},
            Direction::NegZ},
        OrientationCase{
            BlockModelOrientation::RotateZ90,
            {{0.2f, 0.6f, 0.3f}, {0.6f, 0.9f, 0.8f}},
            Direction::NegY},
    };

    for (size_t index = 0; index < cases.size(); ++index) {
        const OrientationCase& item = cases[index];
        TargetingFixture fixture;
        const BlockID oriented = fixture.add(
            model(
                "invented:oriented_" + std::to_string(index),
                {cuboid(
                    {{0.1f, 0.2f, 0.3f}, {0.4f, 0.6f, 0.8f}},
                    {Direction::PosX})}),
            item.orientation);
        fixture.world.setBlock(0, 0, 0, BlockState{oriented});

        const glm::ivec3 expectedNormal = normal(item.face);
        glm::vec3 expectedPosition{};
        for (size_t axis = 0; axis < 3; ++axis) {
            expectedPosition[axis] =
                (item.bounds.min[axis] + item.bounds.max[axis]) * 0.5f;
        }
        const size_t normalAxis = static_cast<size_t>(item.face) / 2;
        expectedPosition[normalAxis] =
            static_cast<size_t>(item.face) % 2 == 0
                ? item.bounds.max[normalAxis]
                : item.bounds.min[normalAxis];
        const glm::vec3 rayOrigin =
            expectedPosition + glm::vec3(expectedNormal);
        const glm::vec3 rayDirection = -glm::vec3(expectedNormal);

        const auto target = raycastBlock(
            fixture.world, rayOrigin, rayDirection, 1.0f);
        CHECK(target.has_value());
        CHECK_EQ(target->state.id, oriented);
        CHECK_EQ(target->face, item.face);
        CHECK_EQ(target->normal, expectedNormal);
        CHECK_NEAR(target->distance, 1.0f, 0.00001f);
        checkPosition(target->position, expectedPosition);
    }
}

TEST_CASE(BlockTargeting_TargetBoundsUseOrientedSelectedCuboid) {
    TargetingFixture fixture;
    const BlockID oriented = fixture.add(
        model(
            "invented:outline_orientation",
            {cuboid({{0.0f, 0.25f, 0.1f},
                     {0.25f, 0.75f, 0.9f}})}),
        BlockModelOrientation::RotateY90);
    const BlockTarget target{
        .block = {4, -2, 7},
        .state = BlockState{oriented},
        .cuboidIndex = 0,
    };

    const auto bounds = blockTargetBounds(
        fixture.resources.registry(), target);

    CHECK(bounds.has_value());
    CHECK_EQ(bounds->min, (std::array<float, 3>{4.1f, -1.75f, 7.0f}));
    CHECK_EQ(bounds->max, (std::array<float, 3>{4.9f, -1.25f, 7.25f}));

    BlockTarget invalid = target;
    invalid.cuboidIndex = 1;
    CHECK(!blockTargetBounds(fixture.resources.registry(), invalid));
}

TEST_CASE(BlockTargeting_DdaFindsOverhangingOwnerAndGlobalNearestSurface) {
    TargetingFixture fixture;
    const BlockID nearOverhang = fixture.add(model(
        "invented:near_overhang",
        {cuboid({{-0.25f, 0.25f, 0.25f},
                 {0.25f, 0.75f, 0.75f}})}));
    const BlockID fartherSameCell = fixture.add(model(
        "invented:farther_same_cell",
        {cuboid({{0.9f, 0.25f, 0.25f},
                 {1.0f, 0.75f, 0.75f}})}));
    fixture.world.setBlock(1, 0, 0, BlockState{nearOverhang});
    fixture.world.setBlock(0, 0, 0, BlockState{fartherSameCell});

    const auto target = raycastBlock(
        fixture.world, {0.5f, 0.5f, 0.5f}, {1.0f, 0.0f, 0.0f}, 2.0f);
    CHECK(target.has_value());
    CHECK_EQ(target->state.id, nearOverhang);
    CHECK_EQ(target->block, (glm::ivec3{1, 0, 0}));
    CHECK_EQ(target->face, Direction::NegX);
    CHECK_NEAR(target->distance, 0.25f, 0.00001f);
    CHECK_NEAR(target->position.x, 0.75f, 0.00001f);

    const auto& extents = fixture.resources.registry().modelExtents();
    CHECK(extents.has_value());
    CHECK_EQ(extents->min[0], -0.25f);
    CHECK_EQ(extents->max[0], 1.0f);
}

TEST_CASE(BlockTargeting_DdaWaitsForLaterOwnerBeforeStopping) {
    TargetingFixture fixture;
    const BlockID fartherKnownHit = fixture.add(model(
        "invented:known_farther_surface",
        {cuboid({{0.9f, 0.25f, 0.25f},
                 {1.0f, 0.75f, 0.75f}})}));
    const BlockID laterOwner = fixture.add(model(
        "invented:later_owner_protrusion",
        {cuboid({{-0.25f, 0.25f, 0.25f},
                 {0.25f, 0.75f, 0.75f}})}));
    fixture.world.setBlock(1, 0, 0, BlockState{fartherKnownHit});
    fixture.world.setBlock(2, 0, 0, BlockState{laterOwner});

    const auto target = raycastBlock(
        fixture.world, {0.1f, 0.5f, 0.5f}, {1.0f, 0.0f, 0.0f}, 3.0f);

    CHECK(target.has_value());
    CHECK_EQ(target->state.id, laterOwner);
    CHECK_EQ(target->block, (glm::ivec3{2, 0, 0}));
    CHECK_EQ(target->face, Direction::NegX);
    CHECK_NEAR(target->distance, 1.65f, 0.00001f);
    CHECK_NEAR(target->position.x, 1.75f, 0.00001f);
}

TEST_CASE(BlockTargeting_FindsMeasuredForwardAndLateralOverhangs) {
    TargetingFixture fixture;
    const BlockID pistonLike = fixture.add(model(
        "invented:piston_like_overhang",
        {cuboid({{0.75f, 0.25f, 0.25f},
                 {1.25f, 0.75f, 0.75f}})}));
    const BlockID torchLike = fixture.add(
        model(
            "invented:wall_torch_scale_overhang",
            {cuboid({{-0.00625f, 0.25f, 0.25f},
                     {0.125f, 0.75f, 0.75f}})}),
        BlockModelOrientation::Identity,
        BlockCollisionShape::empty());
    const BlockID background = fixture.addFullCube();
    fixture.world.setBlock(0, 0, 4, BlockState{pistonLike});
    fixture.world.setBlock(1, 0, -2, BlockState{torchLike});
    fixture.world.setBlock(0, 0, -3, BlockState{background});

    const auto pistonTarget = raycastBlock(
        fixture.world, {2.0f, 0.5f, 4.5f}, {-1.0f, 0.0f, 0.0f}, 2.0f);
    CHECK(pistonTarget.has_value());
    CHECK_EQ(pistonTarget->state.id, pistonLike);
    CHECK_EQ(pistonTarget->block, (glm::ivec3{0, 0, 4}));
    CHECK_EQ(pistonTarget->face, Direction::PosX);
    CHECK_NEAR(pistonTarget->distance, 0.75f, 0.00001f);

    const auto torchTarget = raycastBlock(
        fixture.world, {0.995f, 0.5f, 0.5f},
        {0.0f, 0.0f, -1.0f}, 4.0f);
    CHECK(torchTarget.has_value());
    CHECK_EQ(torchTarget->state.id, torchLike);
    CHECK_EQ(torchTarget->block, (glm::ivec3{1, 0, -2}));
    CHECK_EQ(torchTarget->face, Direction::PosZ);
    CHECK_NEAR(torchTarget->distance, 1.75f, 0.00001f);
}

TEST_CASE(BlockTargeting_VisibleCollisionNoneModelWinsBeforeSolidBlock) {
    TargetingFixture fixture;
    const BlockID visible = fixture.add(
        model(
            "invented:visible_non_colliding",
            {cuboid({{0.25f, 0.0f, 0.0f},
                     {0.75f, 1.0f, 1.0f}})}),
        BlockModelOrientation::Identity,
        BlockCollisionShape::empty());
    const BlockID solid = fixture.addFullCube();
    fixture.world.setBlock(0, 0, -2, BlockState{visible});
    fixture.world.setBlock(0, 0, -3, BlockState{solid});

    const auto target = raycastBlock(
        fixture.world, {0.5f, 0.5f, 0.5f}, {0.0f, 0.0f, -1.0f}, 8.0f);
    CHECK(target.has_value());
    CHECK_EQ(target->state.id, visible);
    CHECK_EQ(target->block, (glm::ivec3{0, 0, -2}));
    CHECK_NEAR(target->distance, 1.5f, 0.00001f);
}

TEST_CASE(BlockTargeting_RejectsInvalidAndOutOfRangeRays) {
    TargetingFixture fixture;
    const BlockID cube = fixture.addFullCube();
    fixture.world.setBlock(0, 0, -3, BlockState{cube});

    const float infinity = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK(!raycastBlock(
        fixture.world, {0.5f, 0.5f, 0.5f},
        {0.0f, 0.0f, 0.0f}, 8.0f));
    CHECK(!raycastBlock(
        fixture.world, {0.5f, 0.5f, 0.5f},
        {BlockRayIntersectionTolerance * 0.5f, 0.0f, 0.0f}, 8.0f));
    CHECK(!raycastBlock(
        fixture.world, {nan, 0.5f, 0.5f}, {0.0f, 0.0f, -1.0f}, 8.0f));
    CHECK(!raycastBlock(
        fixture.world, {0.5f, 0.5f, 0.5f}, {0.0f, infinity, -1.0f}, 8.0f));
    CHECK(!raycastBlock(
        fixture.world, {0.5f, 0.5f, 0.5f}, {0.0f, 0.0f, -1.0f}, nan));
    CHECK(!raycastBlock(
        fixture.world, {0.5f, 0.5f, 0.5f}, {0.0f, 0.0f, -1.0f}, -1.0f));
    CHECK(!raycastBlock(
        fixture.world,
        {static_cast<float>(std::numeric_limits<int>::max()), 0.5f, 0.5f},
        {-1.0f, 0.0f, 0.0f}, 8.0f));
    CHECK(!raycastBlock(
        fixture.world, {0.5f, 0.5f, 0.5f}, {1.0f, 0.0f, 0.0f},
        std::numeric_limits<float>::max()));
    CHECK(!raycastBlock(
        fixture.world, {0.5f, 0.5f, 0.5f}, {0.0f, 0.0f, -1.0f}, 2.4f));

    const auto boundaryTarget = raycastBlock(
        fixture.world, {0.5f, 0.5f, 0.5f}, {0.0f, 0.0f, -1.0f}, 2.5f);
    CHECK(boundaryTarget.has_value());
    CHECK_EQ(boundaryTarget->block, (glm::ivec3{0, 0, -3}));
    CHECK_NEAR(boundaryTarget->distance, 2.5f, 0.00001f);
}
