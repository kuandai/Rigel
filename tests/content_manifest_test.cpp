#include "TestFramework.h"
#include "GeneratorDefinitionTestRegistry.h"

#include "Rigel/Simulation/ContentManifest.h"
#include "Rigel/Voxel/BlockGalleryCatalog.h"
#include "Rigel/Voxel/BlockGalleryChunkGenerator.h"
#include "Rigel/Voxel/BlockRegistry.h"
#include "Rigel/Voxel/WorldGenerator.h"

#include <memory>
#include <string>
#include <vector>

namespace {

using namespace Rigel;

Voxel::BlockType block(
    std::string identifier,
    bool raised,
    float modelBottom = 0.25f,
    float collisionBottom = 0.25f,
    bool alternatePresentation = false,
    bool omitFace = false,
    Voxel::BlockModelOrientation orientation =
        Voxel::BlockModelOrientation::Identity
) {
    Voxel::BlockType type;
    type.identifier = std::move(identifier);
    type.isOpaque = alternatePresentation ? raised : !raised;
    type.cullSameType = alternatePresentation;
    type.emittedLight = alternatePresentation ? 12 : (raised ? 3 : 0);
    type.lightAttenuation = alternatePresentation ? 2 : 15;
    if (raised) {
        Voxel::BlockModelCuboid cuboid{
            .bounds = {{0.0f, modelBottom, 0.0f}, {1.0f, 1.0f, 1.0f}},
        };
        if (!omitFace) {
            cuboid.faces[static_cast<size_t>(Voxel::Direction::PosY)] =
                Voxel::BlockModelFace{
                    .textureSlot = alternatePresentation ? "renamed" : "surface",
                    .uv = alternatePresentation
                        ? Voxel::BlockModelUvRect{0.25f, 0.5f, 0.75f, 1.0f}
                        : Voxel::BlockModelUvRect{},
                    .rotation = alternatePresentation
                        ? Voxel::BlockModelUvRotation::Quarter
                        : Voxel::BlockModelUvRotation::None,
                    .shadingFace = alternatePresentation
                        ? std::optional(Voxel::Direction::NegX)
                        : std::nullopt,
                    .ambientOcclusion = alternatePresentation,
                    .cullAgainstOpaqueNeighbor = alternatePresentation,
                };
        }
        type.model = std::make_shared<const Voxel::BlockModel>(
            alternatePresentation ? "fixture:renamed" : "fixture:raised",
            std::vector<std::string>{
                alternatePresentation ? "renamed" : "surface"},
            std::vector<Voxel::BlockModelCuboid>{std::move(cuboid)});
        type.model.orientation = orientation;
        type.model.rotateTopBottomUv = alternatePresentation;
        type.collision = Voxel::BlockCollisionShape::boxes({{
            .min = {0.0f, collisionBottom, 0.0f},
            .max = {1.0f, 1.0f, 1.0f},
        }}, alternatePresentation
            ? Voxel::BlockCollisionShape::Provenance::Exact
            : Voxel::BlockCollisionShape::Provenance::Authored);
    }
    return type;
}

struct ManifestFixture {
    Voxel::BlockRegistry registry;
    std::shared_ptr<Voxel::WorldGenerator> generator;
};

ManifestFixture fixture(
    bool reverse,
    uint32_t seed = 91,
    float modelBottom = 0.25f,
    float collisionBottom = 0.25f,
    bool alternatePresentation = false,
    bool omitFace = false,
    Voxel::BlockModelOrientation orientation =
        Voxel::BlockModelOrientation::Identity
) {
    ManifestFixture result;
    if (reverse) {
        result.registry.registerBlock(
            "rigel:raised",
            block("rigel:raised", true, modelBottom, collisionBottom,
                  alternatePresentation, omitFace, orientation));
        result.registry.registerBlock("rigel:stone", block("rigel:stone", false));
    } else {
        result.registry.registerBlock("rigel:stone", block("rigel:stone", false));
        result.registry.registerBlock(
            "rigel:raised",
            block("rigel:raised", true, modelBottom, collisionBottom,
                  alternatePresentation, omitFace, orientation));
    }
    result.registry.freeze();
    auto definition = Test::generatorDefinitionFixture(
        "rigel:stone", "rigel:raised", "base:air");
    result.generator = std::make_shared<Voxel::WorldGenerator>(
        result.registry, std::move(definition), seed);
    return result;
}

} // namespace

