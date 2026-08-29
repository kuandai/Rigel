#include "TestFramework.h"

#include "Rigel/Voxel/BlockLoader.h"
#include <array>
#include <span>
#include <string_view>
#include <vector>

using namespace Rigel::Voxel;

namespace {

constexpr std::string_view kStone = R"(
id: test:stone
model: cube
opaque: true
solid: true
layer: opaque
textures:
  all: textures/test/stone.png
)";

constexpr std::string_view kGlass = R"(
id: test:glass
model: cube
opaque: false
solid: true
layer: transparent
textures:
  all: textures/test/glass.png
)";

constexpr std::string_view kGrass = R"(
id: test:grass
model: cube
opaque: true
solid: true
layer: opaque
textures:
  top: textures/test/grass_top.png
  bottom: textures/test/stone.png
  sides: textures/test/grass_side.png
)";

constexpr std::string_view kFullGrass = R"(
id: test:grass[type=full]
model: cube
opaque: true
solid: true
layer: opaque
textures:
  all: textures/test/grass_top.png
)";

BlockDefinitionSource definition(std::string_view path, std::string_view data) {
    return {path, std::span<const char>(data.data(), data.size())};
}

BlockModelDefinitionSource modelDefinition(
    std::string_view path, std::string_view data
) {
    return {path, std::span<const char>(data.data(), data.size())};
}

void addSyntheticTexture(TextureAtlas& atlas, std::string_view path) {
    const std::vector<unsigned char> pixels(
        static_cast<size_t>(atlas.tileSize() * atlas.tileSize() * 4), 255);
    atlas.addTexture(std::string(path), pixels.data());
}

} // namespace

TEST_CASE(BlockLoader_LoadsSyntheticDefinitions) {
    BlockRegistry registry;
    TextureAtlas atlas;
    const std::vector<unsigned char> pixels(
        static_cast<size_t>(atlas.tileSize() * atlas.tileSize() * 4),
        255
    );
    const std::array<const char*, 4> texturePaths = {
        "textures/test/stone.png",
        "textures/test/glass.png",
        "textures/test/grass_top.png",
        "textures/test/grass_side.png"
    };
    for (const char* path : texturePaths) {
        atlas.addTexture(path, pixels.data());
    }

    BlockLoader loader;
    const std::array definitions = {
        definition("blocks/test_stone.yaml", kStone),
        definition("blocks/test_glass.yaml", kGlass),
        definition("blocks/test_grass.yaml", kGrass),
        definition("blocks/test_grass_full.yaml", kFullGrass),
    };

    BlockLoadReport report = loader.loadDefinitions("test", definitions, registry, atlas);
    CHECK_EQ(report.loaded, static_cast<size_t>(4));
    CHECK_EQ(
        report.discovered,
        report.loaded + report.failed + report.skipped
    );
    CHECK(report.representativeFailures.size() <= static_cast<size_t>(3));
    CHECK(registry.hasIdentifier("test:stone"));
    CHECK(registry.hasIdentifier("test:glass"));
    CHECK(registry.hasIdentifier("test:grass"));
    CHECK(registry.hasIdentifier("test:grass[type=full]"));

    const BlockType& stone = registry.getType(*registry.findByIdentifier("test:stone"));
    CHECK_EQ(stone.textures.forFace(Direction::PosX), texturePaths[0]);
    CHECK_EQ(stone.textures.forFace(Direction::NegY), texturePaths[0]);

    const BlockType& glass = registry.getType(*registry.findByIdentifier("test:glass"));
    CHECK(!glass.isOpaque);
    CHECK_EQ(glass.layer, RenderLayer::Transparent);
    CHECK_EQ(glass.textures.forFace(Direction::PosZ), texturePaths[1]);

    const BlockType& grass = registry.getType(*registry.findByIdentifier("test:grass"));
    CHECK_EQ(grass.textures.forFace(Direction::PosY), texturePaths[2]);
    CHECK_EQ(grass.textures.forFace(Direction::NegY), texturePaths[0]);
    CHECK_EQ(grass.textures.forFace(Direction::NegX), texturePaths[3]);

    const BlockType& fullGrass =
        registry.getType(*registry.findByIdentifier("test:grass[type=full]"));
    CHECK_EQ(fullGrass.textures.forFace(Direction::PosY), texturePaths[2]);
    CHECK_EQ(fullGrass.textures.forFace(Direction::NegY), texturePaths[2]);
}

