#include "TestFramework.h"

#include "Rigel/Voxel/MeshBuilder.h"

#include <array>
#include <memory>
#include <string>
#include <vector>

using namespace Rigel::Voxel;

namespace {
BlockRegistry makeRegistry() {
    BlockRegistry registry;
    BlockType solid;
    solid.identifier = "rigel:stone";
    registry.registerBlock(solid.identifier, solid);
    return registry;
}

std::shared_ptr<const BlockModel> makeModel(
    std::string identifier,
    std::vector<std::string> textureSlots,
    std::vector<BlockModelCuboid> cuboids
) {
    return std::make_shared<const BlockModel>(
        std::move(identifier), std::move(textureSlots), std::move(cuboids));
}

BlockModelFace modelFace(
    std::string textureSlot,
    BlockModelUvRect uv = {},
    BlockModelUvRotation rotation = BlockModelUvRotation::None,
    bool ambientOcclusion = false,
    bool cull = false
) {
    return BlockModelFace{
        .textureSlot = std::move(textureSlot),
        .uv = uv,
        .rotation = rotation,
        .ambientOcclusion = ambientOcclusion,
        .cullAgainstOpaqueNeighbor = cull,
    };
}

TextureHandle addTexture(TextureAtlas& atlas, const std::string& path) {
    std::vector<unsigned char> pixels(
        static_cast<size_t>(atlas.tileSize() * atlas.tileSize() * 4), 255);
    return atlas.addTexture(path, pixels.data());
}

using FacePositions = std::array<std::array<float, 3>, 4>;

constexpr std::array<FacePositions, DirectionCount> kCubeFacePositions = {{
    {{{1, 0, 1}, {1, 1, 1}, {1, 1, 0}, {1, 0, 0}}},
    {{{0, 0, 0}, {0, 1, 0}, {0, 1, 1}, {0, 0, 1}}},
    {{{0, 1, 0}, {1, 1, 0}, {1, 1, 1}, {0, 1, 1}}},
    {{{0, 0, 1}, {1, 0, 1}, {1, 0, 0}, {0, 0, 0}}},
    {{{0, 0, 1}, {0, 1, 1}, {1, 1, 1}, {1, 0, 1}}},
    {{{1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0, 0, 0}}},
}};

constexpr std::array<std::array<float, 2>, 4> kCubeUvs = {{
    {0, 0}, {0, 1}, {1, 1}, {1, 0},
}};

constexpr std::array<uint32_t, 6> kQuadIndices = {0, 1, 2, 0, 2, 3};

void checkFaceWinding(
    const ChunkMesh& mesh, size_t firstVertex, Direction direction
) {
    const VoxelVertex& a = mesh.vertices[firstVertex];
    const VoxelVertex& b = mesh.vertices[firstVertex + 1];
    const VoxelVertex& c = mesh.vertices[firstVertex + 2];
    const std::array<float, 3> ab = {b.x - a.x, b.y - a.y, b.z - a.z};
    const std::array<float, 3> ac = {c.x - a.x, c.y - a.y, c.z - a.z};
    const std::array<float, 3> cross = {
        ab[1] * ac[2] - ab[2] * ac[1],
        ab[2] * ac[0] - ab[0] * ac[2],
        ab[0] * ac[1] - ab[1] * ac[0],
    };
    int nx = 0;
    int ny = 0;
    int nz = 0;
    directionOffset(direction, nx, ny, nz);
    // Chunk rendering declares clockwise triangles as front-facing.
    CHECK(cross[0] * nx + cross[1] * ny + cross[2] * nz < 0.0f);
}

BlockModelCuboid completeCuboid(
    BlockModelBounds bounds, std::string textureSlot,
    bool ambientOcclusion = false, bool cull = false
) {
    BlockModelCuboid cuboid;
    cuboid.bounds = bounds;
    for (auto& face : cuboid.faces) {
        face = modelFace(textureSlot, {}, BlockModelUvRotation::None,
                         ambientOcclusion, cull);
    }
    return cuboid;
}
}

