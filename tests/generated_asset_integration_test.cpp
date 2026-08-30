#include "TestFramework.h"

#include "OpenGLFixture.h"
#include "ResourceRegistry.h"
#include "Rigel/Asset/AssetManager.h"
#include "Rigel/Asset/Types.h"
#include "Rigel/Voxel/BlockLoader.h"
#include "Rigel/Voxel/BlockModel.h"
#include "Rigel/Voxel/BlockRegistry.h"
#include "Rigel/Voxel/Chunk.h"
#include "Rigel/Voxel/ChunkRenderer.h"
#include "Rigel/Voxel/GeneratorDefinitionLoader.h"
#include "Rigel/Voxel/MeshBuilder.h"
#include "Rigel/Voxel/TextureAtlas.h"
#include "Rigel/Voxel/WorldGenerator.h"
#include "Rigel/Voxel/WorldMeshStore.h"
#include "Rigel/Voxel/WorldResources.h"

#include <algorithm>
#include <array>
#include <limits>
#include <set>
#include <string>
#include <string_view>

namespace {
using namespace Rigel::Voxel;

constexpr std::array<std::string_view, 5> RequiredMaterials = {
    "base:dirt",
    "base:grass",
    "base:sand",
    "base:stone_shale",
    "base:water[type=source]",
};

constexpr std::string_view SlabId =
    "base:wood_planks[slab_type=bottom]";
constexpr std::string_view StairId =
    "base:wood_planks[stair_type=bottom_PosX]";
constexpr std::string_view DoorId =
    "base:door_steel[part=bottom,power=on,direction=PosX]";
constexpr std::string_view LadderId =
    "base:ladder_steel[direction=PosX]";
constexpr std::string_view PistonHeadId =
    "base:piston[direction=PosX,type=advancing,part=head]";

size_t countResources(std::string_view prefix, std::string_view suffix) {
    return static_cast<size_t>(std::count_if(
        ResourceRegistry::Paths().begin(), ResourceRegistry::Paths().end(),
        [&](std::string_view path) {
            return path.starts_with(prefix) && path.ends_with(suffix);
        }));
}

BlockID requireBlockId(
    const BlockRegistry& registry, std::string_view identifier
) {
    const auto id = registry.findByIdentifier(std::string(identifier));
    if (!id) {
        throw Rigel::Test::TestFailure(
            "Missing representative generated block: " +
            std::string(identifier));
    }
    return *id;
}

const BlockType& requireBlock(
    const BlockRegistry& registry, std::string_view identifier
) {
    return registry.getType(requireBlockId(registry, identifier));
}

size_t faceCount(const BlockModel& model) {
    size_t count = 0;
    for (const BlockModelCuboid& cuboid : model.cuboids()) {
        count += static_cast<size_t>(std::count_if(
            cuboid.faces.begin(), cuboid.faces.end(),
            [](const auto& face) { return face.has_value(); }));
    }
    return count;
}

bool hasCroppedUv(const BlockModel& model) {
    for (const BlockModelCuboid& cuboid : model.cuboids()) {
        for (const auto& optionalFace : cuboid.faces) {
            if (!optionalFace) continue;
            const BlockModelUvRect& uv = optionalFace->uv;
            if (uv.u0 != 0.0f || uv.v0 != 0.0f ||
                uv.u1 != 1.0f || uv.v1 != 1.0f) {
                return true;
            }
        }
    }
    return false;
}

bool hasReversedUv(const BlockModel& model) {
    for (const BlockModelCuboid& cuboid : model.cuboids()) {
        for (const auto& optionalFace : cuboid.faces) {
            if (optionalFace &&
                (optionalFace->uv.u0 > optionalFace->uv.u1 ||
                 optionalFace->uv.v0 > optionalFace->uv.v1)) {
                return true;
            }
        }
    }
    return false;
}

bool hasQuarterTurnedUv(const BlockModel& model) {
    for (const BlockModelCuboid& cuboid : model.cuboids()) {
        for (const auto& optionalFace : cuboid.faces) {
            if (optionalFace &&
                optionalFace->rotation != BlockModelUvRotation::None) {
                return true;
            }
        }
    }
    return false;
}

void checkTextureBindings(
    const BlockType& block, const TextureAtlas& atlas
) {
    CHECK(block.model.geometry);
    CHECK_EQ(block.textures.named().size(), block.model->textureSlots().size());
    for (const std::string& slot : block.model->textureSlots()) {
        const std::string* path = block.textures.find(slot);
        CHECK(path);
        CHECK(atlas.findTexture(*path).isValid());
    }
}

ChunkMesh buildOne(
    const BlockRegistry& registry,
    const TextureAtlas& atlas,
    std::string_view identifier
) {
    Chunk chunk({0, 0, 0});
    chunk.setBlock(1, 1, 1, BlockState{requireBlockId(registry, identifier)});
    return MeshBuilder{}.build({
        .chunk = chunk,
        .registry = registry,
        .atlas = &atlas,
        .neighbors = {},
    });
}

struct PositionRange {
    std::array<float, 3> min = {
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
    };
    std::array<float, 3> max = {
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest(),
    };
};

PositionRange positionRange(const ChunkMesh& mesh) {
    PositionRange result;
    for (const VoxelVertex& vertex : mesh.vertices) {
        const std::array position = {vertex.x, vertex.y, vertex.z};
        for (size_t axis = 0; axis < position.size(); ++axis) {
            result.min[axis] = std::min(result.min[axis], position[axis]);
            result.max[axis] = std::max(result.max[axis], position[axis]);
        }
    }
    return result;
}

void checkMeshCardinality(
    const ChunkMesh& mesh,
    size_t vertexCount,
    size_t indexCount,
    RenderLayer layer
) {
    CHECK_EQ(mesh.vertexCount(), vertexCount);
    CHECK_EQ(mesh.indexCount(), indexCount);
    CHECK_EQ(
        mesh.layers[static_cast<size_t>(layer)].indexCount,
        static_cast<uint32_t>(indexCount));
}

} // namespace

