#include "TestFramework.h"

#include "Rigel/Voxel/BlockGalleryCatalog.h"
#include "Rigel/Voxel/BlockGalleryTargetPresentation.h"
#include "Rigel/Voxel/BlockRegistry.h"

#include <array>
#include <memory>
#include <set>
#include <string>
#include <tuple>
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

BlockID addCollisionSpecimen(
    BlockRegistry& registry,
    const std::string& identifier,
    BlockCollisionShape collision
) {
    BlockType type;
    type.identifier = identifier;
    type.textures = FaceTextures::uniform("invented/collision.png");
    type.collision = std::move(collision);
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
    type.textureRenderLayers.emplace("accent", RenderLayer::Cutout);
    type.isOpaque = false;
    return registry.registerBlock(identifier, std::move(type));
}

BlockID addPartialDiagnostic(
    BlockRegistry& registry,
    const std::string& identifier,
    bool opaqueCoverage,
    bool cullSameType
) {
    BlockModelCuboid cuboid;
    cuboid.bounds.max = {1.0f, 0.5f, 1.0f};
    for (size_t index = 0; index < DirectionCount; ++index) {
        cuboid.faces[index] = BlockModelFace{
            .textureSlot = "surface",
            .cullAgainstOpaqueNeighbor = opaqueCoverage,
        };
    }

    BlockType type;
    type.identifier = identifier;
    type.model = BlockModelInstance(std::make_shared<const BlockModel>(
        identifier + "_model",
        std::vector<std::string>{"surface"},
        std::vector<BlockModelCuboid>{cuboid}));
    type.isOpaque = opaqueCoverage;
    type.cullSameType = cullSameType;
    return registry.registerBlock(identifier, std::move(type));
}

BlockID addLayeredSpecimen(
    BlockRegistry& registry,
    const std::string& identifier,
    std::vector<std::string> slots,
    RenderLayer defaultLayer,
    const std::vector<std::pair<std::string, RenderLayer>>& overrides = {}
) {
    BlockModelCuboid cuboid;
    cuboid.bounds.max = {1.0f, 1.0f, 1.0f};
    if (!slots.empty()) {
        cuboid.faces[static_cast<size_t>(Direction::PosY)] = BlockModelFace{
            .textureSlot = slots.front(),
        };
    }

    BlockType type;
    type.identifier = identifier;
    type.model = BlockModelInstance(std::make_shared<const BlockModel>(
        identifier + "_model",
        slots,
        std::vector<BlockModelCuboid>{cuboid}));
    for (const std::string& slot : slots) {
        type.textures.bind(slot, "invented/" + slot + ".png");
    }
    type.layer = defaultLayer;
    for (const auto& [slot, layer] : overrides) {
        type.textureRenderLayers.emplace(slot, layer);
    }
    return registry.registerBlock(identifier, std::move(type));
}

BlockTarget targetAt(
    BlockGalleryWorldPosition position,
    BlockID blockId
) {
    return {
        .block = {position.x, position.y, position.z},
        .normal = {0, 0, 1},
        .state = BlockState{blockId},
        .distance = 3.0f,
    };
}

BlockTarget targetAt(
    const BlockGalleryCatalogEntry& entry,
    BlockID blockId
) {
    return targetAt(entry.specimenPosition, blockId);
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
    CHECK_EQ(
        presentation->effectiveRenderLayers,
        std::string("cutout + transparent"));
    CHECK_EQ(
        presentation->textureSlotRenderLayers,
        std::string("primary=transparent, accent=cutout"));
    CHECK(!presentation->opaque);
    CHECK_EQ(presentation->collision, std::string("full cube"));
    CHECK(!presentation->fullCube);
    CHECK(!presentation->cullSameType);
    CHECK_EQ(presentation->textureBindingCount, static_cast<size_t>(2));
    CHECK(!presentation->cullingDiagnostic);
}

