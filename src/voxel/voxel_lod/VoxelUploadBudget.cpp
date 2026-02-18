#include "Rigel/Voxel/VoxelLod/VoxelUploadBudget.h"

#include <algorithm>

namespace Rigel::Voxel {

VoxelUploadBudget beginVoxelUploadBudget(int configuredLimit) {
    VoxelUploadBudget budget{};
    budget.limit = std::max(0, configuredLimit);
    budget.used = 0;
    return budget;
}

bool consumeVoxelUploadBudget(VoxelUploadBudget& budget) {
    if (budget.limit == 0) {
        return true;
    }
    if (budget.used >= budget.limit) {
        return false;
    }
    ++budget.used;
    return true;
}

} // namespace Rigel::Voxel
