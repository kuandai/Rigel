#include "TestFramework.h"
#include "LogCapture.h"

#include "Rigel/Asset/AssetLoader.h"
#include "Rigel/Voxel/BlockType.h"
#include "Rigel/Voxel/WorldSet.h"

#include <memory>
#include <vector>

using namespace Rigel::Voxel;

namespace {

class EmptyShaderLoader final : public Rigel::Asset::IAssetLoader {
public:
    std::string_view category() const override { return "shaders"; }

    std::shared_ptr<Rigel::Asset::AssetBase> load(
        const Rigel::Asset::LoadContext&) override {
        return std::make_shared<Rigel::Asset::ShaderAsset>();
    }
};

class ThrowOnceShaderLoader final : public Rigel::Asset::IAssetLoader {
public:
    std::string_view category() const override { return "shaders"; }

    std::shared_ptr<Rigel::Asset::AssetBase> load(
        const Rigel::Asset::LoadContext& ctx) override {
        if (ctx.id == "shaders/voxel") {
            ++voxelLoadCount;
            if (!hasThrown) {
                hasThrown = true;
                throw std::runtime_error("injected voxel shader failure");
            }
        }
        return std::make_shared<Rigel::Asset::ShaderAsset>();
    }

    int voxelLoadCount = 0;
    bool hasThrown = false;
};

class OptionalShadowFailureLoader final : public Rigel::Asset::IAssetLoader {
public:
    std::string_view category() const override { return "shaders"; }

    std::shared_ptr<Rigel::Asset::AssetBase> load(
        const Rigel::Asset::LoadContext& ctx) override {
        if (ctx.id == "shaders/voxel_shadow_depth" ||
            ctx.id == "shaders/voxel_shadow_transmit") {
            ++failureCount;
            throw std::runtime_error(
                "injected optional shadow failure for " + ctx.id);
        }
        return std::make_shared<Rigel::Asset::ShaderAsset>();
    }

    size_t failureCount = 0;
};

} // namespace

template<typename T>
concept HasPublicWorldRemoval = requires(T& worlds, WorldId id) {
    worlds.removeWorld(id);
};

static_assert(!HasPublicWorldRemoval<WorldSet>);

TEST_CASE(WorldSet_DefaultWorldUsesSharedRegistry) {
    WorldSet worldSet;
    World& world = worldSet.createWorld(WorldSet::defaultWorldId());

    CHECK_EQ(world.id(), WorldSet::defaultWorldId());
    CHECK_EQ(&world.blockRegistry(), &worldSet.resources().registry());
}

TEST_CASE(WorldSet_MultipleWorldsHaveIndependentChunks) {
    WorldSet worldSet;

    BlockType solid;
    solid.identifier = "rigel:stone";
    worldSet.resources().registry().registerBlock(solid.identifier, solid);
    auto solidId = worldSet.resources().registry().findByIdentifier(solid.identifier);
    CHECK(solidId.has_value());

    World& first = worldSet.createWorld(1);
    World& second = worldSet.createWorld(2);

    BlockState state;
    state.id = *solidId;
    first.setBlock(0, 0, 0, state);

    CHECK_EQ(first.getBlock(0, 0, 0).id, state.id);
    CHECK(second.getBlock(0, 0, 0).isAir());
}

TEST_CASE(WorldSet_CreateViewPublishesOnlyAfterInitialization) {
    Rigel::Asset::AssetManager assets;
    auto loader = std::make_unique<ThrowOnceShaderLoader>();
    auto* loaderProbe = loader.get();
    assets.registerLoader("shaders", std::move(loader));
    assets.loadManifest("manifest.yaml");

    WorldSet worldSet;
    BlockType solid;
    solid.identifier = "rigel:create_view_sentinel";
    const BlockID solidId =
        worldSet.resources().registry().registerBlock(solid.identifier, solid);
    World& originalWorld = worldSet.createWorld(9);
    originalWorld.setBlock(0, 0, 0, BlockState{solidId});

    std::string diagnostic;
    try {
        worldSet.createView(9, assets);
    } catch (const std::runtime_error& error) {
        diagnostic = error.what();
    }
    CHECK_EQ(diagnostic, "injected voxel shader failure");
    CHECK(worldSet.findView(9) == nullptr);
    CHECK_EQ(&worldSet.world(9), &originalWorld);
    CHECK_EQ(worldSet.world(9).getBlock(0, 0, 0).id, solidId);
    CHECK_EQ(loaderProbe->voxelLoadCount, 1);

    WorldView& initialized = worldSet.createView(9, assets);
    CHECK_EQ(worldSet.findView(9), &initialized);
    CHECK_EQ(&initialized.world(), &originalWorld);
    CHECK_EQ(loaderProbe->voxelLoadCount, 2);

    WorldView& duplicate = worldSet.createView(9, assets);
    CHECK_EQ(&duplicate, &initialized);
    CHECK_EQ(loaderProbe->voxelLoadCount, 2);

    initialized.clear();
    worldSet.clear();
    CHECK(!worldSet.hasWorld(9));
}