TEST_CASE(MeshBuilder_CanonicalCubeMatchesLegacyGeometry) {
    TextureAtlas atlas;
    addTexture(atlas, "textures/test/dummy.png");
    std::array<TextureHandle, DirectionCount> textures;
    for (size_t face = 0; face < DirectionCount; ++face) {
        textures[face] = addTexture(
            atlas, "textures/test/cube_" + std::to_string(face) + ".png");
    }

    BlockRegistry registry;
    BlockType cube;
    cube.identifier = "test:canonical_cube";
    cube.layer = RenderLayer::Transparent;
    for (size_t face = 0; face < DirectionCount; ++face) {
        cube.textures.setFace(
            static_cast<Direction>(face),
            "textures/test/cube_" + std::to_string(face) + ".png");
    }
    const BlockID cubeId = registry.registerBlock(cube.identifier, cube);
    Chunk chunk({0, 0, 0});
    chunk.setBlock(1, 2, 3, BlockState{cubeId});

    const ChunkMesh mesh = MeshBuilder{}.build({
        .chunk = chunk,
        .registry = registry,
        .atlas = &atlas,
        .neighbors = {},
    });

    CHECK_EQ(mesh.vertices.size(), static_cast<size_t>(24));
    CHECK_EQ(mesh.indices.size(), static_cast<size_t>(36));
    for (size_t face = 0; face < DirectionCount; ++face) {
        const size_t firstVertex = face * 4;
        for (size_t vertex = 0; vertex < 4; ++vertex) {
            const VoxelVertex& actual = mesh.vertices[firstVertex + vertex];
            CHECK_EQ(actual.x, 1.0f + kCubeFacePositions[face][vertex][0]);
            CHECK_EQ(actual.y, 2.0f + kCubeFacePositions[face][vertex][1]);
            CHECK_EQ(actual.z, 3.0f + kCubeFacePositions[face][vertex][2]);
            CHECK_EQ(actual.u, kCubeUvs[vertex][0]);
            CHECK_EQ(actual.v, kCubeUvs[vertex][1]);
            CHECK_EQ(actual.normalIndex, static_cast<uint8_t>(face));
            CHECK_EQ(actual.aoLevel, static_cast<uint8_t>(3));
            CHECK_EQ(actual.textureLayer, textures[face].index);
        }
        for (size_t index = 0; index < kQuadIndices.size(); ++index) {
            CHECK_EQ(mesh.indices[face * 6 + index],
                     static_cast<uint32_t>(firstVertex) + kQuadIndices[index]);
        }
        checkFaceWinding(mesh, firstVertex, static_cast<Direction>(face));
    }

    CHECK_EQ(mesh.layers[static_cast<size_t>(RenderLayer::Opaque)].indexStart,
             static_cast<uint32_t>(0));
    CHECK_EQ(mesh.layers[static_cast<size_t>(RenderLayer::Opaque)].indexCount,
             static_cast<uint32_t>(0));
    CHECK_EQ(mesh.layers[static_cast<size_t>(RenderLayer::Cutout)].indexStart,
             static_cast<uint32_t>(0));
    CHECK_EQ(mesh.layers[static_cast<size_t>(RenderLayer::Cutout)].indexCount,
             static_cast<uint32_t>(0));
    CHECK_EQ(mesh.layers[static_cast<size_t>(RenderLayer::Transparent)].indexStart,
             static_cast<uint32_t>(0));
    CHECK_EQ(mesh.layers[static_cast<size_t>(RenderLayer::Transparent)].indexCount,
             static_cast<uint32_t>(36));
    CHECK_EQ(mesh.layers[static_cast<size_t>(RenderLayer::Emissive)].indexStart,
             static_cast<uint32_t>(36));
    CHECK_EQ(mesh.layers[static_cast<size_t>(RenderLayer::Emissive)].indexCount,
             static_cast<uint32_t>(0));
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

TEST_CASE(MeshBuilder_CanonicalCubeRetainsNeighborCullingAndAoDiagonal) {
    TextureAtlas atlas;
    addTexture(atlas, "textures/test/dummy.png");
    const TextureHandle targetTexture =
        addTexture(atlas, "textures/test/target_pos_x.png");

    BlockRegistry registry;
    BlockType target;
    target.identifier = "test:ao_target";
    target.textures.setFace(
        Direction::PosX, "textures/test/target_pos_x.png");
    const BlockID targetId = registry.registerBlock(target.identifier, target);

    BlockType occluder;
    occluder.identifier = "test:ao_occluder";
    occluder.layer = RenderLayer::Cutout;
    const BlockID occluderId =
        registry.registerBlock(occluder.identifier, occluder);

    Chunk chunk({0, 0, 0});
    chunk.setBlock(1, 1, 1, BlockState{targetId});
    chunk.setBlock(2, 2, 1, BlockState{occluderId});
    chunk.setBlock(2, 1, 2, BlockState{occluderId});

    const ChunkMesh mesh = MeshBuilder{}.build({
        .chunk = chunk,
        .registry = registry,
        .atlas = &atlas,
        .neighbors = {},
    });

    CHECK_EQ(mesh.vertices[0].textureLayer, targetTexture.index);
    CHECK_EQ(mesh.vertices[0].aoLevel, static_cast<uint8_t>(2));
    CHECK_EQ(mesh.vertices[1].aoLevel, static_cast<uint8_t>(0));
    CHECK_EQ(mesh.vertices[2].aoLevel, static_cast<uint8_t>(2));
    CHECK_EQ(mesh.vertices[3].aoLevel, static_cast<uint8_t>(3));
    const std::array<uint32_t, 6> flipped = {0, 1, 3, 1, 2, 3};
    for (size_t index = 0; index < flipped.size(); ++index) {
        CHECK_EQ(mesh.indices[index], flipped[index]);
    }

    Chunk adjacent({0, 0, 0});
    adjacent.setBlock(1, 1, 1, BlockState{targetId});
    adjacent.setBlock(2, 1, 1, BlockState{occluderId});
    const ChunkMesh adjacentMesh = MeshBuilder{}.build({
        .chunk = adjacent,
        .registry = registry,
        .atlas = &atlas,
        .neighbors = {},
    });
    CHECK_EQ(adjacentMesh.vertices.size(), static_cast<size_t>(40));
    CHECK_EQ(adjacentMesh.indices.size(), static_cast<size_t>(60));
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

TEST_CASE(MeshBuilder_EmitsInflatedSingleCuboidCardinalGeometry) {
    const BlockModelBounds bounds = {
        {-0.03125f, 0.25f, 0.375f},
        {1.03125f, 0.75f, 0.875f},
    };
    BlockModelCuboid cuboid = completeCuboid(bounds, "surface");
    BlockRegistry registry = makeRegistry();
    BlockType block;
    block.identifier = "test:inflated_cuboid";
    block.model = makeModel(
        "test:inflated_cuboid_model", {"surface"}, {std::move(cuboid)});
    const BlockID blockId = registry.registerBlock(block.identifier, block);

    Chunk chunk({0, 0, 0});
    chunk.setBlock(1, 2, 3, BlockState{blockId});
    const ChunkMesh mesh = MeshBuilder{}.build({
        .chunk = chunk,
        .registry = registry,
        .atlas = nullptr,
        .neighbors = {},
    });

    const std::array<FacePositions, DirectionCount> expected = {{
        {{{1.03125f, 0.25f, 0.875f}, {1.03125f, 0.75f, 0.875f},
          {1.03125f, 0.75f, 0.375f}, {1.03125f, 0.25f, 0.375f}}},
        {{{-0.03125f, 0.25f, 0.375f}, {-0.03125f, 0.75f, 0.375f},
          {-0.03125f, 0.75f, 0.875f}, {-0.03125f, 0.25f, 0.875f}}},
        {{{-0.03125f, 0.75f, 0.375f}, {1.03125f, 0.75f, 0.375f},
          {1.03125f, 0.75f, 0.875f}, {-0.03125f, 0.75f, 0.875f}}},
        {{{-0.03125f, 0.25f, 0.875f}, {1.03125f, 0.25f, 0.875f},
          {1.03125f, 0.25f, 0.375f}, {-0.03125f, 0.25f, 0.375f}}},
        {{{-0.03125f, 0.25f, 0.875f}, {-0.03125f, 0.75f, 0.875f},
          {1.03125f, 0.75f, 0.875f}, {1.03125f, 0.25f, 0.875f}}},
        {{{1.03125f, 0.25f, 0.375f}, {1.03125f, 0.75f, 0.375f},
          {-0.03125f, 0.75f, 0.375f}, {-0.03125f, 0.25f, 0.375f}}},
    }};
    CHECK_EQ(mesh.vertices.size(), static_cast<size_t>(24));
    CHECK_EQ(mesh.indices.size(), static_cast<size_t>(36));
    for (size_t face = 0; face < DirectionCount; ++face) {
        for (size_t vertex = 0; vertex < 4; ++vertex) {
            const VoxelVertex& actual = mesh.vertices[face * 4 + vertex];
            CHECK_EQ(actual.x, 1.0f + expected[face][vertex][0]);
            CHECK_EQ(actual.y, 2.0f + expected[face][vertex][1]);
            CHECK_EQ(actual.z, 3.0f + expected[face][vertex][2]);
            CHECK_EQ(actual.normalIndex, static_cast<uint8_t>(face));
            CHECK_EQ(actual.aoLevel, static_cast<uint8_t>(3));
        }
        for (size_t index = 0; index < kQuadIndices.size(); ++index) {
            CHECK_EQ(mesh.indices[face * 6 + index],
                     static_cast<uint32_t>(face * 4) + kQuadIndices[index]);
        }
        checkFaceWinding(mesh, face * 4, static_cast<Direction>(face));
    }
}

TEST_CASE(MeshBuilder_EmitsNormalizedCuboidsWithMissingFacesAndExtendedBounds) {
    BlockRegistry registry = makeRegistry();

    BlockModelCuboid lower;
    lower.bounds.min = {-0.25f, 0.25f, 0.125f};
    lower.bounds.max = {1.25f, 0.75f, 0.875f};
    lower.faces[static_cast<size_t>(Direction::PosX)] = modelFace("surface");
    lower.faces[static_cast<size_t>(Direction::NegZ)] = modelFace("surface");

    BlockModelCuboid upper;
    upper.bounds.min = {0.25f, 0.75f, 0.25f};
    upper.bounds.max = {0.75f, 1.25f, 0.75f};
    upper.faces[static_cast<size_t>(Direction::PosY)] = modelFace("surface");

    BlockType post;
    post.identifier = "test:post";
    post.model = makeModel(
        "test:post_model", {"surface"}, {lower, upper});
    post.textures.bind("surface", "textures/test/post.png");
    post.layer = RenderLayer::Cutout;
    const BlockID postId = registry.registerBlock(post.identifier, post);

    Chunk chunk({0, 0, 0});
    chunk.setBlock(1, 1, 1, BlockState{postId});

    MeshBuilder builder;
    const ChunkMesh mesh = builder.build({
        .chunk = chunk,
        .registry = registry,
        .atlas = nullptr,
        .neighbors = {},
    });

    CHECK_EQ(mesh.vertices.size(), static_cast<size_t>(12));
    CHECK_EQ(mesh.indices.size(), static_cast<size_t>(18));
    CHECK_EQ(
        mesh.layers[static_cast<size_t>(RenderLayer::Opaque)].indexCount,
        static_cast<uint32_t>(0));
    CHECK_EQ(
        mesh.layers[static_cast<size_t>(RenderLayer::Cutout)].indexCount,
        static_cast<uint32_t>(18));

    const std::array<std::array<float, 3>, 12> expectedPositions = {{
        {2.25f, 1.25f, 1.875f}, {2.25f, 1.75f, 1.875f},
        {2.25f, 1.75f, 1.125f}, {2.25f, 1.25f, 1.125f},
        {2.25f, 1.25f, 1.125f}, {2.25f, 1.75f, 1.125f},
        {0.75f, 1.75f, 1.125f}, {0.75f, 1.25f, 1.125f},
        {1.25f, 2.25f, 1.25f}, {1.75f, 2.25f, 1.25f},
        {1.75f, 2.25f, 1.75f}, {1.25f, 2.25f, 1.75f},
    }};
    const std::array<Direction, 3> expectedDirections = {
        Direction::PosX, Direction::NegZ, Direction::PosY};
    for (size_t vertex = 0; vertex < mesh.vertices.size(); ++vertex) {
        CHECK_EQ(mesh.vertices[vertex].x, expectedPositions[vertex][0]);
        CHECK_EQ(mesh.vertices[vertex].y, expectedPositions[vertex][1]);
        CHECK_EQ(mesh.vertices[vertex].z, expectedPositions[vertex][2]);
        CHECK_EQ(mesh.vertices[vertex].normalIndex,
                 static_cast<uint8_t>(expectedDirections[vertex / 4]));
        CHECK_EQ(mesh.vertices[vertex].aoLevel, static_cast<uint8_t>(3));
    }
    for (size_t face = 0; face < expectedDirections.size(); ++face) {
        for (size_t index = 0; index < kQuadIndices.size(); ++index) {
            CHECK_EQ(mesh.indices[face * 6 + index],
                     static_cast<uint32_t>(face * 4) + kQuadIndices[index]);
        }
        checkFaceWinding(mesh, face * 4, expectedDirections[face]);
    }
}

TEST_CASE(MeshBuilder_ResolvesNamedTexturesAndTransformsModelUvs) {
    TextureAtlas atlas;
    const TextureHandle first = addTexture(atlas, "textures/test/first.png");
    const TextureHandle second = addTexture(atlas, "textures/test/second.png");
    const TextureHandle third = addTexture(atlas, "textures/test/third.png");
    const TextureHandle fourth = addTexture(atlas, "textures/test/fourth.png");

    BlockModelCuboid cuboid;
    cuboid.bounds.max = {1.0f, 1.0f, 1.0f};
    cuboid.faces[static_cast<size_t>(Direction::PosX)] = modelFace(
        "first", {0.125f, 0.25f, 0.875f, 0.75f});
    cuboid.faces[static_cast<size_t>(Direction::NegX)] = modelFace(
        "second", {0.875f, 0.75f, 0.125f, 0.25f},
        BlockModelUvRotation::Quarter);
    cuboid.faces[static_cast<size_t>(Direction::PosY)] = modelFace(
        "third", {0.2f, 0.8f, 0.7f, 0.1f},
        BlockModelUvRotation::Half);
    cuboid.faces[static_cast<size_t>(Direction::NegY)] = modelFace(
        "fourth", {0.1f, 0.2f, 0.9f, 0.6f},
        BlockModelUvRotation::ThreeQuarter);

    BlockRegistry registry = makeRegistry();
    BlockType panel;
    panel.identifier = "test:panel";
    panel.model = makeModel(
        "test:panel_model", {"first", "second", "third", "fourth"},
        {cuboid});
    panel.textures.bind("first", "textures/test/first.png");
    panel.textures.bind("second", "textures/test/second.png");
    panel.textures.bind("third", "textures/test/third.png");
    panel.textures.bind("fourth", "textures/test/fourth.png");
    const BlockID panelId = registry.registerBlock(panel.identifier, panel);

    Chunk chunk({0, 0, 0});
    chunk.setBlock(1, 1, 1, BlockState{panelId});
    MeshBuilder builder;
    const ChunkMesh mesh = builder.build({
        .chunk = chunk,
        .registry = registry,
        .atlas = &atlas,
        .neighbors = {},
    });

    CHECK_EQ(mesh.vertices.size(), static_cast<size_t>(16));
    const std::array<TextureHandle, 4> handles = {first, second, third, fourth};
    const std::array<std::array<std::array<float, 2>, 4>, 4> expectedUvs = {{
        {{{0.125f, 0.25f}, {0.125f, 0.75f},
          {0.875f, 0.75f}, {0.875f, 0.25f}}},
        {{{0.875f, 0.25f}, {0.125f, 0.25f},
          {0.125f, 0.75f}, {0.875f, 0.75f}}},
        {{{0.7f, 0.1f}, {0.7f, 0.8f}, {0.2f, 0.8f}, {0.2f, 0.1f}}},
        {{{0.9f, 0.2f}, {0.1f, 0.2f}, {0.1f, 0.6f}, {0.9f, 0.6f}}},
    }};
    for (size_t face = 0; face < handles.size(); ++face) {
        for (size_t vertex = 0; vertex < 4; ++vertex) {
            const VoxelVertex& actual = mesh.vertices[face * 4 + vertex];
            CHECK_EQ(actual.u, expectedUvs[face][vertex][0]);
            CHECK_EQ(actual.v, expectedUvs[face][vertex][1]);
            CHECK_EQ(actual.textureLayer, handles[face].index);
        }
        checkFaceWinding(mesh, face * 4, static_cast<Direction>(face));
    }
}

TEST_CASE(MeshBuilder_BatchesMultipleCuboidsByBlockRenderLayer) {
    BlockRegistry registry = makeRegistry();

    BlockModelCuboid cutoutCuboid;
    cutoutCuboid.bounds.min = {0.25f, 0.0f, 0.25f};
    cutoutCuboid.bounds.max = {0.75f, 0.5f, 0.75f};
    cutoutCuboid.faces[static_cast<size_t>(Direction::PosX)] =
        modelFace("surface");
    cutoutCuboid.faces[static_cast<size_t>(Direction::NegY)] =
        modelFace("surface");
    BlockType cutout;
    cutout.identifier = "test:cutout_model";
    cutout.model = makeModel(
        "test:cutout_geometry", {"surface"}, {cutoutCuboid});
    cutout.layer = RenderLayer::Cutout;
    const BlockID cutoutId = registry.registerBlock(cutout.identifier, cutout);

    BlockModelCuboid lower;
    lower.bounds.min = {0.0f, 0.0f, 0.0f};
    lower.bounds.max = {1.0f, 0.25f, 1.0f};
    lower.faces[static_cast<size_t>(Direction::PosY)] = modelFace("surface");
    BlockModelCuboid upper;
    upper.bounds.min = {0.25f, 0.25f, 0.25f};
    upper.bounds.max = {0.75f, 1.0f, 0.75f};
    upper.faces[static_cast<size_t>(Direction::PosZ)] = modelFace("surface");
    upper.faces[static_cast<size_t>(Direction::NegZ)] = modelFace("surface");
    BlockType transparent;
    transparent.identifier = "test:transparent_model";
    transparent.model = makeModel(
        "test:transparent_geometry", {"surface"}, {lower, upper});
    transparent.layer = RenderLayer::Transparent;
    const BlockID transparentId =
        registry.registerBlock(transparent.identifier, transparent);

    const BlockID stoneId = *registry.findByIdentifier("rigel:stone");
    Chunk chunk({0, 0, 0});
    chunk.setBlock(1, 1, 1, BlockState{transparentId});
    chunk.setBlock(3, 1, 1, BlockState{cutoutId});
    chunk.setBlock(5, 1, 1, BlockState{stoneId});
    const ChunkMesh mesh = MeshBuilder{}.build({
        .chunk = chunk,
        .registry = registry,
        .atlas = nullptr,
        .neighbors = {},
    });

    CHECK_EQ(mesh.vertices.size(), static_cast<size_t>(44));
    CHECK_EQ(mesh.indices.size(), static_cast<size_t>(66));
    CHECK_EQ(mesh.layers[static_cast<size_t>(RenderLayer::Opaque)].indexStart,
             static_cast<uint32_t>(0));
    CHECK_EQ(mesh.layers[static_cast<size_t>(RenderLayer::Opaque)].indexCount,
             static_cast<uint32_t>(36));
    CHECK_EQ(mesh.layers[static_cast<size_t>(RenderLayer::Cutout)].indexStart,
             static_cast<uint32_t>(36));
    CHECK_EQ(mesh.layers[static_cast<size_t>(RenderLayer::Cutout)].indexCount,
             static_cast<uint32_t>(12));
    CHECK_EQ(mesh.layers[static_cast<size_t>(RenderLayer::Transparent)].indexStart,
             static_cast<uint32_t>(48));
    CHECK_EQ(mesh.layers[static_cast<size_t>(RenderLayer::Transparent)].indexCount,
             static_cast<uint32_t>(18));
    CHECK_EQ(mesh.layers[static_cast<size_t>(RenderLayer::Emissive)].indexStart,
             static_cast<uint32_t>(66));
    CHECK_EQ(mesh.layers[static_cast<size_t>(RenderLayer::Emissive)].indexCount,
             static_cast<uint32_t>(0));

    CHECK_EQ(mesh.vertices[24].normalIndex,
             static_cast<uint8_t>(Direction::PosX));
    CHECK_EQ(mesh.vertices[28].normalIndex,
             static_cast<uint8_t>(Direction::NegY));
    CHECK_EQ(mesh.vertices[32].normalIndex,
             static_cast<uint8_t>(Direction::PosY));
    CHECK_EQ(mesh.vertices[36].normalIndex,
             static_cast<uint8_t>(Direction::PosZ));
    CHECK_EQ(mesh.vertices[40].normalIndex,
             static_cast<uint8_t>(Direction::NegZ));
    CHECK_EQ(mesh.indices[36], static_cast<uint32_t>(24));
    CHECK_EQ(mesh.indices[48], static_cast<uint32_t>(32));
    checkFaceWinding(mesh, 24, Direction::PosX);
    checkFaceWinding(mesh, 28, Direction::NegY);
    checkFaceWinding(mesh, 32, Direction::PosY);
    checkFaceWinding(mesh, 36, Direction::PosZ);
    checkFaceWinding(mesh, 40, Direction::NegZ);
}

TEST_CASE(MeshBuilder_PreservesTextureLayersAcrossByteBoundary) {
    TextureAtlas atlas;
    for (size_t layer = 0; layer < 255; ++layer) {
        addTexture(atlas, "textures/test/filler_" + std::to_string(layer));
    }
    const TextureHandle belowBoundary =
        addTexture(atlas, "textures/test/layer_255.png");
    const TextureHandle aboveBoundary =
        addTexture(atlas, "textures/test/layer_256.png");

    BlockModelCuboid cuboid;
    cuboid.bounds.max = {1.0f, 1.0f, 1.0f};
    cuboid.faces[static_cast<size_t>(Direction::PosX)] = modelFace("below");
    cuboid.faces[static_cast<size_t>(Direction::NegX)] = modelFace("above");

    BlockRegistry registry = makeRegistry();
    BlockType panel;
    panel.identifier = "test:wide_texture_layers";
    panel.model = makeModel(
        "test:wide_texture_layers_model", {"below", "above"}, {cuboid});
    panel.textures.bind("below", "textures/test/layer_255.png");
    panel.textures.bind("above", "textures/test/layer_256.png");
    const BlockID panelId = registry.registerBlock(panel.identifier, panel);

    Chunk chunk({0, 0, 0});
    chunk.setBlock(1, 1, 1, BlockState{panelId});
    const ChunkMesh mesh = MeshBuilder{}.build({
        .chunk = chunk,
        .registry = registry,
        .atlas = &atlas,
        .neighbors = {},
    });

    CHECK_EQ(belowBoundary.index, static_cast<uint16_t>(255));
    CHECK_EQ(aboveBoundary.index, static_cast<uint16_t>(256));
    CHECK_EQ(mesh.vertices.size(), static_cast<size_t>(8));
    for (size_t vertex = 0; vertex < 4; ++vertex) {
        CHECK_EQ(mesh.vertices[vertex].textureLayer, belowBoundary.index);
    }
    for (size_t vertex = 4; vertex < 8; ++vertex) {
        CHECK_EQ(mesh.vertices[vertex].textureLayer, aboveBoundary.index);
    }
}

TEST_CASE(MeshBuilder_ModelFaceCullingIsControlledByFaceMetadata) {
    BlockModelCuboid visibleCuboid;
    visibleCuboid.bounds.min = {0.0f, 0.25f, 0.25f};
    visibleCuboid.bounds.max = {1.0f, 0.75f, 0.75f};
    visibleCuboid.faces[static_cast<size_t>(Direction::PosX)] =
        modelFace("surface", {}, BlockModelUvRotation::None, false, false);

    BlockModelCuboid culledCuboid = visibleCuboid;
    culledCuboid.faces[static_cast<size_t>(Direction::PosX)] =
        modelFace("surface", {}, BlockModelUvRotation::None, false, true);

    BlockModelCuboid interiorCuboid = culledCuboid;
    interiorCuboid.bounds.max[0] = 0.75f;

    BlockModelCuboid overflowCuboid = culledCuboid;
    overflowCuboid.bounds.min[1] = -0.25f;

    BlockRegistry registry = makeRegistry();
    BlockType visible;
    visible.identifier = "test:visible_face";
    visible.model = makeModel(
        "test:visible_face_model", {"surface"}, {visibleCuboid});
    const BlockID visibleId = registry.registerBlock(visible.identifier, visible);

    BlockType culled = visible;
    culled.identifier = "test:culled_face";
    culled.model = makeModel(
        "test:culled_face_model", {"surface"}, {culledCuboid});
    const BlockID culledId = registry.registerBlock(culled.identifier, culled);

    BlockType interior = visible;
    interior.identifier = "test:interior_face";
    interior.model = makeModel(
        "test:interior_face_model", {"surface"}, {interiorCuboid});
    const BlockID interiorId =
        registry.registerBlock(interior.identifier, interior);

    BlockType overflow = visible;
    overflow.identifier = "test:overflow_face";
    overflow.model = makeModel(
        "test:overflow_face_model", {"surface"}, {overflowCuboid});
    const BlockID overflowId =
        registry.registerBlock(overflow.identifier, overflow);
    const BlockID stoneId = *registry.findByIdentifier("rigel:stone");

    MeshBuilder builder;
    auto buildAdjacent = [&](BlockID modelId) {
        Chunk chunk({0, 0, 0});
        chunk.setBlock(1, 1, 1, BlockState{modelId});
        chunk.setBlock(2, 1, 1, BlockState{stoneId});
        return builder.build({
            .chunk = chunk,
            .registry = registry,
            .atlas = nullptr,
            .neighbors = {},
        });
    };

    CHECK_EQ(buildAdjacent(visibleId).indices.size(), static_cast<size_t>(42));
    CHECK_EQ(buildAdjacent(culledId).indices.size(), static_cast<size_t>(36));
    CHECK_EQ(buildAdjacent(interiorId).indices.size(), static_cast<size_t>(42));
    CHECK_EQ(buildAdjacent(overflowId).indices.size(), static_cast<size_t>(42));
}

TEST_CASE(MeshBuilder_UsesConservativeCuboidAdjacencyWithoutFaceSubtraction) {
    BlockRegistry registry = makeRegistry();
    const BlockID stoneId = *registry.findByIdentifier("rigel:stone");

    BlockModelCuboid slab = completeCuboid(
        {{0.0f, 0.0f, 0.0f}, {1.0f, 0.5f, 1.0f}}, "surface",
        false, true);
    BlockType slabType;
    slabType.identifier = "test:slab";
    slabType.model = makeModel(
        "test:slab_model", {"surface"}, {std::move(slab)});
    slabType.cullSameType = true;
    const BlockID slabId = registry.registerBlock(slabType.identifier, slabType);

    auto adjacentIndexCount = [&](BlockID left, BlockID right) {
        Chunk chunk({0, 0, 0});
        chunk.setBlock(1, 1, 1, BlockState{left});
        chunk.setBlock(2, 1, 1, BlockState{right});
        return MeshBuilder{}.build({
            .chunk = chunk,
            .registry = registry,
            .atlas = nullptr,
            .neighbors = {},
        }).indices.size();
    };

    CHECK_EQ(adjacentIndexCount(stoneId, slabId), static_cast<size_t>(66));
    CHECK_EQ(adjacentIndexCount(slabId, stoneId), static_cast<size_t>(66));
    CHECK_EQ(adjacentIndexCount(slabId, slabId), static_cast<size_t>(72));
}

TEST_CASE(MeshBuilder_ModelFacesUseAoOnlyForCubeCompatibleGeometry) {
    TextureAtlas atlas;
    addTexture(atlas, "textures/test/dummy.png");
    const TextureHandle surface =
        addTexture(atlas, "textures/test/ao_surface.png");

    BlockModelCuboid conservativeCuboid;
    conservativeCuboid.bounds.max = {1.0f, 1.0f, 1.0f};
    conservativeCuboid.faces[static_cast<size_t>(Direction::PosX)] =
        modelFace("surface", {}, BlockModelUvRotation::None, false, false);
    BlockModelCuboid shadedCuboid = conservativeCuboid;
    shadedCuboid.faces[static_cast<size_t>(Direction::PosX)] =
        modelFace("surface", {}, BlockModelUvRotation::None, true, false);
    BlockModelCuboid partialCuboid = shadedCuboid;
    partialCuboid.bounds.min = {0.0f, 0.25f, 0.25f};
    partialCuboid.bounds.max = {0.5f, 0.75f, 0.75f};

    BlockRegistry registry = makeRegistry();
    BlockType conservative;
    conservative.identifier = "test:conservative_ao";
    conservative.model = makeModel(
        "test:conservative_ao_model", {"surface"}, {conservativeCuboid});
    conservative.textures.bind("surface", "textures/test/ao_surface.png");
    const BlockID conservativeId =
        registry.registerBlock(conservative.identifier, conservative);

    BlockType shaded = conservative;
    shaded.identifier = "test:shaded_ao";
    shaded.model = makeModel(
        "test:shaded_ao_model", {"surface"}, {shadedCuboid});
    const BlockID shadedId = registry.registerBlock(shaded.identifier, shaded);

    BlockType partial = conservative;
    partial.identifier = "test:partial_ao";
    partial.model = makeModel(
        "test:partial_ao_model", {"surface"}, {partialCuboid});
    const BlockID partialId = registry.registerBlock(partial.identifier, partial);
    const BlockID stoneId = *registry.findByIdentifier("rigel:stone");

    MeshBuilder builder;
    auto faceAo = [&](BlockID modelId) {
        Chunk chunk({0, 0, 0});
        chunk.setBlock(1, 1, 1, BlockState{modelId});
        chunk.setBlock(2, 0, 1, BlockState{stoneId});
        chunk.setBlock(2, 1, 2, BlockState{stoneId});
        chunk.setBlock(2, 0, 2, BlockState{stoneId});
        const ChunkMesh mesh = builder.build({
            .chunk = chunk,
            .registry = registry,
            .atlas = &atlas,
            .neighbors = {},
        });
        std::vector<uint8_t> levels;
        for (const VoxelVertex& vertex : mesh.vertices) {
            if (vertex.textureLayer == surface.index) {
                levels.push_back(vertex.aoLevel);
            }
        }
        return levels;
    };

    const std::vector<uint8_t> conservativeLevels = faceAo(conservativeId);
    CHECK_EQ(conservativeLevels.size(), static_cast<size_t>(4));
    for (const uint8_t level : conservativeLevels) {
        CHECK_EQ(level, static_cast<uint8_t>(3));
    }

    const std::vector<uint8_t> shadedLevels = faceAo(shadedId);
    CHECK_EQ(shadedLevels.size(), static_cast<size_t>(4));
    CHECK_EQ(shadedLevels.front(), static_cast<uint8_t>(0));

    const std::vector<uint8_t> partialLevels = faceAo(partialId);
    CHECK_EQ(partialLevels.size(), static_cast<size_t>(4));
    for (const uint8_t level : partialLevels) {
        CHECK_EQ(level, static_cast<uint8_t>(3));
    }
}
