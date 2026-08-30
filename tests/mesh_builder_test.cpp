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

void checkMeshesEqual(const ChunkMesh& first, const ChunkMesh& second) {
    CHECK_EQ(first.vertices.size(), second.vertices.size());
    CHECK_EQ(first.indices, second.indices);
    CHECK_EQ(first.layers.size(), second.layers.size());
    for (size_t layer = 0; layer < first.layers.size(); ++layer) {
        CHECK_EQ(first.layers[layer].indexStart, second.layers[layer].indexStart);
        CHECK_EQ(first.layers[layer].indexCount, second.layers[layer].indexCount);
    }
    for (size_t index = 0; index < first.vertices.size(); ++index) {
        const VoxelVertex& left = first.vertices[index];
        const VoxelVertex& right = second.vertices[index];
        CHECK_EQ(left.x, right.x);
        CHECK_EQ(left.y, right.y);
        CHECK_EQ(left.z, right.z);
        CHECK_EQ(left.u, right.u);
        CHECK_EQ(left.v, right.v);
        CHECK_EQ(left.normalIndex, right.normalIndex);
        CHECK_EQ(left.aoLevel, right.aoLevel);
        CHECK_EQ(left.textureLayer, right.textureLayer);
    }
}

size_t countFacesOnPlane(
    const ChunkMesh& mesh,
    Direction direction,
    size_t axis,
    float coordinate
) {
    CHECK_EQ(mesh.vertices.size() % 4, static_cast<size_t>(0));
    size_t count = 0;
    for (size_t firstVertex = 0;
         firstVertex < mesh.vertices.size(); firstVertex += 4) {
        bool matches = true;
        for (size_t vertex = 0; vertex < 4; ++vertex) {
            const VoxelVertex& candidate = mesh.vertices[firstVertex + vertex];
            const std::array<float, 3> position = {
                candidate.x, candidate.y, candidate.z};
            if (candidate.normalIndex != static_cast<uint8_t>(direction) ||
                position[axis] != coordinate) {
                matches = false;
                break;
            }
        }
        count += matches ? 1 : 0;
    }
    return count;
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

TEST_CASE(MeshBuilder_EmitsZeroThicknessFaceWithAuthoredShadingDirection) {
    BlockModelCuboid cuboid;
    cuboid.bounds.min = {0.5f, 0.0f, 0.0f};
    cuboid.bounds.max = {0.5f, 1.0f, 1.0f};
    BlockModelFace face = modelFace("surface");
    face.shadingFace = Direction::PosY;
    cuboid.faces[static_cast<size_t>(Direction::NegX)] = std::move(face);

    BlockRegistry registry = makeRegistry();
    BlockType block;
    block.identifier = "test:flat_face";
    block.model = makeModel(
        "test:flat_face_model", {"surface"}, {std::move(cuboid)});
    const BlockID blockId = registry.registerBlock(block.identifier, block);

    Chunk chunk({0, 0, 0});
    chunk.setBlock(1, 2, 3, BlockState{blockId});
    const ChunkMesh mesh = MeshBuilder{}.build({
        .chunk = chunk,
        .registry = registry,
        .atlas = nullptr,
        .neighbors = {},
    });

    CHECK_EQ(mesh.vertices.size(), static_cast<size_t>(4));
    CHECK_EQ(mesh.indices.size(), static_cast<size_t>(6));
    for (const VoxelVertex& vertex : mesh.vertices) {
        CHECK_EQ(vertex.x, 1.5f);
        CHECK_EQ(vertex.normalIndex, static_cast<uint8_t>(Direction::PosY));
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

TEST_CASE(MeshBuilder_OrientsPartialBoundsAndFacesForEveryMeasuredTurn) {
    TextureAtlas atlas;
    addTexture(atlas, "textures/test/dummy.png");
    const TextureHandle east = addTexture(atlas, "textures/test/east.png");
    const TextureHandle top = addTexture(atlas, "textures/test/top.png");

    BlockModelCuboid cuboid;
    cuboid.bounds = {{-0.25f, 0.125f, 0.25f},
                     {0.75f, 0.625f, 1.25f}};
    cuboid.faces[static_cast<size_t>(Direction::PosX)] = modelFace("east");
    cuboid.faces[static_cast<size_t>(Direction::PosY)] = modelFace("top");
    const auto sharedModel = makeModel(
        "test:oriented_partial", {"east", "top"}, {cuboid});

    struct OrientationCase {
        BlockModelOrientation orientation;
        BlockModelBounds bounds;
        std::array<Direction, 2> directions;
    };
    constexpr std::array cases = {
        OrientationCase{
            BlockModelOrientation::Identity,
            {{-0.25f, 0.125f, 0.25f}, {0.75f, 0.625f, 1.25f}},
            {Direction::PosX, Direction::PosY}},
        OrientationCase{
            BlockModelOrientation::RotateX90,
            {{-0.25f, 0.25f, 0.375f}, {0.75f, 1.25f, 0.875f}},
            {Direction::PosX, Direction::NegZ}},
        OrientationCase{
            BlockModelOrientation::RotateX270,
            {{-0.25f, -0.25f, 0.125f}, {0.75f, 0.75f, 0.625f}},
            {Direction::PosX, Direction::PosZ}},
        OrientationCase{
            BlockModelOrientation::RotateY90,
            {{-0.25f, 0.125f, -0.25f}, {0.75f, 0.625f, 0.75f}},
            {Direction::PosZ, Direction::PosY}},
        OrientationCase{
            BlockModelOrientation::RotateY180,
            {{0.25f, 0.125f, -0.25f}, {1.25f, 0.625f, 0.75f}},
            {Direction::NegX, Direction::PosY}},
        OrientationCase{
            BlockModelOrientation::RotateY270,
            {{0.25f, 0.125f, 0.25f}, {1.25f, 0.625f, 1.25f}},
            {Direction::NegZ, Direction::PosY}},
        OrientationCase{
            BlockModelOrientation::RotateZ90,
            {{0.125f, 0.25f, 0.25f}, {0.625f, 1.25f, 1.25f}},
            {Direction::NegY, Direction::PosX}},
    };

    for (size_t caseIndex = 0; caseIndex < cases.size(); ++caseIndex) {
        const OrientationCase& item = cases[caseIndex];
        BlockRegistry registry = makeRegistry();
        BlockType block;
        block.identifier = "test:oriented_" + std::to_string(caseIndex);
        block.model.geometry = sharedModel;
        block.model.orientation = item.orientation;
        block.textures.bind("east", "textures/test/east.png");
        block.textures.bind("top", "textures/test/top.png");
        const BlockID blockId = registry.registerBlock(block.identifier, block);
        Chunk chunk({0, 0, 0});
        chunk.setBlock(2, 3, 4, BlockState{blockId});

        const MeshBuilder builder;
        const MeshBuilder::BuildContext context{
            .chunk = chunk,
            .registry = registry,
            .atlas = &atlas,
            .neighbors = {},
        };
        const ChunkMesh mesh = builder.build(context);
        const ChunkMesh repeated = builder.build(context);
        checkMeshesEqual(mesh, repeated);

        CHECK_EQ(mesh.vertices.size(), static_cast<size_t>(8));
        const std::array<TextureHandle, 2> textures = {east, top};
        for (size_t face = 0; face < 2; ++face) {
            const Direction direction = item.directions[face];
            const size_t directionIndex = static_cast<size_t>(direction);
            for (size_t vertex = 0; vertex < 4; ++vertex) {
                const auto& unit = kCubeFacePositions[directionIndex][vertex];
                const VoxelVertex& actual = mesh.vertices[face * 4 + vertex];
                CHECK_EQ(actual.x, 2.0f + (unit[0] == 0.0f
                    ? item.bounds.min[0] : item.bounds.max[0]));
                CHECK_EQ(actual.y, 3.0f + (unit[1] == 0.0f
                    ? item.bounds.min[1] : item.bounds.max[1]));
                CHECK_EQ(actual.z, 4.0f + (unit[2] == 0.0f
                    ? item.bounds.min[2] : item.bounds.max[2]));
                CHECK_EQ(actual.normalIndex, static_cast<uint8_t>(direction));
                CHECK_EQ(actual.textureLayer, textures[face].index);
                CHECK_EQ(actual.aoLevel, static_cast<uint8_t>(3));
            }
            checkFaceWinding(mesh, face * 4, direction);
        }
    }
}

TEST_CASE(MeshBuilder_OrientsFullCubeTexturesAndExplicitTopBottomUvs) {
    TextureAtlas atlas;
    addTexture(atlas, "textures/test/dummy.png");
    std::array<TextureHandle, DirectionCount> textures;
    BlockType cube;
    cube.identifier = "test:oriented_cube";
    cube.model.orientation = BlockModelOrientation::RotateZ90;
    cube.model.rotateTopBottomUv = true;
    for (size_t source = 0; source < DirectionCount; ++source) {
        const std::string path =
            "textures/test/oriented_" + std::to_string(source) + ".png";
        textures[source] = addTexture(atlas, path);
        cube.textures.setFace(static_cast<Direction>(source), path);
    }
    BlockRegistry registry;
    const BlockID cubeId = registry.registerBlock(cube.identifier, cube);
    Chunk chunk({0, 0, 0});
    chunk.setBlock(1, 1, 1, BlockState{cubeId});

    const ChunkMesh mesh = MeshBuilder{}.build({
        .chunk = chunk,
        .registry = registry,
        .atlas = &atlas,
        .neighbors = {},
    });

    constexpr std::array<Direction, DirectionCount> expectedDirections = {
        Direction::NegY, Direction::PosY, Direction::PosX,
        Direction::NegX, Direction::PosZ, Direction::NegZ};
    constexpr std::array<std::array<std::array<float, 2>, 4>, DirectionCount>
        expectedUvs = {{
            {{{1, 1}, {1, 0}, {0, 0}, {0, 1}}},
            {{{0, 0}, {0, 1}, {1, 1}, {1, 0}}},
            {{{1, 0}, {0, 0}, {0, 1}, {1, 1}}},
            {{{0, 1}, {1, 1}, {1, 0}, {0, 0}}},
            {{{1, 0}, {0, 0}, {0, 1}, {1, 1}}},
            {{{0, 1}, {1, 1}, {1, 0}, {0, 0}}},
        }};
    CHECK_EQ(mesh.vertices.size(), static_cast<size_t>(24));
    for (size_t source = 0; source < DirectionCount; ++source) {
        const size_t firstVertex = source * 4;
        for (size_t vertex = 0; vertex < 4; ++vertex) {
            const VoxelVertex& actual = mesh.vertices[firstVertex + vertex];
            const auto& expectedUv = expectedUvs[source][vertex];
            CHECK_EQ(actual.normalIndex,
                     static_cast<uint8_t>(expectedDirections[source]));
            CHECK_EQ(actual.textureLayer, textures[source].index);
            CHECK_EQ(actual.u, expectedUv[0]);
            CHECK_EQ(actual.v, expectedUv[1]);
        }
        checkFaceWinding(mesh, firstVertex, expectedDirections[source]);
    }
}

TEST_CASE(MeshBuilder_ComposesOrientedCroppedAndReversedFaceUvs) {
    BlockModelCuboid cuboid;
    cuboid.bounds = {{0.25f, 0.0f, 0.125f}, {0.75f, 1.0f, 0.875f}};
    cuboid.faces[static_cast<size_t>(Direction::PosY)] = modelFace(
        "top", {1.0f, 0.25f, 0.0f, 0.75f},
        BlockModelUvRotation::Quarter);
    BlockType block;
    block.identifier = "test:oriented_uv";
    block.model.geometry = makeModel(
        "test:oriented_uv_model", {"top"}, {cuboid});
    block.model.orientation = BlockModelOrientation::RotateZ90;
    block.model.rotateTopBottomUv = true;
    BlockRegistry registry = makeRegistry();
    const BlockID blockId = registry.registerBlock(block.identifier, block);
    Chunk chunk({0, 0, 0});
    chunk.setBlock(1, 1, 1, BlockState{blockId});

    const ChunkMesh mesh = MeshBuilder{}.build({
        .chunk = chunk,
        .registry = registry,
        .atlas = nullptr,
        .neighbors = {},
    });

    constexpr std::array<std::array<float, 2>, 4> expectedUvs = {{
        {0.0f, 0.75f}, {0.0f, 0.25f},
        {1.0f, 0.25f}, {1.0f, 0.75f},
    }};
    CHECK_EQ(mesh.vertices.size(), static_cast<size_t>(4));
    for (size_t vertex = 0; vertex < 4; ++vertex) {
        CHECK_EQ(mesh.vertices[vertex].u, expectedUvs[vertex][0]);
        CHECK_EQ(mesh.vertices[vertex].v, expectedUvs[vertex][1]);
        CHECK_EQ(mesh.vertices[vertex].normalIndex,
                 static_cast<uint8_t>(Direction::PosX));
    }
    checkFaceWinding(mesh, 0, Direction::PosX);
}

TEST_CASE(MeshBuilder_MatchesCosmicReachFaceUvConvention) {
    using Uvs = std::array<std::array<float, 2>, 4>;
    struct Case {
        Direction source;
        Direction destination;
        BlockModelUvRotation authoredRotation;
        BlockModelOrientation orientation;
        Uvs expected;
    };
    constexpr BlockModelUvRect uv = {0.125f, 0.25f, 0.75f, 0.875f};
    const std::array<Case, 6> cases = {{
        {Direction::PosX, Direction::PosX, BlockModelUvRotation::Quarter,
         BlockModelOrientation::Identity,
         {{{0.75f, 0.125f}, {0.125f, 0.125f},
           {0.125f, 0.75f}, {0.75f, 0.75f}}}},
        {Direction::PosY, Direction::PosY, BlockModelUvRotation::Quarter,
         BlockModelOrientation::Identity,
         {{{0.125f, 0.125f}, {0.125f, 0.75f},
           {0.75f, 0.75f}, {0.75f, 0.125f}}}},
        {Direction::NegY, Direction::NegY, BlockModelUvRotation::Quarter,
         BlockModelOrientation::Identity,
         {{{0.125f, 0.125f}, {0.125f, 0.75f},
           {0.75f, 0.75f}, {0.75f, 0.125f}}}},
        {Direction::PosY, Direction::PosY, BlockModelUvRotation::None,
         BlockModelOrientation::RotateY90,
         {{{0.125f, 0.125f}, {0.125f, 0.75f},
           {0.75f, 0.75f}, {0.75f, 0.125f}}}},
        {Direction::PosY, Direction::NegZ, BlockModelUvRotation::None,
         BlockModelOrientation::RotateX90,
         {{{0.75f, 0.75f}, {0.75f, 0.125f},
           {0.125f, 0.125f}, {0.125f, 0.75f}}}},
        {Direction::PosX, Direction::PosX, BlockModelUvRotation::None,
         BlockModelOrientation::RotateX90,
         {{{0.75f, 0.125f}, {0.125f, 0.125f},
           {0.125f, 0.75f}, {0.75f, 0.75f}}}},
    }};

    for (size_t caseIndex = 0; caseIndex < cases.size(); ++caseIndex) {
        const Case& item = cases[caseIndex];
        BlockModelCuboid cuboid;
        cuboid.bounds = {{0.125f, 0.25f, 0.375f},
                         {0.75f, 0.875f, 0.9375f}};
        cuboid.faces[static_cast<size_t>(item.source)] =
            modelFace("surface", uv, item.authoredRotation);

        BlockType block;
        block.identifier = "test:cr_uv_" + std::to_string(caseIndex);
        block.model.geometry = makeModel(
            block.identifier + "_model", {"surface"}, {cuboid});
        block.model.orientation = item.orientation;
        BlockRegistry registry = makeRegistry();
        const BlockID blockId = registry.registerBlock(
            block.identifier, block);
        Chunk chunk({0, 0, 0});
        chunk.setBlock(1, 1, 1, BlockState{blockId});

        const ChunkMesh mesh = MeshBuilder{}.build({
            .chunk = chunk,
            .registry = registry,
            .atlas = nullptr,
            .neighbors = {},
        });
        CHECK_EQ(mesh.vertices.size(), static_cast<size_t>(4));
        for (size_t vertex = 0; vertex < 4; ++vertex) {
            CHECK_EQ(mesh.vertices[vertex].u, item.expected[vertex][0]);
            CHECK_EQ(mesh.vertices[vertex].v, item.expected[vertex][1]);
            CHECK_EQ(mesh.vertices[vertex].normalIndex,
                     static_cast<uint8_t>(item.destination));
        }
        checkFaceWinding(mesh, 0, item.destination);
    }
}

TEST_CASE(MeshBuilder_RotatesBoundaryCullingDirection) {
    BlockModelCuboid cuboid;
    cuboid.bounds = {{0.0f, 0.25f, 0.0f}, {1.0f, 0.75f, 1.0f}};
    cuboid.faces[static_cast<size_t>(Direction::PosX)] =
        modelFace("surface", {}, BlockModelUvRotation::None, false, true);
    BlockType panel;
    panel.identifier = "test:oriented_boundary";
    panel.model.geometry = makeModel(
        "test:oriented_boundary_model", {"surface"}, {cuboid});
    panel.model.orientation = BlockModelOrientation::RotateZ90;

    BlockRegistry registry = makeRegistry();
    const BlockID panelId = registry.registerBlock(panel.identifier, panel);
    const BlockID stoneId = *registry.findByIdentifier("rigel:stone");
    Chunk chunk({0, 0, 0});
    chunk.setBlock(2, 2, 2, BlockState{panelId});
    chunk.setBlock(2, 1, 2, BlockState{stoneId});

    const ChunkMesh mesh = MeshBuilder{}.build({
        .chunk = chunk,
        .registry = registry,
        .atlas = nullptr,
        .neighbors = {},
    });
    CHECK_EQ(mesh.vertices.size(), static_cast<size_t>(24));
    CHECK_EQ(mesh.indices.size(), static_cast<size_t>(36));
}

TEST_CASE(MeshBuilder_UsesRotatedNeighborBoundaryCoverageForCulling) {
    BlockModelCuboid cuboid;
    cuboid.bounds = {{0.0f, 0.0f, 0.0f}, {0.5f, 1.0f, 1.0f}};
    cuboid.faces[static_cast<size_t>(Direction::NegX)] = modelFace("surface");

    BlockType rotated;
    rotated.identifier = "test:rotated_occluder";
    rotated.model.geometry = makeModel(
        "test:rotated_occluder_model", {"surface"}, {cuboid});
    rotated.model.orientation = BlockModelOrientation::RotateZ90;
    rotated.isOpaque = true;

    BlockRegistry registry = makeRegistry();
    const BlockID rotatedId = registry.registerBlock(
        rotated.identifier, rotated);
    const BlockID stoneId = *registry.findByIdentifier("rigel:stone");

    Chunk chunk({0, 0, 0});
    chunk.setBlock(2, 2, 2, BlockState{stoneId});
    chunk.setBlock(2, 1, 2, BlockState{rotatedId});

    const ChunkMesh mesh = MeshBuilder{}.build({
        .chunk = chunk,
        .registry = registry,
        .atlas = nullptr,
        .neighbors = {},
    });

    CHECK_EQ(mesh.vertices.size(), static_cast<size_t>(24));
    CHECK_EQ(mesh.indices.size(), static_cast<size_t>(36));
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
        {{{0.125f, 0.75f}, {0.875f, 0.75f},
          {0.875f, 0.25f}, {0.125f, 0.25f}}},
        {{{0.7f, 0.9f}, {0.2f, 0.9f}, {0.2f, 0.2f}, {0.7f, 0.2f}}},
        {{{0.9f, 0.8f}, {0.9f, 0.4f}, {0.1f, 0.4f}, {0.1f, 0.8f}}},
    }};
    for (size_t face = 0; face < handles.size(); ++face) {
        for (size_t vertex = 0; vertex < 4; ++vertex) {
            const VoxelVertex& actual = mesh.vertices[face * 4 + vertex];
            CHECK_NEAR(actual.u, expectedUvs[face][vertex][0], 0.000001f);
            CHECK_NEAR(actual.v, expectedUvs[face][vertex][1], 0.000001f);
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

TEST_CASE(MeshBuilder_OpaqueNormalizedFullCellModelsOccludeNeighbors) {
    BlockRegistry registry = makeRegistry();
    const BlockID stoneId = *registry.findByIdentifier("rigel:stone");

    BlockType normalizedCube;
    normalizedCube.identifier = "test:normalized_full_cell";
    normalizedCube.model = makeModel(
        "test:normalized_full_cell_model", {"surface"},
        {completeCuboid(
            {{0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}}, "surface",
            false, true)});
    normalizedCube.isOpaque = true;
    const BlockID normalizedCubeId = registry.registerBlock(
        normalizedCube.identifier, normalizedCube);

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

    CHECK_EQ(
        adjacentIndexCount(stoneId, normalizedCubeId),
        static_cast<size_t>(60));
    CHECK_EQ(
        adjacentIndexCount(normalizedCubeId, stoneId),
        static_cast<size_t>(60));
    CHECK_EQ(
        adjacentIndexCount(normalizedCubeId, normalizedCubeId),
        static_cast<size_t>(60));
}

TEST_CASE(MeshBuilder_OpaqueBoundaryCoverageMaySpanMultipleCuboids) {
    BlockModelCuboid source;
    source.bounds = {{0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}};
    source.faces[static_cast<size_t>(Direction::PosX)] =
        modelFace("surface", {}, BlockModelUvRotation::None, false, true);

    auto boundaryHalf = [](float minY, float maxY) {
        BlockModelCuboid cuboid;
        cuboid.bounds = {{0.0f, minY, 0.0f}, {0.5f, maxY, 1.0f}};
        cuboid.faces[static_cast<size_t>(Direction::NegX)] =
            modelFace("surface");
        return cuboid;
    };

    BlockRegistry registry = makeRegistry();
    BlockType sourceType;
    sourceType.identifier = "test:coverage_source";
    sourceType.model = makeModel(
        "test:coverage_source_model", {"surface"}, {source});
    const BlockID sourceId = registry.registerBlock(
        sourceType.identifier, sourceType);

    BlockType joined;
    joined.identifier = "test:joined_boundary";
    joined.model = makeModel(
        "test:joined_boundary_model", {"surface"},
        {boundaryHalf(0.0f, 0.5f), boundaryHalf(0.5f, 1.0f)});
    joined.isOpaque = true;
    const BlockID joinedId = registry.registerBlock(joined.identifier, joined);

    BlockType separated = joined;
    separated.identifier = "test:separated_boundary";
    separated.model = makeModel(
        "test:separated_boundary_model", {"surface"},
        {boundaryHalf(0.0f, 0.375f), boundaryHalf(0.625f, 1.0f)});
    const BlockID separatedId = registry.registerBlock(
        separated.identifier, separated);

    auto adjacentIndexCount = [&](BlockID neighbor) {
        Chunk chunk({0, 0, 0});
        chunk.setBlock(1, 1, 1, BlockState{sourceId});
        chunk.setBlock(2, 1, 1, BlockState{neighbor});
        return MeshBuilder{}.build({
            .chunk = chunk,
            .registry = registry,
            .atlas = nullptr,
            .neighbors = {},
        }).indices.size();
    };

    CHECK_EQ(adjacentIndexCount(joinedId), static_cast<size_t>(12));
    CHECK_EQ(adjacentIndexCount(separatedId), static_cast<size_t>(18));
}

TEST_CASE(MeshBuilder_BoundaryCoverageRequiresNeighborOpacity) {
    BlockModelCuboid source;
    source.bounds = {{0.0f, 0.25f, 0.25f}, {1.0f, 0.75f, 0.75f}};
    source.faces[static_cast<size_t>(Direction::PosX)] =
        modelFace("surface", {}, BlockModelUvRotation::None, false, true);

    BlockModelCuboid covering;
    covering.bounds = {{0.0f, 0.0f, 0.0f}, {0.5f, 1.0f, 1.0f}};
    covering.faces[static_cast<size_t>(Direction::NegX)] = modelFace("surface");

    BlockRegistry registry = makeRegistry();
    BlockType sourceType;
    sourceType.identifier = "test:opacity_source";
    sourceType.model = makeModel(
        "test:opacity_source_model", {"surface"}, {source});
    const BlockID sourceId = registry.registerBlock(
        sourceType.identifier, sourceType);

    BlockType opaque;
    opaque.identifier = "test:opaque_cover";
    opaque.model = makeModel(
        "test:opaque_cover_model", {"surface"}, {covering});
    opaque.isOpaque = true;
    const BlockID opaqueId = registry.registerBlock(opaque.identifier, opaque);

    BlockType transparent = opaque;
    transparent.identifier = "test:transparent_cover";
    transparent.isOpaque = false;
    const BlockID transparentId = registry.registerBlock(
        transparent.identifier, transparent);

    auto adjacentIndexCount = [&](BlockID neighbor) {
        Chunk chunk({0, 0, 0});
        chunk.setBlock(1, 1, 1, BlockState{sourceId});
        chunk.setBlock(2, 1, 1, BlockState{neighbor});
        return MeshBuilder{}.build({
            .chunk = chunk,
            .registry = registry,
            .atlas = nullptr,
            .neighbors = {},
        }).indices.size();
    };

    CHECK_EQ(adjacentIndexCount(opaqueId), static_cast<size_t>(6));
    CHECK_EQ(adjacentIndexCount(transparentId), static_cast<size_t>(12));
}

TEST_CASE(MeshBuilder_FullCellModelMissingBoundaryFaceDoesNotOcclude) {
    BlockRegistry registry = makeRegistry();
    const BlockID stoneId = *registry.findByIdentifier("rigel:stone");

    BlockModelCuboid openCube = completeCuboid(
        {{0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}}, "surface",
        false, true);
    openCube.faces[static_cast<size_t>(Direction::NegX)] = std::nullopt;
    BlockType openType;
    openType.identifier = "test:open_full_cell";
    openType.model = makeModel(
        "test:open_full_cell_model", {"surface"}, {std::move(openCube)});
    openType.isOpaque = true;
    const BlockID openId = registry.registerBlock(
        openType.identifier, openType);

    Chunk chunk({0, 0, 0});
    chunk.setBlock(1, 1, 1, BlockState{stoneId});
    chunk.setBlock(2, 1, 1, BlockState{openId});
    const ChunkMesh mesh = MeshBuilder{}.build({
        .chunk = chunk,
        .registry = registry,
        .atlas = nullptr,
        .neighbors = {},
    });

    CHECK_EQ(mesh.indices.size(), static_cast<size_t>(66));
}

TEST_CASE(MeshBuilder_CullsOnlyMatchingSameTypeCuboidBoundaryFaces) {
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

    auto buildAdjacent = [&](BlockID left, BlockID right) {
        Chunk chunk({0, 0, 0});
        chunk.setBlock(1, 1, 1, BlockState{left});
        chunk.setBlock(2, 1, 1, BlockState{right});
        return MeshBuilder{}.build({
            .chunk = chunk,
            .registry = registry,
            .atlas = nullptr,
            .neighbors = {},
        });
    };

    const ChunkMesh cubeThenSlab = buildAdjacent(stoneId, slabId);
    CHECK_EQ(cubeThenSlab.indices.size(), static_cast<size_t>(66));
    CHECK_EQ(
        countFacesOnPlane(cubeThenSlab, Direction::PosX, 0, 2.0f),
        static_cast<size_t>(1));
    CHECK_EQ(
        countFacesOnPlane(cubeThenSlab, Direction::NegX, 0, 2.0f),
        static_cast<size_t>(0));

    const ChunkMesh slabThenCube = buildAdjacent(slabId, stoneId);
    CHECK_EQ(slabThenCube.indices.size(), static_cast<size_t>(66));
    CHECK_EQ(
        countFacesOnPlane(slabThenCube, Direction::PosX, 0, 2.0f),
        static_cast<size_t>(0));
    CHECK_EQ(
        countFacesOnPlane(slabThenCube, Direction::NegX, 0, 2.0f),
        static_cast<size_t>(1));

    CHECK_EQ(
        buildAdjacent(slabId, slabId).indices.size(),
        static_cast<size_t>(60));

    BlockModelCuboid upper;
    upper.bounds = {{0.25f, 0.5f, 0.0f}, {1.0f, 1.0f, 1.0f}};
    upper.faces[static_cast<size_t>(Direction::PosX)] = modelFace("surface");
    BlockModelCuboid lower;
    lower.bounds = {{0.0f, 0.0f, 0.0f}, {0.75f, 0.5f, 1.0f}};
    lower.faces[static_cast<size_t>(Direction::NegX)] = modelFace("surface");
    BlockType disjointType;
    disjointType.identifier = "test:disjoint_faces";
    disjointType.model = makeModel(
        "test:disjoint_faces_model", {"surface"},
        {std::move(upper), std::move(lower)});
    disjointType.cullSameType = true;
    const BlockID disjointId = registry.registerBlock(
        disjointType.identifier, disjointType);

    CHECK_EQ(
        buildAdjacent(disjointId, disjointId).indices.size(),
        static_cast<size_t>(24));

    BlockModelCuboid smallPositive;
    smallPositive.bounds = {{0.5f, 0.25f, 0.25f}, {1.0f, 0.75f, 0.75f}};
    smallPositive.faces[static_cast<size_t>(Direction::PosX)] =
        modelFace("surface");
    BlockModelCuboid largeNegative;
    largeNegative.bounds = {{0.0f, 0.0f, 0.0f}, {0.5f, 1.0f, 1.0f}};
    largeNegative.faces[static_cast<size_t>(Direction::NegX)] =
        modelFace("surface");
    BlockType asymmetricType;
    asymmetricType.identifier = "test:asymmetric_same_type";
    asymmetricType.model = makeModel(
        "test:asymmetric_same_type_model", {"surface"},
        {std::move(smallPositive), std::move(largeNegative)});
    asymmetricType.isOpaque = false;
    asymmetricType.cullSameType = true;
    const BlockID asymmetricId = registry.registerBlock(
        asymmetricType.identifier, asymmetricType);

    const ChunkMesh asymmetricNeighbors = buildAdjacent(
        asymmetricId, asymmetricId);
    CHECK_EQ(asymmetricNeighbors.indices.size(), static_cast<size_t>(18));
    CHECK_EQ(
        countFacesOnPlane(
            asymmetricNeighbors, Direction::PosX, 0, 2.0f),
        static_cast<size_t>(0));
    CHECK_EQ(
        countFacesOnPlane(
            asymmetricNeighbors, Direction::NegX, 0, 2.0f),
        static_cast<size_t>(1));
}

TEST_CASE(MeshBuilder_CullsModelBoundariesAcrossChunkEdges) {
    BlockModelCuboid boundary;
    boundary.bounds = {{0.0f, 0.25f, 0.25f}, {1.0f, 0.75f, 0.75f}};
    boundary.faces[static_cast<size_t>(Direction::PosX)] =
        modelFace("surface", {}, BlockModelUvRotation::None, false, true);

    BlockRegistry registry = makeRegistry();
    BlockType modelType;
    modelType.identifier = "test:chunk_boundary_model";
    modelType.model = makeModel(
        "test:chunk_boundary_model_geometry", {"surface"}, {boundary});
    const BlockID modelId = registry.registerBlock(
        modelType.identifier, modelType);
    const BlockID stoneId = *registry.findByIdentifier("rigel:stone");

    Chunk chunk({0, 0, 0});
    chunk.setBlock(Chunk::SIZE - 1, 1, 1, BlockState{modelId});
    Chunk neighbor({1, 0, 0});
    neighbor.setBlock(0, 1, 1, BlockState{stoneId});

    std::array<const Chunk*, DirectionCount> neighbors{};
    neighbors[static_cast<size_t>(Direction::PosX)] = &neighbor;

    const ChunkMesh loaded = MeshBuilder{}.build({
        .chunk = chunk,
        .registry = registry,
        .atlas = nullptr,
        .neighbors = neighbors,
    });
    const ChunkMesh missing = MeshBuilder{}.build({
        .chunk = chunk,
        .registry = registry,
        .atlas = nullptr,
        .neighbors = {},
    });

    CHECK(loaded.isEmpty());
    CHECK_EQ(missing.indices.size(), static_cast<size_t>(6));
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
