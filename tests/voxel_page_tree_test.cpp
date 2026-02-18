#include "TestFramework.h"

#include "Rigel/Voxel/VoxelLod/VoxelPageTree.h"

#include <random>

namespace {
using namespace Rigel::Voxel;

VoxelMaterialClass basicClassifier(VoxelId id) {
    return id == 0 ? VoxelMaterialClass::Air : VoxelMaterialClass::Opaque;
}

void validateTreeInvariants(const VoxelPageTree& tree) {
    CHECK(!tree.empty());
    CHECK(tree.root < tree.nodes.size());

    for (size_t i = 0; i < tree.nodes.size(); ++i) {
        const VoxelSvoNode& node = tree.nodes[i];
        if (node.kind == VoxelSvoNodeKind::Mixed) {
            CHECK_NE(node.childMask, 0);
            for (int child = 0; child < 8; ++child) {
                const bool present = (node.childMask & (1u << child)) != 0;
                const uint32_t idx = node.children[child];
                if (present) {
                    CHECK_NE(idx, VoxelSvoNode::kInvalidChild);
                    CHECK(idx < tree.nodes.size());
                } else {
                    CHECK_EQ(idx, VoxelSvoNode::kInvalidChild);
                }
            }
        } else {
            CHECK_EQ(node.childMask, 0);
            for (int child = 0; child < 8; ++child) {
                CHECK_EQ(node.children[child], VoxelSvoNode::kInvalidChild);
            }
        }
    }
}

} // namespace

namespace Rigel::Voxel {
namespace {

TEST_CASE(VoxelPageTree_UniformAirCollapsesToSingleEmptyLeaf) {
    constexpr int dim = 16;
    std::vector<VoxelId> l0(static_cast<size_t>(dim * dim * dim), 0);
    VoxelPageCpu page = buildVoxelPageCpu(VoxelPageKey{0, 0, 0, 0}, l0, dim);

    VoxelPageTree tree = buildVoxelPageTree(page, 1, basicClassifier);
    CHECK(!tree.empty());
    CHECK_EQ(tree.nodes.size(), static_cast<size_t>(1));
    CHECK_EQ(tree.root, static_cast<uint32_t>(0));
    CHECK_EQ(tree.nodes[0].kind, VoxelSvoNodeKind::Empty);
    CHECK_EQ(tree.nodes[0].leafSizeVoxels, static_cast<uint16_t>(dim));
    validateTreeInvariants(tree);
}

TEST_CASE(VoxelPageTree_UniformSolidCollapsesToSingleSolidLeaf) {
    constexpr int dim = 16;
    constexpr VoxelId kSolid = 42;
    std::vector<VoxelId> l0(static_cast<size_t>(dim * dim * dim), kSolid);
    VoxelPageCpu page = buildVoxelPageCpu(VoxelPageKey{0, 0, 0, 0}, l0, dim);

    VoxelPageTree tree = buildVoxelPageTree(page, 1, basicClassifier);
    CHECK(!tree.empty());
    CHECK_EQ(tree.nodes.size(), static_cast<size_t>(1));
    CHECK_EQ(tree.root, static_cast<uint32_t>(0));
    CHECK_EQ(tree.nodes[0].kind, VoxelSvoNodeKind::Solid);
    CHECK_EQ(tree.nodes[0].materialId, kSolid);
    CHECK_EQ(tree.nodes[0].materialClass, VoxelMaterialClass::Opaque);
    CHECK_EQ(tree.nodes[0].leafSizeVoxels, static_cast<uint16_t>(dim));
    validateTreeInvariants(tree);
}

TEST_CASE(VoxelPageTree_MinLeafStopsEarlierAndReducesNodeCount) {
    constexpr int dim = 32;
    std::vector<VoxelId> l0(static_cast<size_t>(dim * dim * dim), 0);
    for (int z = 0; z < dim; ++z) {
        for (int y = 0; y < dim; ++y) {
            for (int x = 0; x < dim; ++x) {
                bool solid = ((x ^ y ^ z) & 1) != 0;
                l0[static_cast<size_t>(x + y * dim + z * dim * dim)] = solid ? 7 : 0;
            }
        }
    }

    VoxelPageCpu page = buildVoxelPageCpu(VoxelPageKey{0, 0, 0, 0}, l0, dim);
    VoxelPageTree fine = buildVoxelPageTree(page, 1, basicClassifier);
    VoxelPageTree coarse = buildVoxelPageTree(page, 8, basicClassifier);

    CHECK(!fine.empty());
    CHECK(!coarse.empty());
    CHECK(fine.nodes.size() > coarse.nodes.size());
    validateTreeInvariants(fine);
    validateTreeInvariants(coarse);
}

TEST_CASE(VoxelPageTree_RandomInputMaintainsInvariants) {
    constexpr int dim = 32;
    std::mt19937 rng(1337);
    std::uniform_int_distribution<int> dist(0, 5);
    std::vector<VoxelId> l0(static_cast<size_t>(dim * dim * dim), 0);
    for (VoxelId& v : l0) {
        v = static_cast<VoxelId>(dist(rng));
    }

    VoxelPageCpu page = buildVoxelPageCpu(VoxelPageKey{0, 0, 0, 0}, l0, dim);
    VoxelPageTree tree = buildVoxelPageTree(page, 4, basicClassifier);
    CHECK(!tree.empty());
    validateTreeInvariants(tree);
}

} // namespace
} // namespace Rigel::Voxel