TEST_CASE(GeneratedAssets_LoadNormalizedBlockDefinitions) {
    Rigel::Asset::AssetManager assets;
    assets.loadManifest("manifest.yaml");

    BlockModelRegistry models;
    BlockRegistry preparedRegistry;
    TextureAtlas preparedAtlas;
    const BlockLoadReport report = BlockLoader{}.loadFromManifest(
        assets, models, preparedRegistry, preparedAtlas);
    CHECK(report.modelsLoaded > 0);
    CHECK_EQ(report.modelsFailed, static_cast<size_t>(0));
    CHECK_EQ(report.failed, static_cast<size_t>(0));
    CHECK(report.loaded > 0);
    for (const std::string_view identifier : RequiredMaterials) {
        CHECK(preparedRegistry.findByIdentifier(std::string(identifier)));
    }

#ifdef RIGEL_EXPECT_COSMIC_REACH_0_6_1_ASSETS
    CHECK_EQ(countResources("blocks/", ".yaml"), static_cast<size_t>(2021));
    CHECK_EQ(
        countResources("models/blocks/", ".yaml"), static_cast<size_t>(51));
    CHECK_EQ(
        countResources("models/entities/", ".json"), static_cast<size_t>(16));
    CHECK_EQ(
        countResources("animations/entities/", ".json"), static_cast<size_t>(7));
    CHECK_EQ(countResources("textures/", ".png"), static_cast<size_t>(438));
    CHECK_EQ(countResources("sounds/", ".ogg"), static_cast<size_t>(59));
    CHECK_EQ(report.modelsDiscovered, static_cast<size_t>(51));
    CHECK_EQ(report.modelsLoaded, static_cast<size_t>(51));
    CHECK_EQ(models.size(), static_cast<size_t>(53));
    CHECK_EQ(report.discovered, static_cast<size_t>(2021));
    CHECK_EQ(report.loaded, static_cast<size_t>(2020));
    CHECK_EQ(report.skipped, static_cast<size_t>(1));
    CHECK_EQ(preparedRegistry.size(), static_cast<size_t>(2021));
    CHECK_EQ(preparedAtlas.textureCount(), static_cast<size_t>(276));
#endif
}

