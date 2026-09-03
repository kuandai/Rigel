#include "TestFramework.h"

#include "Rigel/Voxel/RayAabb.h"

#include <limits>

namespace {
using namespace Rigel::Voxel;

constexpr glm::vec3 UnitMin{-1.0f, -2.0f, -3.0f};
constexpr glm::vec3 UnitMax{1.0f, 2.0f, 3.0f};

RayAabbInterval requireInterval(
    const glm::vec3& origin,
    const glm::vec3& direction,
    float maximum = 100.0f
) {
    const auto interval = intersectRayAabb(
        origin, direction, UnitMin, UnitMax, maximum);
    CHECK(interval.has_value());
    return *interval;
}

} // namespace

TEST_CASE(RayAabb_ReturnsStablePositiveAndNegativeIntervals) {
    const RayAabbInterval positive = requireInterval(
        {-4.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f});
    CHECK_NEAR(positive.entryDistance, 3.0f, 0.000001f);
    CHECK_NEAR(positive.exitDistance, 5.0f, 0.000001f);
    CHECK_EQ(positive.entryFace, Direction::NegX);
    CHECK_EQ(positive.exitFace, Direction::PosX);
    CHECK(!positive.startsInside);

    const RayAabbInterval negative = requireInterval(
        {4.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f});
    CHECK_NEAR(negative.entryDistance, 3.0f, 0.000001f);
    CHECK_NEAR(negative.exitDistance, 5.0f, 0.000001f);
    CHECK_EQ(negative.entryFace, Direction::PosX);
    CHECK_EQ(negative.exitFace, Direction::NegX);
}

TEST_CASE(RayAabb_HandlesParallelBoundaryAndNegativeCoordinates) {
    const glm::vec3 boundsMin{-5.0f, -4.0f, -3.0f};
    const glm::vec3 boundsMax{-4.0f, -2.0f, -1.0f};
    const auto boundary = intersectRayAabb(
        {-6.0f, -4.0f, -2.0f}, {1.0f, 0.0f, 0.0f},
        boundsMin, boundsMax, 2.0f);
    CHECK(boundary.has_value());
    CHECK_NEAR(boundary->entryDistance, 1.0f, 0.000001f);
    CHECK_EQ(boundary->entryFace, Direction::NegX);
    CHECK(!boundary->startsInside);

    CHECK(!intersectRayAabb(
        {-6.0f, -4.0f - BlockRayIntersectionTolerance * 2.0f, -2.0f},
        {1.0f, 0.0f, 0.0f}, boundsMin, boundsMax, 2.0f));
}

TEST_CASE(RayAabb_UsesDeterministicEdgeAndCornerNormals) {
    constexpr float diagonal = 0.57735026919f;
    const RayAabbInterval corner = requireInterval(
        {-4.0f, -5.0f, -6.0f},
        {diagonal, diagonal, diagonal});
    CHECK_EQ(corner.entryFace, Direction::NegX);
    CHECK_EQ(corner.exitFace, Direction::PosX);

    constexpr float edge = 0.70710678118f;
    const RayAabbInterval mixedEdge = requireInterval(
        {4.0f, -5.0f, 0.0f}, {-edge, edge, 0.0f});
    CHECK_EQ(mixedEdge.entryFace, Direction::PosX);
    CHECK_EQ(mixedEdge.exitFace, Direction::NegX);
}

TEST_CASE(RayAabb_ReturnsExitForInsideOrigins) {
    const RayAabbInterval inside = requireInterval(
        {0.0f, 0.0f, 0.0f}, {0.0f, -1.0f, 0.0f});
    CHECK(inside.startsInside);
    CHECK_NEAR(inside.entryDistance, -2.0f, 0.000001f);
    CHECK_NEAR(inside.exitDistance, 2.0f, 0.000001f);
    CHECK_EQ(inside.entryFace, Direction::PosY);
    CHECK_EQ(inside.exitFace, Direction::NegY);

    const RayAabbInterval boundary = requireInterval(
        {-1.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f});
    CHECK(!boundary.startsInside);
    CHECK_EQ(boundary.entryDistance, 0.0f);
    CHECK_EQ(boundary.entryFace, Direction::NegX);
}

TEST_CASE(RayAabb_AppliesInclusiveMaximumDistance) {
    CHECK(intersectRayAabb(
        {-4.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f},
        UnitMin, UnitMax, 3.0f));
    CHECK(!intersectRayAabb(
        {-4.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f},
        UnitMin, UnitMax,
        3.0f - BlockRayIntersectionTolerance * 2.0f));
}

TEST_CASE(RayAabb_RejectsInvalidFiniteContractInputs) {
    const float infinity = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK(!intersectRayAabb(
        {nan, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f},
        UnitMin, UnitMax, 3.0f));
    CHECK(!intersectRayAabb(
        {0.0f, 0.0f, 0.0f}, {infinity, 0.0f, 0.0f},
        UnitMin, UnitMax, 3.0f));
    CHECK(!intersectRayAabb(
        {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f},
        UnitMin, UnitMax, 3.0f));
    CHECK(!intersectRayAabb(
        {0.0f, 0.0f, 0.0f}, {2.0f, 0.0f, 0.0f},
        UnitMin, UnitMax, 3.0f));
    CHECK(!intersectRayAabb(
        {0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f},
        UnitMax, UnitMin, 3.0f));
    CHECK(!intersectRayAabb(
        {0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f},
        UnitMin, UnitMax, -1.0f));
}
