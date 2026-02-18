#pragma once

#include "Rigel/Voxel/VoxelLod/VoxelPageCpu.h"

#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <vector>

namespace Rigel::Voxel {

enum class VoxelSvoNodeKind : uint8_t {
    Empty = 0,
    Solid = 1,
    Mixed = 2,
};

enum class VoxelMaterialClass : uint8_t {
    Air = 0,
    Opaque = 1,
    Cutout = 2,
    Transparent = 3,
};

struct VoxelSvoNode {
    static constexpr uint32_t kInvalidChild = std::numeric_limits<uint32_t>::max();

    VoxelSvoNodeKind kind = VoxelSvoNodeKind::Empty;
    VoxelMaterialClass materialClass = VoxelMaterialClass::Air;
    VoxelId materialId = 0;
    uint16_t leafSizeVoxels = 0; // power-of-two in L0 voxels for leaf nodes
    uint8_t childMask = 0; // bit i set when children[i] is valid
    std::array<uint32_t, 8> children = {
        kInvalidChild, kInvalidChild, kInvalidChild, kInvalidChild,
        kInvalidChild, kInvalidChild, kInvalidChild, kInvalidChild
    };

    bool isLeaf() const { return kind != VoxelSvoNodeKind::Mixed; }
};

struct VoxelPageTree {
    VoxelPageKey key{};
    int dim = 0;
    int minLeafVoxels = 1;
    uint32_t root = VoxelSvoNode::kInvalidChild;
    std::vector<VoxelSvoNode> nodes;

    bool empty() const { return nodes.empty() || root == VoxelSvoNode::kInvalidChild; }
    size_t cpuBytes() const { return nodes.size() * sizeof(VoxelSvoNode); }
};

using VoxelMaterialClassifier = std::function<VoxelMaterialClass(VoxelId)>;

// Build an adaptive voxel SVO tree over a page-sized mip pyramid.
//
// - Uniform mip cells collapse to an Empty/Solid leaf.
// - Mixed regions subdivide until `minLeafVoxels` (power-of-two) and then emit a coarse leaf using
//   the mip cell representative value.
//
// Empty regions are omitted from the tree (except when the entire page is empty, in which case the
// root is a single Empty leaf).
VoxelPageTree buildVoxelPageTree(const VoxelPageCpu& page,
                                 int minLeafVoxels,
                                 const VoxelMaterialClassifier& classify);

} // namespace Rigel::Voxel

