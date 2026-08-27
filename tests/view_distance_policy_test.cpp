#include "TestFramework.h"

#include "Rigel/Voxel/Chunk.h"
#include "Rigel/Voxel/ViewDistancePolicy.h"

using Rigel::Voxel::Chunk;
using Rigel::Voxel::ViewDistancePolicy;

TEST_CASE(ViewDistancePolicy_DerivesOneCompleteEffectivePolicy) {
    const auto policy = ViewDistancePolicy::derive(16, 8, 7);

    CHECK_EQ(policy->viewDistanceChunks(), 16);
    CHECK_EQ(policy->desiredRadiusChunks(), 16);
    CHECK_EQ(policy->unloadRadiusChunks(), 17);
    CHECK_NEAR(
        policy->renderDistanceWorldUnits(),
        static_cast<float>(17 * Chunk::SIZE),
        0.0001f);
    CHECK_NEAR(
        policy->projectionFarPlaneWorldUnits(),
        policy->renderDistanceWorldUnits() + static_cast<float>(Chunk::SIZE),
        0.0001f);
    CHECK_EQ(policy->preloadRadiusRegions(), 2);
    CHECK_EQ(policy->preloadRegionsPerRequest(), static_cast<size_t>(12));
    CHECK_NEAR(
        policy->shadowDistanceCeilingWorldUnits(),
        policy->renderDistanceWorldUnits(),
        0.0001f);
    CHECK_EQ(policy->generation(), static_cast<uint64_t>(7));
}

TEST_CASE(ViewDistancePolicy_ValidatesBeforeConstruction) {
    CHECK_THROWS(ViewDistancePolicy::derive(1, 8, 1));
    CHECK_THROWS(ViewDistancePolicy::derive(17, 8, 1));
    CHECK_THROWS(ViewDistancePolicy::derive(12, 0, 1));
    CHECK_THROWS(ViewDistancePolicy::derive(12, 8, 0));
}
