#include "TestFramework.h"
#include "GeneratorDefinitionTestRegistry.h"

#include "Rigel/Simulation/ContentManifest.h"
#include "Rigel/Voxel/BlockGalleryCatalog.h"
#include "Rigel/Voxel/BlockGalleryChunkGenerator.h"
#include "Rigel/Voxel/BlockRegistry.h"
#include "Rigel/Voxel/BlockTargeting.h"
#include "Rigel/Voxel/World.h"
#include "Rigel/Voxel/WorldGenerator.h"
#include "Rigel/Voxel/WorldResources.h"

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

enum class InertPlacement { None, Before, After };

struct InertGeometryFixture {
    Voxel::WorldResources resources;
    Voxel::World world{resources};
    std::shared_ptr<Voxel::WorldGenerator> generator;
    std::unique_ptr<Simulation::ContentDictionary> dictionary;
    Voxel::BlockID raised;

    InertGeometryFixture(
        InertPlacement placement,
        Voxel::BlockModelBounds inertBounds,
        bool giveInertCuboidFace = false
    ) {
        resources.registry().registerBlock(
            "rigel:stone", block("rigel:stone", false));
        auto raisedType = block("rigel:raised", true);
        std::vector<Voxel::BlockModelCuboid> cuboids(
            raisedType.model->cuboids().begin(),
            raisedType.model->cuboids().end());
        if (placement != InertPlacement::None) {
            Voxel::BlockModelCuboid inert{.bounds = inertBounds};
            if (giveInertCuboidFace) {
                inert.faces[static_cast<size_t>(Voxel::Direction::PosY)] =
                    Voxel::BlockModelFace{.textureSlot = "surface"};
            }
            cuboids.insert(
                placement == InertPlacement::Before
                    ? cuboids.begin() : cuboids.end(),
                std::move(inert));
            raisedType.model = std::make_shared<const Voxel::BlockModel>(
                "fixture:raised", std::vector<std::string>{"surface"},
                std::move(cuboids));
        }
        raised = resources.registry().registerBlock(
            "rigel:raised", std::move(raisedType));
        resources.registry().freeze();
        generator = std::make_shared<Voxel::WorldGenerator>(
            resources.registry(),
            Test::generatorDefinitionFixture(
                "rigel:stone", "rigel:raised", "base:air"),
            91);
        dictionary = std::make_unique<Simulation::ContentDictionary>(
            resources.registry(), *generator);
        world.setBlock(0, 0, 0, Voxel::BlockState{raised});
    }

    std::optional<Voxel::BlockTarget> target() const {
        return Voxel::raycastBlock(
            world, glm::vec3{0.5f, 2.0f, 0.5f},
            glm::vec3{0.0f, -1.0f, 0.0f}, 4.0f);
    }
};

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
    auto equivalentOrientation = fixture(
        false, 91, 0.25f, 0.25f, false, false,
        Voxel::BlockModelOrientation::RotateY90);
    auto changedOrientation = fixture(
        false, 91, 0.25f, 0.25f, false, false,
        Voxel::BlockModelOrientation::RotateX90);
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
    Simulation::ContentDictionary equivalentOrientationDictionary(
        equivalentOrientation.registry, *equivalentOrientation.generator);

    CHECK_EQ(originalDictionary.identity(), presentationDictionary.identity());
    CHECK_EQ(
        originalDictionary.identity(),
        equivalentOrientationDictionary.identity());
    CHECK_NE(originalDictionary.identity(), modelDictionary.identity());
    CHECK_NE(originalDictionary.identity(), faceDictionary.identity());
    CHECK_NE(originalDictionary.identity(), orientationDictionary.identity());
    CHECK_NE(originalDictionary.identity(), collisionDictionary.identity());
    CHECK_NE(modelDictionary.identity(), collisionDictionary.identity());
}

TEST_CASE(ContentManifest_excludes_faceless_cuboids_from_selection_identity) {
    InertGeometryFixture original(
        InertPlacement::None, {{0, 0, 0}, {1, 1, 1}});
    InertGeometryFixture inertBefore(
        InertPlacement::Before, {{-32, -4, -16}, {48, 12, 24}});
    InertGeometryFixture inertAfter(
        InertPlacement::After, {{-128, -8, -64}, {96, 20, 80}});
    InertGeometryFixture selectableOverhang(
        InertPlacement::Before, {{-2, 0, -2}, {3, 2, 3}}, true);

    CHECK_EQ(original.dictionary->identity(), inertBefore.dictionary->identity());
    CHECK_EQ(original.dictionary->identity(), inertAfter.dictionary->identity());
    CHECK_NE(
        original.dictionary->identity(),
        selectableOverhang.dictionary->identity());
    CHECK_EQ(
        original.resources.registry().modelExtents(),
        inertBefore.resources.registry().modelExtents());
    CHECK_EQ(
        original.resources.registry().modelExtents(),
        inertAfter.resources.registry().modelExtents());

    const auto expected = original.target();
    CHECK(expected.has_value());
    for (const auto* equivalent : {&inertBefore, &inertAfter}) {
        const auto actual = equivalent->target();
        CHECK(actual.has_value());
        CHECK_EQ(actual->block, expected->block);
        CHECK_EQ(actual->face, expected->face);
        CHECK_EQ(actual->normal, expected->normal);
        CHECK_NEAR(actual->distance, expected->distance, 0.00001f);
        CHECK_EQ(actual->position, expected->position);
    }
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
