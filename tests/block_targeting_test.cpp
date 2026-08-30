#include "TestFramework.h"

#include "Rigel/Voxel/BlockRegistry.h"
#include "Rigel/Voxel/BlockTargeting.h"
#include "Rigel/Voxel/World.h"
#include "Rigel/Voxel/WorldResources.h"

namespace {
using namespace Rigel::Voxel;

struct TargetingFixture {
    WorldResources resources;
    BlockID solidId;
    World world;

    TargetingFixture()
        : solidId([this] {
              BlockType solid;
              const std::string identifier = "invented:target";
              solid.identifier = identifier;
              return resources.registry().registerBlock(
                  identifier, std::move(solid));
          }())
        , world(resources) {}
};

} // namespace

TEST_CASE(BlockTargeting_ReturnsWholeCellStateNormalAndDistance) {
    TargetingFixture fixture;
    fixture.world.setBlock(-2, 1, 0, BlockState{fixture.solidId});

    const auto target = raycastBlock(
        fixture.world,
        glm::vec3{1.5f, 1.25f, 0.5f},
        glm::vec3{-4.0f, 0.0f, 0.0f},
        8.0f);

    CHECK(target.has_value());
    CHECK_EQ(target->block, (glm::ivec3{-2, 1, 0}));
    CHECK_EQ(target->normal, (glm::ivec3{1, 0, 0}));
    CHECK_EQ(target->state.id, fixture.solidId);
    CHECK_NEAR(target->distance, 2.5f, 0.0001f);
}

TEST_CASE(BlockTargeting_StartingCellHasZeroDistanceAndNormal) {
    TargetingFixture fixture;
    fixture.world.setBlock(3, -1, 5, BlockState{fixture.solidId});

    const auto target = raycastBlock(
        fixture.world,
        glm::vec3{3.25f, -0.25f, 5.75f},
        glm::vec3{0.2f, 1.0f, -0.4f},
        0.0f);

    CHECK(target.has_value());
    CHECK_EQ(target->block, (glm::ivec3{3, -1, 5}));
    CHECK_EQ(target->normal, (glm::ivec3{0, 0, 0}));
    CHECK_EQ(target->distance, 0.0f);
}

TEST_CASE(BlockTargeting_RejectsDegenerateAndOutOfRangeRays) {
    TargetingFixture fixture;
    fixture.world.setBlock(0, 0, -3, BlockState{fixture.solidId});

    CHECK(!raycastBlock(
        fixture.world,
        glm::vec3{0.5f, 0.5f, 0.5f},
        glm::vec3{0.0f},
        8.0f));
    CHECK(!raycastBlock(
        fixture.world,
        glm::vec3{0.5f, 0.5f, 0.5f},
        glm::vec3{0.0f, 0.0f, -1.0f},
        2.4f));

    const auto boundaryTarget = raycastBlock(
        fixture.world,
        glm::vec3{0.5f, 0.5f, 0.5f},
        glm::vec3{0.0f, 0.0f, -1.0f},
        2.5f);
    CHECK(boundaryTarget.has_value());
    CHECK_EQ(boundaryTarget->block, (glm::ivec3{0, 0, -3}));
}
