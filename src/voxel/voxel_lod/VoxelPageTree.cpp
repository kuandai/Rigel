#include "Rigel/Voxel/VoxelLod/VoxelPageTree.h"

#include <algorithm>

namespace Rigel::Voxel {
namespace {

int clampPow2(int value, int minValue) {
    int v = std::max(minValue, value);
    int p = 1;
    while (p < v) {
        p <<= 1;
    }
    return p;
}

int log2Pow2(int value) {
    int v = value;
    int log = 0;
    while (v > 1) {
        v >>= 1;
        ++log;
    }
    return log;
}

size_t cellIndex(int x, int y, int z, int dim) {
    return static_cast<size_t>(x)
        + static_cast<size_t>(y) * static_cast<size_t>(dim)
        + static_cast<size_t>(z) * static_cast<size_t>(dim) * static_cast<size_t>(dim);
}

VoxelMaterialClass classifyOrDefault(const VoxelMaterialClassifier& classify, VoxelId id) {
    if (!classify) {
        return id == 0 ? VoxelMaterialClass::Air : VoxelMaterialClass::Opaque;
    }
    return classify(id);
}

} // namespace

VoxelPageTree buildVoxelPageTree(const VoxelPageCpu& page,
                                 int minLeafVoxels,
                                 const VoxelMaterialClassifier& classify) {
    VoxelPageTree out{};
    out.key = page.key;
    out.dim = page.dim;
    out.minLeafVoxels = clampPow2(minLeafVoxels, 1);
    if (out.minLeafVoxels > out.dim) {
        out.minLeafVoxels = out.dim;
    }

    if (page.dim <= 0 || page.mips.empty() || page.mips.baseDim != page.dim) {
        return out;
    }

    const int baseDim = page.mips.baseDim;
    const int maxMip = static_cast<int>(page.mips.levels.size()) - 1;
    if (maxMip < 0) {
        return out;
    }

    const int rootMip = log2Pow2(baseDim);
    if (rootMip != maxMip) {
        // Mip pyramid is malformed (expected levels[log2(baseDim)] to be the 1^3 root cell).
        return out;
    }

    auto makeLeaf = [&](VoxelSvoNodeKind kind, VoxelId material, uint16_t leafSize) -> uint32_t {
        VoxelSvoNode node{};
        node.kind = kind;
        node.leafSizeVoxels = leafSize;
        if (kind == VoxelSvoNodeKind::Solid) {
            node.materialId = material;
            node.materialClass = classifyOrDefault(classify, material);
        }
        uint32_t index = static_cast<uint32_t>(out.nodes.size());
        out.nodes.push_back(node);
        return index;
    };

    auto makeMixed = [&]() -> uint32_t {
        VoxelSvoNode node{};
        node.kind = VoxelSvoNodeKind::Mixed;
        uint32_t index = static_cast<uint32_t>(out.nodes.size());
        out.nodes.push_back(node);
        return index;
    };

    std::function<uint32_t(int, int, int, int, bool)> buildRegion;
    buildRegion = [&](int x0, int y0, int z0, int size, bool isRoot) -> uint32_t {
        const int mip = log2Pow2(size);
        const VoxelMipLevel& level = page.mips.levels[mip];
        const int cellX = x0 >> mip;
        const int cellY = y0 >> mip;
        const int cellZ = z0 >> mip;
        uint32_t packed = level.cells[cellIndex(cellX, cellY, cellZ, level.dim)];
        const bool uniform = VoxelMipLevel::isUniform(packed);
        const VoxelId rep = VoxelMipLevel::value(packed);

        if (uniform) {
            if (rep == 0) {
                if (isRoot) {
                    return makeLeaf(VoxelSvoNodeKind::Empty, 0, static_cast<uint16_t>(size));
                }
                return VoxelSvoNode::kInvalidChild;
            }
            return makeLeaf(VoxelSvoNodeKind::Solid, rep, static_cast<uint16_t>(size));
        }

        if (size <= out.minLeafVoxels) {
            if (rep == 0) {
                if (isRoot) {
                    return makeLeaf(VoxelSvoNodeKind::Empty, 0, static_cast<uint16_t>(size));
                }
                return VoxelSvoNode::kInvalidChild;
            }
            return makeLeaf(VoxelSvoNodeKind::Solid, rep, static_cast<uint16_t>(size));
        }

        const int half = size / 2;
        uint8_t childMask = 0;
        std::array<uint32_t, 8> children = {
            VoxelSvoNode::kInvalidChild, VoxelSvoNode::kInvalidChild,
            VoxelSvoNode::kInvalidChild, VoxelSvoNode::kInvalidChild,
            VoxelSvoNode::kInvalidChild, VoxelSvoNode::kInvalidChild,
            VoxelSvoNode::kInvalidChild, VoxelSvoNode::kInvalidChild
        };

        for (int child = 0; child < 8; ++child) {
            const int dx = (child & 1) ? half : 0;
            const int dy = (child & 2) ? half : 0;
            const int dz = (child & 4) ? half : 0;
            uint32_t childIndexNode =
                buildRegion(x0 + dx, y0 + dy, z0 + dz, half, false);
            if (childIndexNode == VoxelSvoNode::kInvalidChild) {
                continue;
            }
            childMask |= static_cast<uint8_t>(1u << child);
            children[child] = childIndexNode;
        }

        if (childMask == 0) {
            if (isRoot) {
                return makeLeaf(VoxelSvoNodeKind::Empty, 0, static_cast<uint16_t>(size));
            }
            return VoxelSvoNode::kInvalidChild;
        }

        uint32_t nodeIndex = makeMixed();
        VoxelSvoNode& node = out.nodes[nodeIndex];
        node.childMask = childMask;
        node.children = children;
        return nodeIndex;
    };

    out.root = buildRegion(0, 0, 0, baseDim, true);
    if (out.root == VoxelSvoNode::kInvalidChild) {
        out.nodes.clear();
    }
    return out;
}

} // namespace Rigel::Voxel
