#include "TestFramework.h"

#include "Rigel/Voxel/BlockLoader.h"
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
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

std::string readTextFile(const std::filesystem::path& path) {
    std::ifstream stream(path);
    if (!stream) {
        throw std::runtime_error("Unable to read fixture output: " + path.string());
    }
    std::ostringstream content;
    content << stream.rdbuf();
    return content.str();
}

std::string shellQuote(std::string_view value) {
    std::string result = "'";
    for (const char character : value) {
        if (character == '\'') {
            result += "'\"'\"'";
        } else {
            result += character;
        }
    }
    result += '\'';
    return result;
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
        shading: pos_y
        ambient_occlusion: false
        cull: false
      neg_x:
        texture: cap
        uv: [0, 0, 1, 0]
  - bounds: [0.25, 0.75, 0.25, 0.75, 1.25, 0.75]
    faces:
      pos_y:
        texture: side
  - bounds: [0.5, 0, 0, 0.5, 1, 1]
    faces:
      neg_x:
        texture: side
)";
    constexpr std::string_view blockYaml = R"(
id: post_block
model: post
opaque: false
solid: true
layer: opaque
texture_render_layers:
  cap: transparent
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
    CHECK_EQ(model->cuboids().size(), static_cast<size_t>(3));
    CHECK_EQ(model->cuboids()[0].bounds.min[0], -0.25f);
    CHECK_EQ(model->cuboids()[0].bounds.max[0], 1.25f);
    CHECK(!model->cuboids()[0].faces[static_cast<size_t>(Direction::PosY)]);
    const auto& face = *model->cuboids()[0].faces[static_cast<size_t>(Direction::PosX)];
    CHECK_EQ(face.uv.u0, 1.0f);
    CHECK_EQ(face.uv.u1, 0.0f);
    CHECK_EQ(face.rotation, BlockModelUvRotation::Quarter);
    CHECK_EQ(face.shadingFace, Direction::PosY);
    CHECK(!face.ambientOcclusion);
    CHECK(!face.cullAgainstOpaqueNeighbor);
    CHECK_EQ(
        model->cuboids()[0]
            .faces[static_cast<size_t>(Direction::NegX)]->uv.v1,
        0.0f);

    const BlockType& block =
        blocks.getType(*blocks.findByIdentifier("test:post_block"));
    CHECK_EQ(block.model.get(), model.get());
    CHECK_EQ(*block.textures.find("side"), "textures/test/post_side.png");
    CHECK_EQ(*block.textures.find("cap"), "textures/test/post_cap.png");
    CHECK_EQ(
        block.renderLayerForTextureSlot("side"), RenderLayer::Opaque);
    CHECK_EQ(
        block.renderLayerForTextureSlot("cap"), RenderLayer::Transparent);
}

TEST_CASE(BlockLoader_RejectsRenderLayerForUndeclaredModelSlot) {
    constexpr std::string_view modelYaml = R"(
id: post
texture_slots: [surface]
cuboids:
  - bounds: [0, 0, 0, 1, 1, 1]
    faces:
      pos_y: {texture: surface}
)";
    constexpr std::string_view blockYaml = R"(
id: bad_post
model: post
layer: opaque
texture_render_layers:
  missing: transparent
textures:
  surface: textures/test/post.png
)";
    TextureAtlas atlas;
    addSyntheticTexture(atlas, "textures/test/post.png");
    BlockModelRegistry models;
    BlockRegistry blocks;
    const std::array modelDefinitions = {
        modelDefinition("models/blocks/post.yaml", modelYaml)};
    const std::array blockDefinitions = {
        definition("blocks/bad_post.yaml", blockYaml)};

    const BlockLoadReport report = BlockLoader{}.loadDefinitions(
        "test", modelDefinitions, blockDefinitions, models, blocks, atlas);

    CHECK_EQ(report.failed, static_cast<size_t>(1));
    CHECK_EQ(report.modelsLoaded, static_cast<size_t>(0));
    CHECK_EQ(report.loaded, static_cast<size_t>(0));
    CHECK(report.representativeFailures.front().reason.find("missing") !=
          std::string::npos);
}

