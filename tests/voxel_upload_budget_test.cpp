#include "TestFramework.h"

#include "Rigel/Voxel/VoxelLod/VoxelUploadBudget.h"

using namespace Rigel::Voxel;

TEST_CASE(VoxelUploadBudget_ZeroMeansUnlimited) {
    VoxelUploadBudget budget = beginVoxelUploadBudget(0);
    CHECK_EQ(budget.limit, 0);
    for (int i = 0; i < 16; ++i) {
        CHECK(consumeVoxelUploadBudget(budget));
    }
}

TEST_CASE(VoxelUploadBudget_PositiveLimitCapsUploads) {
    VoxelUploadBudget budget = beginVoxelUploadBudget(3);
    CHECK_EQ(budget.limit, 3);
    CHECK(consumeVoxelUploadBudget(budget));
    CHECK(consumeVoxelUploadBudget(budget));
    CHECK(consumeVoxelUploadBudget(budget));
    CHECK(!consumeVoxelUploadBudget(budget));
}

TEST_CASE(VoxelUploadBudget_NegativeIsSanitizedToUnlimited) {
    VoxelUploadBudget budget = beginVoxelUploadBudget(-5);
    CHECK_EQ(budget.limit, 0);
    CHECK(consumeVoxelUploadBudget(budget));
}
