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

    CHECK_EQ(mesh.vertices[0].x, 2.25f);
    CHECK_EQ(mesh.vertices[0].y, 1.25f);
    CHECK_EQ(mesh.vertices[0].z, 1.875f);
    CHECK_EQ(mesh.vertices[4].x, 2.25f);
    CHECK_EQ(mesh.vertices[4].y, 1.25f);
    CHECK_EQ(mesh.vertices[4].z, 1.125f);
    CHECK_EQ(mesh.vertices[8].x, 1.25f);
    CHECK_EQ(mesh.vertices[8].y, 2.25f);
    CHECK_EQ(mesh.vertices[8].z, 1.25f);
    for (const VoxelVertex& vertex : mesh.vertices) {
        CHECK_EQ(vertex.aoLevel, static_cast<uint8_t>(3));
    }
}

TEST_CASE(MeshBuilder_ResolvesNamedTexturesAndTransformsModelUvs) {
    TextureAtlas atlas;
    const TextureHandle first = addTexture(atlas, "textures/test/first.png");
    const TextureHandle second = addTexture(atlas, "textures/test/second.png");

    BlockModelCuboid cuboid;
    cuboid.bounds.max = {1.0f, 1.0f, 1.0f};
    cuboid.faces[static_cast<size_t>(Direction::PosX)] = modelFace("first");
    cuboid.faces[static_cast<size_t>(Direction::NegX)] = modelFace(
        "second", {0.875f, 0.75f, 0.125f, 0.25f},
        BlockModelUvRotation::Quarter);

    BlockRegistry registry = makeRegistry();
    BlockType panel;
    panel.identifier = "test:panel";
    panel.model = makeModel(
        "test:panel_model", {"first", "second"}, {cuboid});
    panel.textures.bind("first", "textures/test/first.png");
    panel.textures.bind("second", "textures/test/second.png");
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

    CHECK_EQ(mesh.vertices.size(), static_cast<size_t>(8));
    for (size_t vertex = 0; vertex < 4; ++vertex) {
        CHECK_EQ(mesh.vertices[vertex].textureLayer,
                 static_cast<uint8_t>(first.index));
    }
    for (size_t vertex = 4; vertex < 8; ++vertex) {
        CHECK_EQ(mesh.vertices[vertex].textureLayer,
                 static_cast<uint8_t>(second.index));
    }

    CHECK_EQ(mesh.vertices[4].u, 0.875f);
    CHECK_EQ(mesh.vertices[4].v, 0.25f);
    CHECK_EQ(mesh.vertices[5].u, 0.125f);
    CHECK_EQ(mesh.vertices[5].v, 0.25f);
    CHECK_EQ(mesh.vertices[6].u, 0.125f);
    CHECK_EQ(mesh.vertices[6].v, 0.75f);
    CHECK_EQ(mesh.vertices[7].u, 0.875f);
    CHECK_EQ(mesh.vertices[7].v, 0.75f);
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
}

TEST_CASE(MeshBuilder_ModelFacesUseAmbientOcclusionOnlyWhenRequested) {
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
            if (vertex.textureLayer == static_cast<uint8_t>(surface.index)) {
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
}