TEST_CASE(BlockLoader_LoadsImportedSingleCuboidFixture) {
    const std::filesystem::path fixtureRoot =
        std::filesystem::current_path() / "single-cuboid-import";
    const std::filesystem::path fixtureScript =
        std::filesystem::path(RIGEL_TEST_SOURCE_DIRECTORY) /
        "tests/single_cuboid_fixture.py";
    const std::string command =
        shellQuote(RIGEL_TEST_PYTHON_EXECUTABLE) + " " +
        shellQuote(fixtureScript.string()) + " " +
        shellQuote(fixtureRoot.string());
    CHECK_EQ(std::system(command.c_str()), 0);

    const std::filesystem::path assets = fixtureRoot / ".rigel/assets";
    const std::string modelYaml =
        readTextFile(assets / "models/blocks/ledge.yaml");
    const std::string baseYaml =
        readTextFile(assets / "blocks/test__ledge.yaml");
    const std::string generatedYaml =
        readTextFile(assets / "blocks/test__ledge[facing=east].yaml");
    const std::array modelDefinitions = {
        modelDefinition("models/blocks/ledge.yaml", modelYaml)};
    const std::array blockDefinitions = {
        definition("blocks/test__ledge.yaml", baseYaml),
        definition("blocks/test__ledge[facing=east].yaml", generatedYaml),
    };

    TextureAtlas atlas;
    addSyntheticTexture(atlas, "textures/blocks/ledge.png");
    BlockModelRegistry models;
    BlockRegistry blocks;
    const BlockLoadReport report = BlockLoader{}.loadDefinitions(
        "base", modelDefinitions, blockDefinitions, models, blocks, atlas);
    CHECK_EQ(report.modelsLoaded, static_cast<size_t>(1));
    CHECK_EQ(report.loaded, static_cast<size_t>(2));
    CHECK_EQ(report.modelsFailed, static_cast<size_t>(0));
    CHECK_EQ(report.failed, static_cast<size_t>(0));

    const auto model = models.find("base:block_model/ledge");
    CHECK(model);
    CHECK_EQ(model->cuboids().size(), static_cast<size_t>(1));
    CHECK_EQ(model->cuboids().front().bounds.min[0], -0.15625f);
    CHECK_EQ(model->cuboids().front().bounds.max[0], 1.15625f);
    CHECK(!model->cuboids().front().faces[static_cast<size_t>(Direction::NegX)]);
    const auto& face = *model->cuboids().front().faces[
        static_cast<size_t>(Direction::PosX)];
    CHECK_EQ(face.uv.u0, 0.9375f);
    CHECK_EQ(face.uv.u1, 0.1875f);
    CHECK_EQ(face.rotation, BlockModelUvRotation::Quarter);
    CHECK(!face.ambientOcclusion);
    CHECK(!face.cullAgainstOpaqueNeighbor);

    const BlockType& base =
        blocks.getType(*blocks.findByIdentifier("test:ledge"));
    const BlockType& generated =
        blocks.getType(*blocks.findByIdentifier("test:ledge[facing=east]"));
    CHECK_EQ(base.model.geometry.get(), model.get());
    CHECK_EQ(generated.model.geometry.get(), model.get());
    CHECK_EQ(generated.model.orientation, BlockModelOrientation::RotateY90);
}

TEST_CASE(BlockLoader_LoadsMeasuredOrientationsAsSharedModelInstances) {
    constexpr std::string_view modelYaml = R"(
id: directional
texture_slots: []
cuboids:
  - bounds: [-0.25, 0.125, 0.25, 0.75, 0.625, 1.25]
    faces: {}
)";
    struct OrientationCase {
        std::array<int, 3> angles;
        BlockModelOrientation expected;
        bool rotateTopBottom;
    };
    constexpr std::array cases = {
        OrientationCase{{0, 0, 0}, BlockModelOrientation::Identity, false},
        OrientationCase{{90, 0, 0}, BlockModelOrientation::RotateX90, true},
        OrientationCase{{270, 0, 0}, BlockModelOrientation::RotateX270, false},
        OrientationCase{{0, 90, 0}, BlockModelOrientation::RotateY90, false},
        OrientationCase{{0, 180, 0}, BlockModelOrientation::RotateY180, false},
        OrientationCase{{0, 270, 0}, BlockModelOrientation::RotateY270, false},
        OrientationCase{{0, 0, 90}, BlockModelOrientation::RotateZ90, true},
    };

    std::vector<std::string> yaml;
    yaml.reserve(cases.size());
    std::vector<BlockDefinitionSource> blockDefinitions;
    blockDefinitions.reserve(cases.size());
    for (size_t index = 0; index < cases.size(); ++index) {
        const auto& item = cases[index];
        yaml.push_back(
            "id: oriented_" + std::to_string(index) +
            "\nmodel: directional\norientation: [" +
            std::to_string(item.angles[0]) + ", " +
            std::to_string(item.angles[1]) + ", " +
            std::to_string(item.angles[2]) + "]\nrotate_top_bottom: " +
            (item.rotateTopBottom ? "true" : "false") +
            "\ntextures: {}\n");
        blockDefinitions.push_back(definition(
            "blocks/oriented.yaml", yaml.back()));
    }

    BlockModelRegistry models;
    BlockRegistry blocks;
    TextureAtlas atlas;
    const std::array modelDefinitions = {
        modelDefinition("models/blocks/directional.yaml", modelYaml)};
    const BlockLoadReport report = BlockLoader{}.loadDefinitions(
        "test", modelDefinitions, blockDefinitions, models, blocks, atlas);

    CHECK_EQ(report.loaded, cases.size());
    const auto sharedModel = models.find("test:directional");
    CHECK(sharedModel);
    for (size_t index = 0; index < cases.size(); ++index) {
        const BlockType& block = blocks.getType(
            *blocks.findByIdentifier("test:oriented_" + std::to_string(index)));
        CHECK_EQ(block.model.get(), sharedModel.get());
        CHECK_EQ(block.model.orientation, cases[index].expected);
        CHECK_EQ(block.model.rotateTopBottomUv, cases[index].rotateTopBottom);
    }
}

