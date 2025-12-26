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