TEST_CASE(BlockGalleryTargetPresentation_ReportsEveryCollisionKindAndFallback) {
    BlockRegistry registry;
    const BlockID emptyId = addCollisionSpecimen(
        registry,
        "invented:collision_empty",
        BlockCollisionShape::empty());
    const BlockID fullId = addCollisionSpecimen(
        registry,
        "invented:collision_full",
        BlockCollisionShape::fullCube());
    const BlockID singleId = addCollisionSpecimen(
        registry,
        "invented:collision_single",
        BlockCollisionShape::boxes({
            {{0.25f, 0.0f, 0.25f}, {0.75f, 0.5f, 0.75f}},
        }));
    const BlockID multipleId = addCollisionSpecimen(
        registry,
        "invented:collision_multiple",
        BlockCollisionShape::boxes({
            {{0.0f, 0.0f, 0.0f}, {1.0f, 0.25f, 1.0f}},
            {{0.0f, 0.25f, 0.0f}, {0.25f, 1.0f, 1.0f}},
            {{0.75f, 0.25f, 0.0f}, {1.0f, 1.0f, 1.0f}},
        }));
    const BlockID fallbackId = addCollisionSpecimen(
        registry,
        "invented:collision_fallback",
        BlockCollisionShape::fullCube(
            BlockCollisionShape::Provenance::ConservativeFallback));
    registry.freeze();
    const BlockGalleryCatalog catalog(registry);

    for (const auto& [id, expected] :
         std::array{
             std::pair{emptyId, std::string("none")},
             std::pair{fullId, std::string("full cube")},
             std::pair{singleId, std::string("one box")},
             std::pair{multipleId, std::string("3 boxes")},
             std::pair{
                 fallbackId,
                 std::string("full cube (conservative fallback)")},
         }) {
        const BlockGalleryCatalogEntry* entry = catalog.findByBlockId(id);
        CHECK(entry != nullptr);
        const auto presentation = makeBlockGalleryTargetPresentation(
            catalog, registry, targetAt(*entry, id));
        CHECK(presentation.has_value());
        CHECK_EQ(presentation->collision, expected);
    }
}

TEST_CASE(BlockGalleryTargetPresentation_ReportsStableCompactLayerMappings) {
    BlockRegistry registry;
    const BlockID opaqueId = addLayeredSpecimen(
        registry,
        "invented:opaque",
        {"surface"},
        RenderLayer::Opaque);
    const BlockID cutoutId = addLayeredSpecimen(
        registry,
        "invented:cutout",
        {"surface"},
        RenderLayer::Cutout);
    const BlockID transparentId = addLayeredSpecimen(
        registry,
        "invented:transparent",
        {"surface"},
        RenderLayer::Transparent);
    const BlockID defaultUnionId = addLayeredSpecimen(
        registry,
        "invented:default_union",
        {"glass", "wood"},
        RenderLayer::Cutout,
        {
            {"glass", RenderLayer::Transparent},
            {"wood", RenderLayer::Opaque},
        });

    const std::vector<std::string> mixedSlots = {
        "glass", "mask", "wood", "glow", "detail", "fluid",
    };
    const BlockID mixedForwardId = addLayeredSpecimen(
        registry,
        "invented:mixed_forward",
        mixedSlots,
        RenderLayer::Transparent,
        {
            {"mask", RenderLayer::Cutout},
            {"wood", RenderLayer::Opaque},
            {"glow", RenderLayer::Emissive},
            {"detail", RenderLayer::Opaque},
        });
    const BlockID mixedReverseId = addLayeredSpecimen(
        registry,
        "invented:mixed_reverse",
        mixedSlots,
        RenderLayer::Transparent,
        {
            {"detail", RenderLayer::Opaque},
            {"glow", RenderLayer::Emissive},
            {"wood", RenderLayer::Opaque},
            {"mask", RenderLayer::Cutout},
        });
    registry.freeze();
    const BlockGalleryCatalog catalog(registry);

    const auto presentationFor = [&](BlockID id) {
        const BlockGalleryCatalogEntry* entry = catalog.findByBlockId(id);
        CHECK(entry != nullptr);
        return makeBlockGalleryTargetPresentation(
            catalog, registry, targetAt(*entry, id));
    };

    for (const auto& [id, expectedLayer] :
         std::array{
             std::pair{opaqueId, std::string("opaque")},
             std::pair{cutoutId, std::string("cutout")},
             std::pair{transparentId, std::string("transparent")},
         }) {
        const auto presentation = presentationFor(id);
        CHECK(presentation.has_value());
        CHECK_EQ(presentation->effectiveRenderLayers, expectedLayer);
        CHECK(presentation->textureSlotRenderLayers.empty());
    }

    const auto defaultUnion = presentationFor(defaultUnionId);
    CHECK(defaultUnion.has_value());
    CHECK_EQ(
        defaultUnion->effectiveRenderLayers,
        std::string("opaque + cutout + transparent"));
    CHECK_EQ(
        defaultUnion->textureSlotRenderLayers,
        std::string("glass=transparent, wood=opaque"));

    const auto mixedForward = presentationFor(mixedForwardId);
    const auto mixedReverse = presentationFor(mixedReverseId);
    CHECK(mixedForward.has_value());
    CHECK(mixedReverse.has_value());
    CHECK_EQ(
        mixedForward->effectiveRenderLayers,
        std::string("opaque + cutout + transparent + emissive"));
    CHECK_EQ(
        mixedForward->textureSlotRenderLayers,
        std::string(
            "glass=transparent, mask=cutout, wood=opaque, glow=emissive, "
            "... (+2 more)"));
    CHECK_EQ(
        mixedReverse->effectiveRenderLayers,
        mixedForward->effectiveRenderLayers);
    CHECK_EQ(
        mixedReverse->textureSlotRenderLayers,
        mixedForward->textureSlotRenderLayers);
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

    CHECK(!makeBlockGalleryTargetPresentation(
        catalog,
        registry,
        targetAt({123, BlockGalleryCatalog::SpecimenHeight, 123},
                 BlockRegistry::airId())));
}