TEST_CASE(BlockLoader_RejectsUnmeasuredAndComposedOrientationsAtomically) {
    constexpr std::string_view modelYaml = R"(
id: directional
texture_slots: []
cuboids: [{bounds: [0, 0, 0, 1, 1, 1], faces: {}}]
)";
    constexpr std::array<std::string_view, 9> invalidBlocks = {
        R"(id: bad
model: directional
orientation: 90
textures: {}
)",
        R"(id: bad
model: directional
orientation: [90, 0]
textures: {}
)",
        R"(id: bad
model: directional
orientation: [east, 0, 0]
textures: {}
)",
        R"(id: bad
model: directional
orientation: [45, 0, 0]
textures: {}
)",
        R"(id: bad
model: directional
orientation: [180, 0, 0]
textures: {}
)",
        R"(id: bad
model: directional
orientation: [0, 0, 270]
textures: {}
)",
        R"(id: bad
model: directional
orientation: [90, 90, 0]
textures: {}
)",
        R"(id: bad
model: directional
orientation: [0, 0, 90]
rotate_top_bottom: quarter
textures: {}
)",
        R"(id: bad
model: directional
orientation: [0, 0, 0]
rotate_top_bottom: true
textures: {}
)",
    };

    for (size_t index = 0; index < invalidBlocks.size(); ++index) {
        BlockModelRegistry models;
        BlockRegistry blocks;
        TextureAtlas atlas;
        const std::array modelDefinitions = {
            modelDefinition("models/blocks/directional.yaml", modelYaml)};
        const std::array blockDefinitions = {definition(
            "blocks/bad.yaml", invalidBlocks[index])};
        const BlockLoadReport report = BlockLoader{}.loadDefinitions(
            "test", modelDefinitions, blockDefinitions, models, blocks, atlas);

        CHECK_EQ(report.failed, static_cast<size_t>(1));
        CHECK_EQ(report.loaded, static_cast<size_t>(0));
        CHECK_EQ(report.modelsLoaded, static_cast<size_t>(0));
        CHECK_EQ(models.size(), static_cast<size_t>(2));
        CHECK_EQ(blocks.size(), static_cast<size_t>(1));
        CHECK(!models.find("test:directional"));
        CHECK(!blocks.findByIdentifier("test:bad"));
    }
}

TEST_CASE(BlockLoader_RejectsMalformedNormalizedModelsAtomically) {
    constexpr std::array<std::string_view, 8> invalidModels = {
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
        R"(id: bad
texture_slots: [all]
cuboids: [{bounds: [0, 0, 0, 1, 1, 1], faces: {pos_x: {texture: all, shading: diagonal}}}]
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

TEST_CASE(BlockLoader_BlockCapacityFailureDoesNotPublishModels) {
    constexpr std::string_view modelYaml = R"(
id: capacity_model
texture_slots: []
cuboids:
  - bounds: [0, 0, 0, 1, 1, 1]
    faces: {}
)";
    constexpr std::string_view blockYaml = R"(
id: over_capacity
model: capacity_model
textures: {}
)";
    const std::array modelDefinitions = {
        modelDefinition("models/blocks/capacity_model.yaml", modelYaml)};
    const std::array blockDefinitions = {
        definition("blocks/over_capacity.yaml", blockYaml)};
    BlockModelRegistry models;
    BlockRegistry blocks;
    TextureAtlas atlas;
    for (size_t index = blocks.size(); index < 65535; ++index) {
        BlockType type;
        type.model = BlockModel::empty();
        blocks.registerBlock("test:filler_" + std::to_string(index),
                             std::move(type));
    }
    const size_t originalBlockCount = blocks.size();

    BlockLoader loader;
    CHECK_THROWS(loader.loadDefinitions(
        "test", modelDefinitions, blockDefinitions, models, blocks, atlas));

    CHECK_EQ(models.size(), static_cast<size_t>(2));
    CHECK_EQ(blocks.size(), originalBlockCount);
    CHECK_EQ(atlas.textureCount(), static_cast<size_t>(0));
    CHECK(!models.find("test:capacity_model"));
    CHECK(!blocks.findByIdentifier("test:over_capacity"));
}