TEST_CASE(BlockLoader_LoadsReusableNormalizedCuboids) {
    constexpr std::string_view modelYaml = R"(
id: post
texture_slots: [side, cap]
cuboids:
  - bounds: [-0.25, 0, 0.125, 1.25, 0.75, 0.875]
    faces:
      pos_x:
        texture: cap
        uv: [1, 0.25, 0, 0.75]
        rotation: 90
        ambient_occlusion: false
        cull: false
      neg_x:
        texture: cap
  - bounds: [0.25, 0.75, 0.25, 0.75, 1.25, 0.75]
    faces:
      pos_y:
        texture: side
)";
    constexpr std::string_view blockYaml = R"(
id: post_block
model: post
opaque: false
solid: true
textures:
  side: textures/test/post_side.png
  cap: textures/test/post_cap.png
)";

    TextureAtlas atlas;
    addSyntheticTexture(atlas, "textures/test/post_side.png");
    addSyntheticTexture(atlas, "textures/test/post_cap.png");
    BlockModelRegistry models;
    BlockRegistry blocks;
    const std::array modelDefinitions = {
        modelDefinition("models/blocks/post.yaml", modelYaml)};
    const std::array blockDefinitions = {
        definition("blocks/post.yaml", blockYaml)};

    BlockLoader loader;
    const BlockLoadReport report = loader.loadDefinitions(
        "test", modelDefinitions, blockDefinitions, models, blocks, atlas);
    CHECK_EQ(report.modelsLoaded, static_cast<size_t>(1));
    CHECK_EQ(report.loaded, static_cast<size_t>(1));
    CHECK_EQ(report.modelsFailed, static_cast<size_t>(0));
    CHECK_EQ(report.failed, static_cast<size_t>(0));

    const auto model = models.find("test:post");
    CHECK(model);
    CHECK_EQ(model->cuboids().size(), static_cast<size_t>(2));
    CHECK_EQ(model->cuboids()[0].bounds.min[0], -0.25f);
    CHECK_EQ(model->cuboids()[0].bounds.max[0], 1.25f);
    CHECK(!model->cuboids()[0].faces[static_cast<size_t>(Direction::PosY)]);
    const auto& face = *model->cuboids()[0].faces[static_cast<size_t>(Direction::PosX)];
    CHECK_EQ(face.uv.u0, 1.0f);
    CHECK_EQ(face.uv.u1, 0.0f);
    CHECK_EQ(face.rotation, BlockModelUvRotation::Quarter);
    CHECK(!face.ambientOcclusion);
    CHECK(!face.cullAgainstOpaqueNeighbor);

    const BlockType& block =
        blocks.getType(*blocks.findByIdentifier("test:post_block"));
    CHECK_EQ(block.model.get(), model.get());
    CHECK_EQ(*block.textures.find("side"), "textures/test/post_side.png");
    CHECK_EQ(*block.textures.find("cap"), "textures/test/post_cap.png");
}

TEST_CASE(BlockLoader_RejectsMalformedNormalizedModelsAtomically) {
    constexpr std::array<std::string_view, 7> invalidModels = {
        R"(id: bad
texture_slots: [all]
cuboids: [{bounds: [0, 0, 0, 1, 1, 1], faces: {}}]
unknown: true
)",
        R"(id: bad
texture_slots: [all]
cuboids: [{bounds: [0, 0, 0, 1, 1], faces: {}}]
)",
        R"(id: bad
texture_slots: [all]
cuboids: [{bounds: [0, 0, 0, 1, 0, 1], faces: {}}]
)",
        R"(id: bad
texture_slots: [all]
cuboids: [{bounds: [0, 0, 0, 1, 1, 1], faces: {diagonal: {texture: all}}}]
)",
        R"(id: bad