TEST_CASE(WorldSet_OptionalShadowFailuresPublishDegradedView) {
    Rigel::Test::LogCapture logs("voxel-shadow-failure-test");
    Rigel::Asset::AssetManager assets;
    auto loader = std::make_unique<OptionalShadowFailureLoader>();
    auto* loaderProbe = loader.get();
    assets.registerLoader("shaders", std::move(loader));
    assets.loadManifest("manifest.yaml");

    WorldSet worldSet;
    World& world = worldSet.createWorld(10);
    WorldView* view = nullptr;
    CHECK_NO_THROW(view = &worldSet.createView(10, assets));
    CHECK(view != nullptr);
    CHECK_EQ(worldSet.findView(10), view);
    CHECK_EQ(&view->world(), &world);
    CHECK_EQ(loaderProbe->failureCount, static_cast<size_t>(2));
    const auto output = logs.output();
    CHECK_EQ(Rigel::Test::countOccurrences(
                 output,
                 "Optional startup resource 'shaders/voxel_shadow_depth'"),
             static_cast<size_t>(1));
    CHECK_EQ(Rigel::Test::countOccurrences(
                 output,
                 "Optional startup resource 'shaders/voxel_shadow_transmit'"),
             static_cast<size_t>(1));

    view->clear();
    worldSet.clear();
}

TEST_CASE(WorldSet_ClearDestroysAllWorldsAndCanRepeatDuringTeardown) {
    Rigel::Asset::AssetManager assets;
    assets.registerLoader("shaders", std::make_unique<EmptyShaderLoader>());
    assets.loadManifest("manifest.yaml");

    WorldSet worldSet;
    BlockType solid;
    solid.identifier = "rigel:world_set_clear_solid";
    const BlockID solidId =
        worldSet.resources().registry().registerBlock(solid.identifier, solid);

    World& world = worldSet.createWorld(1);
    WorldGenConfig generation;
    generation.solidBlock = solid.identifier;
    generation.surfaceBlock = solid.identifier;
    auto generator = std::make_shared<WorldGenerator>(
        worldSet.resources().registry(), generation);
    world.setGenerator(generator);

    WorldView& view = worldSet.createView(1, assets);
    StreamingConfig streaming;
    streaming.viewDistanceChunks = 0;
    streaming.unloadDistanceChunks = 0;
    streaming.genQueueLimit = 0;
    streaming.meshQueueLimit = 0;
    streaming.updateBudgetPerFrame = 0;
    streaming.applyBudgetPerFrame = 0;
    streaming.workerThreads = 0;
    streaming.maxResidentChunks = 0;
    view.setStreamConfig(streaming);

    const ChunkCoord coord{0, 0, 0};
    Chunk& chunk = world.chunkManager().getOrCreateChunk(coord);
    chunk.setBlock(0, 0, 0, BlockState{solidId}, worldSet.resources().registry());
    chunk.setWorldGenVersion(generator->config().world.version);
    chunk.setLoadedFromDisk(true);
    view.updateStreaming(coord.toWorldCenter());
    view.updateMeshes();

    worldSet.createWorld(2).setBlock(ChunkSize, 0, 0, BlockState{});

    CHECK(worldSet.hasWorld(1));
    CHECK_EQ(worldSet.findView(1), &view);
    CHECK(view.meshStore().contains(coord));
    std::vector<ChunkStreamer::DebugChunkState> states;
    view.getChunkDebugStates(states, coord, 0);
    CHECK_EQ(states.size(), static_cast<size_t>(1));
    CHECK_EQ(states.front().coord, coord);
    CHECK_EQ(states.front().state,
             ChunkStreamer::DebugState::AcceptedNonemptyGeometry);

    view.clear();
    CHECK(world.chunkManager().hasChunk(coord));
    CHECK(!view.meshStore().contains(coord));
    view.getChunkDebugStates(states, coord, 0);
    CHECK(states.empty());
    CHECK_EQ(worldSet.findView(1), &view);

    worldSet.clear();

    CHECK(!worldSet.hasWorld(1));
    CHECK(!worldSet.hasWorld(2));
    CHECK(worldSet.findView(1) == nullptr);
    CHECK(worldSet.findView(2) == nullptr);
    CHECK_NO_THROW(worldSet.clear());
}
