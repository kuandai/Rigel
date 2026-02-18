#pragma once

namespace Rigel::Voxel {

struct VoxelUploadBudget {
    int limit = 0;
    int used = 0;
};

// Limit <= 0 means unlimited uploads.
VoxelUploadBudget beginVoxelUploadBudget(int configuredLimit);
bool consumeVoxelUploadBudget(VoxelUploadBudget& budget);

} // namespace Rigel::Voxel
