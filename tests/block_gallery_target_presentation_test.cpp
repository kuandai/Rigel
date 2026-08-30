#include "TestFramework.h"

#include "Rigel/Voxel/BlockGalleryCatalog.h"
#include "Rigel/Voxel/BlockGalleryTargetPresentation.h"
#include "Rigel/Voxel/BlockRegistry.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace Rigel::Voxel;

BlockID addFullCube(BlockRegistry& registry, const std::string& identifier) {
    BlockType type;
    type.identifier = identifier;
    type.textures = FaceTextures::uniform("invented/full_cube.png");
    return registry.registerBlock(identifier, std::move(type));
}

BlockID addPresentedSpecimen(
    BlockRegistry& registry,
    const std::string& identifier
) {
    BlockModelCuboid lower;
    lower.bounds.max = {1.0f, 0.5f, 1.0f};
    lower.faces[static_cast<size_t>(Direction::PosY)] = BlockModelFace{
        .textureSlot = "primary",
    };
    BlockModelCuboid upper;
    upper.bounds.min = {0.25f, 0.5f, 0.25f};
    upper.bounds.max = {0.75f, 1.0f, 0.75f};
    upper.faces[static_cast<size_t>(Direction::PosY)] = BlockModelFace{
        .textureSlot = "accent",
    };

    BlockType type;
    type.identifier = identifier;
    type.model = BlockModelInstance(std::make_shared<const BlockModel>(
        "invented:two_cuboid_model",
        std::vector<std::string>{"primary", "accent"},
        std::vector<BlockModelCuboid>{lower, upper}));
    type.model.orientation = BlockModelOrientation::RotateZ90;
    type.textures.bind("primary", "invented/primary.png");
    type.textures.bind("accent", "invented/accent.png");
    type.layer = RenderLayer::Transparent;
    type.isOpaque = false;
    type.isSolid = true;
    return registry.registerBlock(identifier, std::move(type));
}

BlockTarget targetAt(
    const BlockGalleryCatalogEntry& entry,
    BlockID blockId
) {
    return {
        .block = {
            entry.specimenPosition.x,
            entry.specimenPosition.y,
            entry.specimenPosition.z,
        },
        .normal = {0, 0, 1},
        .state = BlockState{blockId},
        .distance = 3.0f,
    };
}

} // namespace

TEST_CASE(BlockGalleryTargetPresentation_UsesCatalogAndRuntimeMetadata) {
    BlockRegistry registry;
    addFullCube(registry, "invented:before");
    const BlockID specimenId = addPresentedSpecimen(
        registry, "invented:target[state=raised]");
    registry.freeze();
    const BlockGalleryCatalog catalog(registry);
    const BlockGalleryCatalogEntry* entry = catalog.findByBlockId(specimenId);
    CHECK(entry != nullptr);

    const auto presentation = makeBlockGalleryTargetPresentation(
        catalog, registry, targetAt(*entry, specimenId));

    CHECK(presentation.has_value());
    CHECK_EQ(
        presentation->blockStateIdentifier,
        std::string("invented:target[state=raised]"));
    CHECK_EQ(presentation->catalogPosition, static_cast<size_t>(2));
    CHECK_EQ(presentation->catalogSize, static_cast<size_t>(2));
    CHECK_EQ(presentation->gridCoordinate, entry->gridCoordinate);
    CHECK_EQ(
        presentation->modelIdentifier,
        std::string("invented:two_cuboid_model"));
    CHECK_EQ(presentation->cuboidCount, static_cast<size_t>(2));
    CHECK_EQ(presentation->orientation, std::string("rotate_z_90"));
    CHECK_EQ(presentation->renderLayer, std::string("transparent"));
    CHECK(!presentation->opaque);
    CHECK(presentation->solid);
    CHECK_EQ(presentation->textureBindingCount, static_cast<size_t>(2));
}

TEST_CASE(BlockGalleryTargetPresentation_RejectsNonSpecimenCellsSafely) {
    BlockRegistry registry;
    const BlockID specimenId = addPresentedSpecimen(
        registry, "invented:target");
    const BlockID otherId = addFullCube(registry, "invented:other");
    registry.freeze();
    const BlockGalleryCatalog catalog(registry);
    const BlockGalleryCatalogEntry* entry = catalog.findByBlockId(specimenId);
    CHECK(entry != nullptr);

    BlockTarget floor = targetAt(*entry, specimenId);
    floor.block.y = 0;
    CHECK(!makeBlockGalleryTargetPresentation(catalog, registry, floor));

    BlockTarget diagnostic = targetAt(*entry, specimenId);
    diagnostic.block.x += 1;
    CHECK(!makeBlockGalleryTargetPresentation(catalog, registry, diagnostic));

    CHECK(!makeBlockGalleryTargetPresentation(
        catalog, registry, targetAt(*entry, otherId)));
}