TEST_CASE(ContentManifest_is_stable_across_compact_registration_order) {
    auto first = fixture(false);
    auto reordered = fixture(true);
    Simulation::ContentDictionary firstDictionary(
        first.registry, *first.generator);
    Simulation::ContentDictionary reorderedDictionary(
        reordered.registry, *reordered.generator);

    CHECK_EQ(firstDictionary.identity(), reorderedDictionary.identity());
    CHECK_EQ(firstDictionary.identity().hex().size(), static_cast<size_t>(64));
    CHECK_NE(
        firstDictionary.localId("rigel:stone").type,
        reorderedDictionary.localId("rigel:stone").type);

    const Voxel::BlockState local{
        firstDictionary.localId("rigel:raised"), 7, 0x31};
    const auto semantic = firstDictionary.semanticState(local);
    CHECK_EQ(semantic.blockKey, std::string("rigel:raised"));
    CHECK_EQ(reorderedDictionary.localState(semantic).metadata, uint8_t{7});
    CHECK_EQ(
        reorderedDictionary.semanticState(
            reorderedDictionary.localState(semantic, local.lightLevel)),
        semantic);
    CHECK_EQ(
        reorderedDictionary.localState(semantic, local.lightLevel).lightLevel,
        uint8_t{0x31});
    CHECK(reorderedDictionary.supportsState(semantic));
    CHECK(reorderedDictionary.supportsState({"base:air", 0}));
    CHECK(!reorderedDictionary.supportsState({"base:air", 1}));
    CHECK(reorderedDictionary.supportsPublishedState({"base:air", 0}, 0));
    CHECK(!reorderedDictionary.supportsPublishedState({"base:air", 0}, 1));
}

TEST_CASE(ContentManifest_rejects_mismatch_and_unfrozen_content) {
    auto first = fixture(false, 91);
    auto changed = fixture(false, 92);
    Simulation::ContentDictionary firstDictionary(
        first.registry, *first.generator);
    Simulation::ContentDictionary changedDictionary(
        changed.registry, *changed.generator);

    CHECK_NE(firstDictionary.identity(), changedDictionary.identity());
    CHECK_THROWS(firstDictionary.requireIdentity(changedDictionary.identity()));
    CHECK_THROWS(firstDictionary.localId("rigel:missing"));

    Voxel::BlockRegistry mutableRegistry;
    auto definition = Test::generatorDefinitionFixture(
        "base:air", "base:air", "base:air");
    Voxel::WorldGenerator generator(mutableRegistry, definition, 1);
    CHECK_THROWS(Simulation::ContentDictionary(mutableRegistry, generator));
}

TEST_CASE(ContentManifest_excludes_presentation_but_preserves_authority_geometry) {
    auto original = fixture(false);
    auto changedPresentation = fixture(false, 91, 0.25f, 0.25f, true);
    auto changedModel = fixture(false, 91, 0.125f, 0.25f);
    auto changedFaces = fixture(false, 91, 0.25f, 0.25f, false, true);
    auto changedOrientation = fixture(
        false, 91, 0.25f, 0.25f, false, false,
        Voxel::BlockModelOrientation::RotateY90);
    auto changedCollision = fixture(false, 91, 0.25f, 0.125f);
    Simulation::ContentDictionary originalDictionary(
        original.registry, *original.generator);
    Simulation::ContentDictionary presentationDictionary(
        changedPresentation.registry, *changedPresentation.generator);
    Simulation::ContentDictionary modelDictionary(
        changedModel.registry, *changedModel.generator);
    Simulation::ContentDictionary collisionDictionary(
        changedCollision.registry, *changedCollision.generator);
    Simulation::ContentDictionary faceDictionary(
        changedFaces.registry, *changedFaces.generator);
    Simulation::ContentDictionary orientationDictionary(
        changedOrientation.registry, *changedOrientation.generator);

    CHECK_EQ(originalDictionary.identity(), presentationDictionary.identity());
    CHECK_NE(originalDictionary.identity(), modelDictionary.identity());
    CHECK_NE(originalDictionary.identity(), faceDictionary.identity());
    CHECK_NE(originalDictionary.identity(), orientationDictionary.identity());
    CHECK_NE(originalDictionary.identity(), collisionDictionary.identity());
    CHECK_NE(modelDictionary.identity(), collisionDictionary.identity());
}

TEST_CASE(ContentManifest_rejects_unrepresented_gallery_generation) {
    auto source = fixture(false);
    const Voxel::BlockGalleryCatalog catalog(source.registry);
    auto gallery = std::make_shared<const Voxel::BlockGalleryChunkGenerator>(
        source.registry, catalog);
    const auto identity = Voxel::prepareBlockGalleryGeneratorIdentity(
        source.registry, gallery->worldBounds());
    Voxel::WorldGenerator ordinary(source.registry, identity.data, 0);
    Voxel::WorldGenerator galleryGenerator(
        source.registry, identity.data, 0,
        Voxel::kGeneratorSemanticsVersion, gallery);

    CHECK(ordinary.matchesGenerationInputs(
        galleryGenerator.definition(), galleryGenerator.seed(),
        galleryGenerator.semanticsVersion()));
    CHECK(!ordinary.matchesRuntimeGenerator(galleryGenerator));
    CHECK_NO_THROW(Simulation::ContentDictionary(source.registry, ordinary));
    CHECK_THROWS(
        Simulation::ContentDictionary(source.registry, galleryGenerator));
}