TEST_CASE(GeneratedAssets_BuildUploadAndSubmitRepresentativeModels) {
#ifndef RIGEL_EXPECT_COSMIC_REACH_0_6_1_ASSETS
    SKIP_TEST("representative model expectations target Cosmic Reach 0.6.1");
#else
    Rigel::Test::HiddenOpenGLContext context;
    context.require();

    Rigel::Asset::AssetManager assets;
    assets.loadManifest("manifest.yaml");

    WorldResources resources;
    resources.initialize(assets);
    CHECK(resources.initialized());
    CHECK(resources.registry().frozen());
    CHECK_EQ(resources.registry().size(), static_cast<size_t>(2021));
    CHECK_EQ(resources.textureAtlas().textureCount(), static_cast<size_t>(276));

    const auto generator = loadPreparedGeneratorDefinitionSnapshot(
        assets, resources.registry(), "rigel:default");
    WorldGenerator worldGenerator(resources.registry(), generator.data, 1337u);
    ChunkBuffer generatedChunk;
    worldGenerator.generate({0, 3, 0}, generatedChunk);
    CHECK(!generatedChunk.blocks.empty());

    const BlockType& slab = requireBlock(resources.registry(), SlabId);
    const BlockType& stair = requireBlock(resources.registry(), StairId);
    const BlockType& door = requireBlock(resources.registry(), DoorId);
    const BlockType& ladder = requireBlock(resources.registry(), LadderId);
    const BlockType& pistonHead =
        requireBlock(resources.registry(), PistonHeadId);
    for (const BlockType* block : {
             &slab, &stair, &door, &ladder, &pistonHead}) {
        CHECK(!block->model->isFullCube());
        checkTextureBindings(*block, resources.textureAtlas());
    }

    CHECK_EQ(slab.model->cuboids().size(), static_cast<size_t>(1));
    CHECK_EQ(faceCount(*slab.model.geometry), static_cast<size_t>(6));
    CHECK(hasCroppedUv(*slab.model.geometry));
    CHECK_EQ(stair.model->cuboids().size(), static_cast<size_t>(4));
    CHECK_EQ(faceCount(*stair.model.geometry), static_cast<size_t>(10));
    CHECK(hasQuarterTurnedUv(*stair.model.geometry));
    CHECK_EQ(door.model.orientation, BlockModelOrientation::RotateY90);
    CHECK_EQ(faceCount(*door.model.geometry), static_cast<size_t>(6));
    CHECK_EQ(ladder.model.orientation, BlockModelOrientation::RotateY90);
    CHECK_EQ(faceCount(*ladder.model.geometry), static_cast<size_t>(2));
    CHECK_EQ(pistonHead.model.orientation, BlockModelOrientation::RotateY90);
    CHECK_EQ(pistonHead.model->cuboids().size(), static_cast<size_t>(2));
    CHECK_EQ(faceCount(*pistonHead.model.geometry), static_cast<size_t>(12));
    CHECK(hasCroppedUv(*pistonHead.model.geometry));
    CHECK(hasReversedUv(*pistonHead.model.geometry));
    CHECK(hasQuarterTurnedUv(*pistonHead.model.geometry));
    CHECK_EQ(pistonHead.model->cuboids().back().bounds.max[2], 1.25f);

    const ChunkMesh slabMesh = buildOne(
        resources.registry(), resources.textureAtlas(), SlabId);
    checkMeshCardinality(slabMesh, 24, 36, RenderLayer::Transparent);
    const PositionRange slabRange = positionRange(slabMesh);
    CHECK_EQ(slabRange.min[1], 1.0f);
    CHECK_EQ(slabRange.max[1], 1.5f);

    const ChunkMesh stairMesh = buildOne(
        resources.registry(), resources.textureAtlas(), StairId);
    checkMeshCardinality(stairMesh, 40, 60, RenderLayer::Transparent);

    const ChunkMesh doorMesh = buildOne(
        resources.registry(), resources.textureAtlas(), DoorId);
    checkMeshCardinality(doorMesh, 24, 36, RenderLayer::Transparent);
    const PositionRange doorRange = positionRange(doorMesh);
    CHECK_NEAR(doorRange.max[2] - doorRange.min[2], 0.125f, 0.00001f);

    const ChunkMesh ladderMesh = buildOne(
        resources.registry(), resources.textureAtlas(), LadderId);
    checkMeshCardinality(ladderMesh, 8, 12, RenderLayer::Transparent);
    const PositionRange ladderRange = positionRange(ladderMesh);
    CHECK_NEAR(
        ladderRange.max[0] - ladderRange.min[0], 0.00625f, 0.00001f);

    const ChunkMesh pistonMesh = buildOne(
        resources.registry(), resources.textureAtlas(), PistonHeadId);
    checkMeshCardinality(pistonMesh, 48, 72, RenderLayer::Opaque);
    const PositionRange pistonRange = positionRange(pistonMesh);
    CHECK_EQ(pistonRange.min[0], 0.75f);
    CHECK_EQ(pistonRange.max[0], 2.0f);
    CHECK(std::any_of(
        pistonMesh.vertices.begin(), pistonMesh.vertices.end(),
        [](const VoxelVertex& vertex) {
            return vertex.u == 0.375f || vertex.u == 0.625f ||
                   vertex.v == 0.375f || vertex.v == 0.625f;
        }));

    Chunk chunk({0, 0, 0});
    chunk.setBlock(
        2, 2, 2, BlockState{requireBlockId(resources.registry(), SlabId)});
    chunk.setBlock(
        5, 2, 2, BlockState{requireBlockId(resources.registry(), StairId)});
    chunk.setBlock(
        8, 2, 2, BlockState{requireBlockId(resources.registry(), DoorId)});
    chunk.setBlock(
        11, 2, 2, BlockState{requireBlockId(resources.registry(), LadderId)});
    chunk.setBlock(
        15, 2, 2,
        BlockState{requireBlockId(resources.registry(), PistonHeadId)});
    ChunkMesh mesh = MeshBuilder{}.build({
        .chunk = chunk,
        .registry = resources.registry(),
        .atlas = &resources.textureAtlas(),
        .neighbors = {},
    });
    CHECK_EQ(mesh.vertexCount(), static_cast<size_t>(144));
    CHECK_EQ(mesh.indexCount(), static_cast<size_t>(216));
    CHECK_EQ(
        mesh.layers[static_cast<size_t>(RenderLayer::Opaque)].indexCount,
        static_cast<uint32_t>(72));
    CHECK_EQ(
        mesh.layers[static_cast<size_t>(RenderLayer::Transparent)].indexCount,
        static_cast<uint32_t>(144));

    const std::array<std::string_view, 5> expectedTextures = {
        "textures/blocks/foliage/wood_planks.png",
        "textures/blocks/furniture/steel_door_bottom.png",
        "textures/blocks/furniture/ladder_steel.png",
        "textures/blocks/machines/machine_top.png",
        "textures/blocks/machines/piston_advancing_side.png",
    };
    std::set<uint16_t> expectedLayers;
    for (const std::string_view path : expectedTextures) {
        const TextureHandle handle =
            resources.textureAtlas().findTexture(std::string(path));
        CHECK(handle.isValid());
        expectedLayers.insert(handle.index);
    }
    std::set<uint16_t> actualLayers;
    for (const VoxelVertex& vertex : mesh.vertices) {
        actualLayers.insert(vertex.textureLayer);
    }
    CHECK_EQ(actualLayers, expectedLayers);

    WorldMeshStore store;
    const ChunkCoord coord{0, 0, 0};
    store.set(coord, std::move(mesh));
    const auto installed = store.snapshot(coord);
    CHECK(installed.has_value());
    CHECK(!installed->empty);

    WorldRenderContext renderContext;
    renderContext.meshes = &store;
    renderContext.atlas = &resources.textureAtlas();
    renderContext.shader =
        assets.get<Rigel::Asset::ShaderAsset>("shaders/voxel");
    renderContext.renderDistanceWorldUnits = 128.0f;
    glViewport(0, 0, 64, 64);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    ChunkRenderer renderer;
    renderer.render(renderContext);
    CHECK_EQ(renderer.cachedMeshCount(), static_cast<size_t>(1));
    CHECK(renderer.hasDrawnMesh(
        store.storeId(), coord, installed->revision));
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));

    renderer.releaseResources();
    resources.releaseRenderResources();
#endif
}
