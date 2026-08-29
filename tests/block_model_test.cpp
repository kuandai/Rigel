#include "TestFramework.h"

#include "Rigel/Voxel/BlockModel.h"
#include "Rigel/Voxel/BlockRegistry.h"

using namespace Rigel::Voxel;

TEST_CASE(BlockModel_CanonicalFullCubeCarriesFastPathMetadata) {
    const auto first = BlockModel::fullCube();
    const auto second = BlockModel::fullCube();
    CHECK_EQ(first.get(), second.get());
    CHECK(first->isFullCube());
    CHECK_EQ(first->cuboids().size(), static_cast<size_t>(1));
    CHECK_EQ(first->textureSlots().size(), DirectionCount);

    const BlockModelCuboid& cuboid = first->cuboids().front();
    CHECK_EQ(cuboid.bounds.min, (std::array<float, 3>{0.0f, 0.0f, 0.0f}));
    CHECK_EQ(cuboid.bounds.max, (std::array<float, 3>{1.0f, 1.0f, 1.0f}));
    for (size_t index = 0; index < DirectionCount; ++index) {
        CHECK(cuboid.faces[index].has_value());
        CHECK(cuboid.faces[index]->ambientOcclusion);
        CHECK(cuboid.faces[index]->cullAgainstOpaqueNeighbor);
    }
}

TEST_CASE(BlockModel_ExplicitEmptyGeometryIsNotANullReference) {
    BlockType block;
    block.model = BlockModel::empty();
    CHECK(block.model);
    CHECK(block.model->isEmpty());
    CHECK(!block.model->isFullCube());
}

TEST_CASE(BlockType_OwnsSharedModelLifetimeTransitively) {
    BlockRegistry blocks;
    std::weak_ptr<const BlockModel> lifetime;
    {
        BlockModelCuboid cuboid;
        cuboid.bounds.min = {-0.25f, 0.0f, 0.0f};
        cuboid.bounds.max = {1.25f, 0.5f, 1.0f};
        auto model = std::make_shared<const BlockModel>(
            "test:extended", std::vector<std::string>{},
            std::vector<BlockModelCuboid>{cuboid});
        lifetime = model;

        BlockModelRegistry models;
        const std::array registrations = {model};
        models.registerModels(registrations);

        BlockType block;
        block.identifier = "test:extended_block";
        block.model = models.find("test:extended");
        blocks.registerBlock("test:extended_block", std::move(block));
    }

    CHECK(!lifetime.expired());
    const BlockType& block =
        blocks.getType(*blocks.findByIdentifier("test:extended_block"));
    CHECK_EQ(block.model->cuboids().front().bounds.min[0], -0.25f);
    CHECK_EQ(block.model->cuboids().front().bounds.max[0], 1.25f);
}

TEST_CASE(BlockRegistries_RejectMutationAfterFreeze) {
    BlockModelRegistry models;
    models.freeze();
    auto model = std::make_shared<const BlockModel>(
        "test:model", std::vector<std::string>{},
        std::vector<BlockModelCuboid>{});
    const std::array registrations = {model};
    CHECK_THROWS(models.registerModels(registrations));

    BlockRegistry blocks;
    blocks.freeze();
    BlockType block;
    block.identifier = "test:block";
    CHECK_THROWS(blocks.registerBlock(block.identifier, block));
}
