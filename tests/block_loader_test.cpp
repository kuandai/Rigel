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
