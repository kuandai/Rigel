#include "TestFramework.h"
#include "GeneratorDefinitionTestRegistry.h"

#include "Rigel/Simulation/ContentManifest.h"
#include "Rigel/Voxel/BlockRegistry.h"
#include "Rigel/Voxel/WorldGenerator.h"

#include <memory>
#include <string>
#include <vector>

namespace {

using namespace Rigel;

Voxel::BlockType block(std::string identifier, bool raised) {
    Voxel::BlockType type;
    type.identifier = std::move(identifier);
    type.isOpaque = !raised;
    type.emittedLight = raised ? 3 : 0;
    if (raised) {
        type.model = std::make_shared<const Voxel::BlockModel>(
            "fixture:raised",
            std::vector<std::string>{"surface"},
            std::vector<Voxel::BlockModelCuboid>{Voxel::BlockModelCuboid{
                .bounds = {{0.0f, 0.25f, 0.0f}, {1.0f, 1.0f, 1.0f}},
            }});
        type.collision = Voxel::BlockCollisionShape::boxes({{
            .min = {0.0f, 0.25f, 0.0f},
            .max = {1.0f, 1.0f, 1.0f},
        }});
    }
    return type;
}

struct ManifestFixture {
    Voxel::BlockRegistry registry;
    std::shared_ptr<Voxel::WorldGenerator> generator;
};

ManifestFixture fixture(bool reverse, uint32_t seed = 91) {
    ManifestFixture result;
    if (reverse) {
        result.registry.registerBlock("rigel:raised", block("rigel:raised", true));
        result.registry.registerBlock("rigel:stone", block("rigel:stone", false));
    } else {
        result.registry.registerBlock("rigel:stone", block("rigel:stone", false));
        result.registry.registerBlock("rigel:raised", block("rigel:raised", true));
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
            reorderedDictionary.localState(semantic)),
        semantic);
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
