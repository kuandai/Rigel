#include "TestFramework.h"

#include "Rigel/Voxel/MeshBuilder.h"

using namespace Rigel::Voxel;

namespace {
BlockRegistry makeRegistry() {
    BlockRegistry registry;
    BlockType solid;
    solid.identifier = "rigel:stone";
    registry.registerBlock(solid.identifier, solid);
    return registry;
}
}

TEST_CASE(MeshBuilder_SingleBlock) {
    BlockRegistry registry = makeRegistry();
    Chunk chunk({0, 0, 0});
    BlockState state;
    state.id = registry.findByIdentifier("rigel:stone").value();
    chunk.setBlock(1, 1, 1, state);

    MeshBuilder builder;
    MeshBuilder::BuildContext ctx{
        .chunk = chunk,
        .registry = registry,
        .atlas = nullptr,
        .neighbors = {}
    };

    ChunkMesh mesh = builder.build(ctx);
    CHECK_EQ(mesh.vertices.size(), static_cast<size_t>(24));
    CHECK_EQ(mesh.indices.size(), static_cast<size_t>(36));
    CHECK_EQ(mesh.layers[static_cast<size_t>(RenderLayer::Opaque)].indexCount, static_cast<uint32_t>(36));
}

TEST_CASE(MeshBuilder_EmptyChunk) {
    BlockRegistry registry = makeRegistry();
    Chunk chunk({0, 0, 0});

    MeshBuilder builder;
    MeshBuilder::BuildContext ctx{
        .chunk = chunk,
        .registry = registry,
        .atlas = nullptr,
        .neighbors = {}
    };

    ChunkMesh mesh = builder.build(ctx);
    CHECK(mesh.isEmpty());
}

TEST_CASE(MeshBuilder_MissingBoundaryNeighborIsAir) {
    BlockRegistry registry = makeRegistry();
    Chunk chunk({0, 0, 0});
    BlockState state;
    state.id = registry.findByIdentifier("rigel:stone").value();
    chunk.setBlock(Chunk::SIZE - 1, 1, 1, state);

    MeshBuilder builder;
    MeshBuilder::BuildContext ctx{
        .chunk = chunk,
        .registry = registry,
        .atlas = nullptr,
        .neighbors = {}
    };

    ChunkMesh mesh = builder.build(ctx);
    CHECK_EQ(mesh.vertices.size(), static_cast<size_t>(24));
    CHECK_EQ(mesh.indices.size(), static_cast<size_t>(36));
}

TEST_CASE(MeshBuilder_PartialOpaqueModelDoesNotOccludeAFullCellFace) {
    BlockRegistry registry = makeRegistry();
    BlockModelCuboid cuboid;
    cuboid.bounds.max = {1.0f, 0.5f, 1.0f};
    auto partialModel = std::make_shared<const BlockModel>(
        "test:partial", std::vector<std::string>{},
        std::vector<BlockModelCuboid>{cuboid});
    BlockType partial;
    partial.identifier = "test:partial";
    partial.model = partialModel;
    const BlockID partialId = registry.registerBlock(partial.identifier, partial);

    Chunk chunk({0, 0, 0});
    const BlockID stoneId = *registry.findByIdentifier("rigel:stone");
    chunk.setBlock(1, 1, 1, BlockState{stoneId});
    chunk.setBlock(2, 1, 1, BlockState{partialId});

    MeshBuilder builder;
    const ChunkMesh mesh = builder.build({
        .chunk = chunk,
        .registry = registry,
        .atlas = nullptr,
        .neighbors = {},
    });
    CHECK_EQ(mesh.vertices.size(), static_cast<size_t>(24));
    CHECK_EQ(mesh.indices.size(), static_cast<size_t>(36));
}