TEST_CASE(BlockGalleryTargetPresentation_IdentifiesEveryDiagnosticPairCell) {
    BlockRegistry registry;
    const BlockID baselineId = addFullCube(
        registry, "invented:baseline");
    const BlockID coverageId = addPartialDiagnostic(
        registry, "invented:coverage", true, false);
    const BlockID sameTypeId = addPartialDiagnostic(
        registry, "invented:same_type", false, true);
    registry.freeze();
    const BlockGalleryCatalog catalog(registry);

    const auto& placements = catalog.cullingDiagnosticPlacements();
    CHECK_EQ(placements.size(), static_cast<size_t>(6));

    BlockRegistry reorderedRegistry;
    addPartialDiagnostic(
        reorderedRegistry, "invented:same_type", false, true);
    addPartialDiagnostic(
        reorderedRegistry, "invented:coverage", true, false);
    addFullCube(reorderedRegistry, "invented:baseline");
    reorderedRegistry.freeze();
    const BlockGalleryCatalog reorderedCatalog(reorderedRegistry);
    const auto& reorderedPlacements =
        reorderedCatalog.cullingDiagnosticPlacements();
    CHECK_EQ(reorderedPlacements.size(), placements.size());
    for (size_t index = 0; index < placements.size(); ++index) {
        CHECK_EQ(reorderedPlacements[index].caseKind,
                 placements[index].caseKind);
        CHECK_EQ(reorderedPlacements[index].label, placements[index].label);
        CHECK_EQ(reorderedPlacements[index].caseOrdinal,
                 placements[index].caseOrdinal);
        CHECK_EQ(reorderedPlacements[index].caseCount,
                 placements[index].caseCount);
        CHECK_EQ(reorderedPlacements[index].pairPosition,
                 placements[index].pairPosition);
        CHECK_EQ(reorderedPlacements[index].pairOrdinal,
                 placements[index].pairOrdinal);
        CHECK_EQ(reorderedPlacements[index].pairCount,
                 placements[index].pairCount);
        CHECK_EQ(reorderedPlacements[index].sourceIdentifier,
                 placements[index].sourceIdentifier);
        CHECK_EQ(reorderedPlacements[index].worldPosition,
                 placements[index].worldPosition);
    }

    const std::array<BlockGalleryCullingCaseKind, 3> expectedKinds = {
        BlockGalleryCullingCaseKind::OpaqueFullCube,
        BlockGalleryCullingCaseKind::SameType,
        BlockGalleryCullingCaseKind::OpaqueCoverage,
    };
    const std::array<BlockID, 3> expectedIds = {
        baselineId,
        sameTypeId,
        coverageId,
    };
    const std::array<std::string, 3> expectedLabels = {
        "Opaque full-cube baseline",
        "Same-type shared boundary",
        "Complete opposite-face coverage",
    };
    const std::array<std::string, 3> expectedIdentifiers = {
        "invented:baseline",
        "invented:same_type",
        "invented:coverage",
    };

    std::set<std::tuple<int, int, int>> occupiedCells;
    for (size_t caseIndex = 0; caseIndex < expectedKinds.size(); ++caseIndex) {
        const auto& first = placements[caseIndex * 2];
        const auto& second = placements[caseIndex * 2 + 1];
        CHECK_EQ(first.caseKind, expectedKinds[caseIndex]);
        CHECK_EQ(second.caseKind, expectedKinds[caseIndex]);
        CHECK_EQ(first.label, expectedLabels[caseIndex]);
        CHECK_EQ(second.label, expectedLabels[caseIndex]);
        CHECK_EQ(first.caseOrdinal, caseIndex + 1);
        CHECK_EQ(second.caseOrdinal, caseIndex + 1);
        CHECK_EQ(first.caseCount, static_cast<size_t>(3));
        CHECK_EQ(second.caseCount, static_cast<size_t>(3));
        CHECK_EQ(
            first.pairPosition,
            BlockGalleryDiagnosticPairPosition::First);
        CHECK_EQ(
            second.pairPosition,
            BlockGalleryDiagnosticPairPosition::Second);
        CHECK_EQ(first.pairOrdinal, static_cast<size_t>(1));
        CHECK_EQ(second.pairOrdinal, static_cast<size_t>(2));
        CHECK_EQ(first.pairCount, static_cast<size_t>(2));
        CHECK_EQ(second.pairCount, static_cast<size_t>(2));
        CHECK_EQ(first.sourceBlockId, expectedIds[caseIndex]);
        CHECK_EQ(second.sourceBlockId, expectedIds[caseIndex]);
        CHECK_EQ(first.sourceIdentifier, expectedIdentifiers[caseIndex]);
        CHECK_EQ(second.sourceIdentifier, first.sourceIdentifier);
        CHECK_EQ(second.worldPosition.x, first.worldPosition.x + 1);
        CHECK_EQ(second.worldPosition.y, first.worldPosition.y);
        CHECK_EQ(second.worldPosition.z, first.worldPosition.z);

        for (const auto* placement : {&first, &second}) {
            CHECK(occupiedCells.emplace(
                placement->worldPosition.x,
                placement->worldPosition.y,
                placement->worldPosition.z).second);
            CHECK_EQ(
                catalog.findCullingDiagnosticByPosition(
                    placement->worldPosition),
                placement);
            CHECK(!catalog.findBySpecimenPosition(
                placement->worldPosition));

            const auto presentation = makeBlockGalleryTargetPresentation(
                catalog,
                registry,
                targetAt(
                    placement->worldPosition,
                    placement->sourceBlockId));
            CHECK(presentation.has_value());
            CHECK_EQ(
                presentation->blockStateIdentifier,
                placement->sourceIdentifier);
            CHECK(presentation->cullingDiagnostic.has_value());
            CHECK_EQ(
                presentation->cullingDiagnostic->caseKind,
                placement->caseKind);
            CHECK_EQ(
                presentation->cullingDiagnostic->label,
                placement->label);
            CHECK_EQ(
                presentation->cullingDiagnostic->caseOrdinal,
                placement->caseOrdinal);
            CHECK_EQ(
                presentation->cullingDiagnostic->caseCount,
                placement->caseCount);
            CHECK_EQ(
                presentation->cullingDiagnostic->pairPosition,
                placement->pairPosition);
            CHECK_EQ(
                presentation->cullingDiagnostic->pairOrdinal,
                placement->pairOrdinal);
            CHECK_EQ(
                presentation->cullingDiagnostic->pairCount,
                placement->pairCount);
            CHECK_EQ(presentation->catalogPosition, static_cast<size_t>(0));
            CHECK_EQ(presentation->modelIdentifier,
                     registry.getType(placement->sourceBlockId)
                         .model->identifier());
            CHECK_EQ(presentation->renderLayer, std::string("opaque"));
            CHECK_EQ(
                presentation->effectiveRenderLayers,
                std::string("opaque"));
            CHECK(presentation->textureSlotRenderLayers.empty());
            CHECK_EQ(presentation->collision, std::string("full cube"));
            CHECK_EQ(presentation->fullCube, caseIndex == 0);
            CHECK_EQ(presentation->cullSameType, caseIndex == 1);
            CHECK_EQ(presentation->opaque, caseIndex != 1);
        }
    }

    const int diagnosticZ = placements.front().worldPosition.z;
    CHECK(!catalog.findCullingDiagnosticByPosition({2, 1, diagnosticZ}));
    CHECK(!makeBlockGalleryTargetPresentation(
        catalog, registry, targetAt({0, 0, diagnosticZ}, baselineId)));
    CHECK(!makeBlockGalleryTargetPresentation(
        catalog,
        registry,
        targetAt({2, BlockGalleryCatalog::SpecimenHeight, diagnosticZ},
                 BlockRegistry::airId())));
    CHECK(!makeBlockGalleryTargetPresentation(
        catalog,
        registry,
        targetAt(placements.front().worldPosition, coverageId)));
}
