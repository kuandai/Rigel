#include "TestFramework.h"

#include "Rigel/Voxel/BlockLoader.h"
#include "Rigel/Asset/AssetManager.h"
#include "Rigel/Asset/AssetIR.h"

using namespace Rigel::Voxel;
using namespace Rigel::Asset;

TEST_CASE(BlockLoader_LoadsManifestBlocks) {
    AssetManager assets;
    assets.loadManifest("manifest.yaml");

    BlockRegistry registry;
    TextureAtlas atlas;
    BlockLoader loader;

    size_t loaded = loader.loadFromManifest(assets, registry, atlas);
    if (loaded == 0) {
        SKIP_TEST("No block definitions loaded");
    }

    CHECK(registry.hasIdentifier("base:dirt"));
}

TEST_CASE(BlockLoader_LoadFromAssetGraph_DeterministicIdsAcrossInputOrder) {
    auto makeGraph = [](bool reverse) {
        Rigel::Asset::IR::AssetGraphIR graph;

        Rigel::Asset::IR::BlockDefIR firstBlock;
        firstBlock.rootIdentifier = "rigel:alpha";
        firstBlock.sourcePath = "blocks/alpha.yaml";
        Rigel::Asset::IR::BlockStateIR firstState;
        firstState.identifier = "rigel:alpha";
        firstState.rootIdentifier = "rigel:alpha";
        firstState.sourcePath = firstBlock.sourcePath;
        firstState.model = "cube";
        firstState.renderLayer = "opaque";
        firstState.isOpaque = true;
        firstState.isSolid = true;
        firstState.textures["all"] = "textures/blocks/alpha.png";
        firstBlock.states.push_back(firstState);

        Rigel::Asset::IR::BlockDefIR secondBlock;
        secondBlock.rootIdentifier = "rigel:beta";
        secondBlock.sourcePath = "blocks/beta.yaml";
        Rigel::Asset::IR::BlockStateIR secondState;
        secondState.identifier = "rigel:beta";
        secondState.rootIdentifier = "rigel:beta";
        secondState.sourcePath = secondBlock.sourcePath;
        secondState.model = "cube";
        secondState.renderLayer = "opaque";
        secondState.isOpaque = true;
        secondState.isSolid = true;
        secondState.textures["all"] = "textures/blocks/beta.png";
        secondBlock.states.push_back(secondState);

        if (reverse) {
            graph.blocks.push_back(secondBlock);
            graph.blocks.push_back(firstBlock);
        } else {
            graph.blocks.push_back(firstBlock);
            graph.blocks.push_back(secondBlock);
        }

        return graph;
    };

    const Rigel::Asset::IR::AssetGraphIR ordered = makeGraph(false);
    const Rigel::Asset::IR::AssetGraphIR reversed = makeGraph(true);

    BlockRegistry registryOrdered;
    BlockRegistry registryReversed;
    TextureAtlas atlasOrdered;
    TextureAtlas atlasReversed;
    BlockLoader loader;

    const size_t loadedOrdered = loader.loadFromAssetGraph(ordered, registryOrdered, atlasOrdered);
    const size_t loadedReversed = loader.loadFromAssetGraph(reversed, registryReversed, atlasReversed);
    CHECK_EQ(loadedOrdered, static_cast<size_t>(2));
    CHECK_EQ(loadedReversed, static_cast<size_t>(2));

    const auto alphaOrdered = registryOrdered.findByIdentifier("rigel:alpha");
    const auto alphaReversed = registryReversed.findByIdentifier("rigel:alpha");
    const auto betaOrdered = registryOrdered.findByIdentifier("rigel:beta");
    const auto betaReversed = registryReversed.findByIdentifier("rigel:beta");
    CHECK(alphaOrdered.has_value());
    CHECK(alphaReversed.has_value());
    CHECK(betaOrdered.has_value());
    CHECK(betaReversed.has_value());
    CHECK_EQ(alphaOrdered->type, alphaReversed->type);
    CHECK_EQ(betaOrdered->type, betaReversed->type);
    CHECK_EQ(registryOrdered.snapshotHash(), registryReversed.snapshotHash());
}