texture_slots: [all]
cuboids: [{bounds: [0, 0, 0, 1, 1, 1], faces: {pos_x: {texture: all, uv: [0, 0, 2, 1]}}}]
)",
        R"(id: bad
texture_slots: [all]
cuboids: [{bounds: [0, 0, 0, 1, 1, 1], faces: {pos_x: {texture: all, rotation: 45}}}]
)",
        R"(id: bad
texture_slots: [declared]
cuboids: [{bounds: [0, 0, 0, 1, 1, 1], faces: {pos_x: {texture: missing}}}]
)",
    };

    BlockLoader loader;
    for (size_t index = 0; index < invalidModels.size(); ++index) {
        BlockModelRegistry models;
        BlockRegistry blocks;
        TextureAtlas atlas;
        const std::string path =
            "models/blocks/invalid_" + std::to_string(index) + ".yaml";
        const std::array definitions = {
            modelDefinition(path, invalidModels[index])};
        const BlockLoadReport report = loader.loadDefinitions(
            "test", definitions, std::span<const BlockDefinitionSource>{},
            models, blocks, atlas);
        CHECK_EQ(report.modelsFailed, static_cast<size_t>(1));
        CHECK_EQ(report.modelsLoaded, static_cast<size_t>(0));
        CHECK_EQ(models.size(), static_cast<size_t>(2));
        CHECK_EQ(blocks.size(), static_cast<size_t>(1));
        CHECK_EQ(report.representativeFailures.front().definitionPath, path);
        CHECK(!report.representativeFailures.front().reason.empty());
    }
}

TEST_CASE(BlockLoader_RejectsDuplicateModelIdsWithoutPartialRegistration) {
    constexpr std::string_view validModel = R"(
id: repeated
texture_slots: []
cuboids:
  - bounds: [0, 0, 0, 1, 1, 1]
    faces: {}
)";
    const std::array definitions = {
        modelDefinition("models/blocks/a.yaml", validModel),
        modelDefinition("models/blocks/b.yaml", validModel),
    };
    BlockModelRegistry models;
    BlockRegistry blocks;
    TextureAtlas atlas;

    BlockLoader loader;
    const BlockLoadReport report = loader.loadDefinitions(
        "test", definitions, std::span<const BlockDefinitionSource>{},
        models, blocks, atlas);
    CHECK_EQ(report.modelsFailed, static_cast<size_t>(1));
    CHECK_EQ(report.modelsLoaded, static_cast<size_t>(0));
    CHECK(!models.find("test:repeated"));
    CHECK(report.representativeFailures.front().reason.find("Duplicate") !=
          std::string::npos);
}

TEST_CASE(BlockLoader_UnresolvedBlockSlotRollsBackModelsAndBlocks) {
    constexpr std::string_view modelYaml = R"(
id: two_slots
texture_slots: [first, second]
cuboids:
  - bounds: [0, 0, 0, 1, 1, 1]
    faces:
      pos_x: {texture: first}
      neg_x: {texture: second}
)";
    constexpr std::string_view blockYaml = R"(
id: incomplete
model: two_slots
textures:
  first: textures/test/first.png
)";
    const std::array modelDefinitions = {
        modelDefinition("models/blocks/two_slots.yaml", modelYaml)};
    const std::array blockDefinitions = {
        definition("blocks/incomplete.yaml", blockYaml)};
    BlockModelRegistry models;
    BlockRegistry blocks;
    TextureAtlas atlas;
    addSyntheticTexture(atlas, "textures/test/first.png");

    BlockLoader loader;
    const BlockLoadReport report = loader.loadDefinitions(
        "test", modelDefinitions, blockDefinitions, models, blocks, atlas);
    CHECK_EQ(report.failed, static_cast<size_t>(1));
    CHECK_EQ(report.modelsLoaded, static_cast<size_t>(0));
    CHECK_EQ(report.loaded, static_cast<size_t>(0));
    CHECK(!models.find("test:two_slots"));
    CHECK(!blocks.findByIdentifier("test:incomplete"));
    CHECK(report.representativeFailures.front().reason.find("second") !=
          std::string::npos);
}
