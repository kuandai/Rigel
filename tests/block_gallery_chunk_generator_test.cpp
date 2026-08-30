#include "TestFramework.h"

#include "Rigel/Persistence/AsyncChunkLoader.h"
#include "Rigel/Persistence/Backends/Memory/MemoryFormat.h"
#include "Rigel/Persistence/InMemoryStorage.h"
#include "Rigel/Persistence/PersistenceService.h"
#include "Rigel/Persistence/WorldSettings.h"
#include "Rigel/Voxel/BlockGalleryCatalog.h"
#include "Rigel/Voxel/BlockGalleryChunkGenerator.h"
#include "Rigel/Voxel/BlockRegistry.h"
#include "Rigel/Voxel/MeshBuilder.h"
#include "Rigel/Voxel/StreamingConfig.h"
#include "Rigel/Voxel/World.h"
#include "Rigel/Voxel/WorldResources.h"
#include "Rigel/Voxel/WorldView.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {
using namespace Rigel;
using namespace Rigel::Voxel;

std::shared_ptr<const BlockModel> partialModel(bool cullBoundary) {
    BlockModelCuboid cuboid;
    cuboid.bounds = {{0.0f, 0.0f, 0.0f}, {1.0f, 0.5f, 1.0f}};
    for (size_t index = 0; index < DirectionCount; ++index) {
        cuboid.faces[index] = BlockModelFace{
            .textureSlot = "surface",
            .ambientOcclusion = true,
            .cullAgainstOpaqueNeighbor = cullBoundary,
        };
    }
    return std::make_shared<const BlockModel>(
        cullBoundary ? "invented:coverage_model" : "invented:partial_model",
        std::vector<std::string>{"surface"},
        std::vector<BlockModelCuboid>{cuboid});
}

std::shared_ptr<const BlockModel> mismatchedXBoundaryModel(
    std::string identifier, bool cullBoundary
) {
    BlockModelCuboid positive;
    positive.bounds = {{0.0f, 0.5f, 0.0f}, {1.0f, 1.0f, 1.0f}};
    positive.faces[static_cast<size_t>(Direction::PosX)] = BlockModelFace{
        .textureSlot = "surface",
        .cullAgainstOpaqueNeighbor = cullBoundary,
    };
    BlockModelCuboid negative;
    negative.bounds = {{0.0f, 0.0f, 0.0f}, {1.0f, 0.5f, 1.0f}};
    negative.faces[static_cast<size_t>(Direction::NegX)] = BlockModelFace{
        .textureSlot = "surface",
        .cullAgainstOpaqueNeighbor = cullBoundary,
    };
    return std::make_shared<const BlockModel>(
        std::move(identifier),
        std::vector<std::string>{"surface"},
        std::vector<BlockModelCuboid>{
            std::move(positive), std::move(negative)});
}

void populateGalleryRegistry(BlockRegistry& registry, size_t numberedCount) {
    BlockType mismatchedCoverage;
    mismatchedCoverage.identifier = "invented:aaa_coverage_mismatch";
    mismatchedCoverage.model = mismatchedXBoundaryModel(
        "invented:aaa_coverage_mismatch_model", true);
    mismatchedCoverage.isOpaque = true;
    const std::string mismatchedCoverageId =
        mismatchedCoverage.identifier;
    registry.registerBlock(
        mismatchedCoverageId, std::move(mismatchedCoverage));

    BlockType mismatchedSameType;
    mismatchedSameType.identifier = "invented:aab_joined_mismatch";
    mismatchedSameType.model = mismatchedXBoundaryModel(
        "invented:aab_joined_mismatch_model", false);
    mismatchedSameType.cullSameType = true;
    const std::string mismatchedSameTypeId =
        mismatchedSameType.identifier;
    registry.registerBlock(
        mismatchedSameTypeId, std::move(mismatchedSameType));

    BlockType floor;
    floor.identifier = "invented:floor";
    floor.model = BlockModel::fullCube();
    floor.isOpaque = true;
    floor.layer = RenderLayer::Opaque;
    registry.registerBlock("invented:floor", std::move(floor));

    BlockType coverage;
    coverage.identifier = "invented:coverage";
    coverage.model = partialModel(true);
    coverage.isOpaque = true;
    registry.registerBlock("invented:coverage", std::move(coverage));

    BlockType sameType;
    sameType.identifier = "invented:joined";
    sameType.model = partialModel(false);
    sameType.isOpaque = false;
    sameType.cullSameType = true;
    registry.registerBlock("invented:joined", std::move(sameType));

    const auto model = partialModel(false);
    for (size_t index = 0; index < numberedCount; ++index) {
        BlockType specimen;
        specimen.identifier =
            "invented:specimen[state=" + std::to_string(index) + "]";
        const std::string identifier = specimen.identifier;
        specimen.model = model;
        specimen.isOpaque = false;
        registry.registerBlock(identifier, std::move(specimen));
    }
}

std::vector<BlockGalleryBlockPlacement> diagnosticPair(
    const BlockGalleryChunkGenerator& generator,
    BlockGalleryPlacementKind kind
) {
    const std::vector<BlockGalleryBlockPlacement> placements =
        generator.placements();
    std::vector<BlockGalleryBlockPlacement> pair;
    std::copy_if(
        placements.begin(),
        placements.end(),
        std::back_inserter(pair),
        [kind](const BlockGalleryBlockPlacement& placement) {
            return placement.kind == kind;
        });
    return pair;
}

size_t countSharedXPlaneFaces(
    const ChunkMesh& mesh,
    ChunkCoord chunk,
    const std::vector<BlockGalleryBlockPlacement>& pair
) {
    CHECK_EQ(pair.size(), static_cast<size_t>(2));
    const float planeX = static_cast<float>(
        pair[1].position.x - chunk.x * Chunk::SIZE);
    const float minY = static_cast<float>(
        pair[0].position.y - chunk.y * Chunk::SIZE);
    const float minZ = static_cast<float>(
        pair[0].position.z - chunk.z * Chunk::SIZE);
    CHECK_EQ(mesh.vertices.size() % 4, static_cast<size_t>(0));
    size_t count = 0;
    for (size_t first = 0; first < mesh.vertices.size(); first += 4) {
        bool matches = true;
        for (size_t vertex = 0; vertex < 4; ++vertex) {
            const VoxelVertex& candidate = mesh.vertices[first + vertex];
            if (candidate.x != planeX ||
                candidate.y < minY || candidate.y > minY + 1.0f ||
                candidate.z < minZ || candidate.z > minZ + 1.0f) {
                matches = false;
                break;
            }
        }
        count += matches ? 1 : 0;
    }
    return count;
}

ChunkMesh buildGeneratedDiagnosticChunk(
    const BlockGalleryChunkGenerator& generator,
    const BlockRegistry& registry,
    const std::vector<BlockGalleryBlockPlacement>& pair
) {
    const ChunkCoord chunkCoord = worldToChunk(
        pair.front().position.x,
        pair.front().position.y,
        pair.front().position.z);
    CHECK_EQ(
        worldToChunk(
            pair.back().position.x,
            pair.back().position.y,
            pair.back().position.z),
        chunkCoord);
    ChunkBuffer buffer;
    generator.generate(chunkCoord, buffer);
    Chunk chunk(chunkCoord);
    chunk.copyFrom(buffer.blocks, registry);
    return MeshBuilder{}.build({
        .chunk = chunk,
        .registry = registry,
        .atlas = nullptr,
        .neighbors = {},
    });
}

BlockState generatedBlock(
    const BlockGalleryChunkGenerator& generator,
    BlockGalleryWorldPosition position) {
    ChunkBuffer buffer;
    const ChunkCoord coord = worldToChunk(position.x, position.y, position.z);
    generator.generate(coord, buffer);
    int x = 0;
    int y = 0;
    int z = 0;
    worldToLocal(position.x, position.y, position.z, x, y, z);
    return buffer.at(x, y, z);
}

void pumpStreaming(WorldView& view, const glm::vec3& camera, size_t count = 8) {
    for (size_t index = 0; index < count; ++index) {
        view.updateStreaming(camera);
        view.updateMeshes();
    }
}

} // namespace

TEST_CASE(BlockGalleryChunkGenerator_PlacesCatalogFloorAndDiagnostics) {
    BlockRegistry registry;
    populateGalleryRegistry(registry, 100);
    registry.freeze();
    auto catalog = std::make_shared<const BlockGalleryCatalog>(registry);
    const BlockGalleryChunkGenerator generator(registry, catalog);
    const BlockGalleryChunkGenerator repeated(registry, catalog);
    const std::vector<BlockGalleryBlockPlacement> placements =
        generator.placements();

    CHECK_EQ(placements, repeated.placements());
    CHECK_EQ(generator.overview(), repeated.overview());
    CHECK(generator.overview().cameraDistance > 0.0f);
    CHECK(generator.overview().cameraHeight > 0.0f);
    CHECK(generator.overview().cameraDistance <= 128.0f);
    CHECK(generator.overview().cameraHeight <= 48.0f);
    const ChunkCoord overviewChunk = worldToChunk(
        static_cast<int>(std::floor(
            generator.overview().centerX +
            generator.overview().cameraDistance)),
        static_cast<int>(std::floor(generator.overview().cameraHeight)),
        static_cast<int>(std::floor(
            generator.overview().centerZ +
            generator.overview().cameraDistance)));
    CHECK(std::any_of(
        placements.begin(),
        placements.end(),
        [&](const BlockGalleryBlockPlacement& placement) {
            const ChunkCoord placementChunk = worldToChunk(
                placement.position.x,
                placement.position.y,
                placement.position.z);
            const int dx = placementChunk.x - overviewChunk.x;
            const int dy = placementChunk.y - overviewChunk.y;
            const int dz = placementChunk.z - overviewChunk.z;
            return dx * dx + dy * dy + dz * dz <= 4;
        }));

    const auto referenceFloor = std::find_if(
        placements.begin(), placements.end(),
        [](const BlockGalleryBlockPlacement& placement) {
            return placement.position == BlockGalleryWorldPosition{-1, 0, -1} &&
                placement.kind == BlockGalleryPlacementKind::ReferenceFloor;
        });
    CHECK(referenceFloor != placements.end());
    CHECK_EQ(
        registry.getType(referenceFloor->blockId).identifier,
        std::string("invented:floor"));
    int floorMinX = 0;
    int floorMinZ = 0;
    for (const BlockGalleryBlockPlacement& placement : placements) {
        if (placement.kind != BlockGalleryPlacementKind::ReferenceFloor) {
            continue;
        }
        floorMinX = std::min(floorMinX, placement.position.x);
        floorMinZ = std::min(floorMinZ, placement.position.z);
    }
    CHECK_EQ(floorMinX, -1);
    CHECK_EQ(floorMinZ, -1);

    for (const BlockGalleryCatalogEntry& entry : catalog->entries()) {
        const BlockState state = generatedBlock(
            generator, entry.specimenPosition);
        CHECK_EQ(state.id, entry.blockId);
        CHECK_EQ(state.skyLight(), static_cast<uint8_t>(15));
    }
    CHECK_EQ(
        generatedBlock(generator, {-1, 0, -1}).id,
        referenceFloor->blockId);

    const int galleryLastZ = static_cast<int>(
        catalog->gridDimensions().rows - 1) *
        BlockGalleryCatalog::SpecimenSpacing;
    size_t opaqueDiagnostics = 0;
    size_t sameTypeDiagnostics = 0;
    size_t coverageDiagnostics = 0;
    for (const BlockGalleryBlockPlacement& placement : placements) {
        opaqueDiagnostics += placement.kind ==
            BlockGalleryPlacementKind::OpaqueCullingDiagnostic;
        sameTypeDiagnostics += placement.kind ==
            BlockGalleryPlacementKind::SameTypeCullingDiagnostic;
        coverageDiagnostics += placement.kind ==
            BlockGalleryPlacementKind::CoverageCullingDiagnostic;
    }
    CHECK_EQ(opaqueDiagnostics, static_cast<size_t>(2));
    CHECK_EQ(sameTypeDiagnostics, static_cast<size_t>(2));
    CHECK_EQ(coverageDiagnostics, static_cast<size_t>(2));

    const auto opaquePair = diagnosticPair(
        generator, BlockGalleryPlacementKind::OpaqueCullingDiagnostic);
    CHECK_EQ(opaquePair.size(), static_cast<size_t>(2));
    const int diagnosticZ = opaquePair.front().position.z;
    CHECK(diagnosticZ > galleryLastZ);

    for (const BlockGalleryPlacementKind kind : {
             BlockGalleryPlacementKind::OpaqueCullingDiagnostic,
             BlockGalleryPlacementKind::SameTypeCullingDiagnostic,
             BlockGalleryPlacementKind::CoverageCullingDiagnostic}) {
        const std::vector<BlockGalleryBlockPlacement> pair = diagnosticPair(
            generator, kind);
        CHECK_EQ(pair.size(), static_cast<size_t>(2));
        CHECK_EQ(pair[0].position.z, diagnosticZ);
        CHECK_EQ(pair[1].position.z, diagnosticZ);
        CHECK_EQ(pair[0].position.y, BlockGalleryCatalog::SpecimenHeight);
        CHECK_EQ(pair[1].position.y, BlockGalleryCatalog::SpecimenHeight);
        CHECK_EQ(pair[1].position.x, pair[0].position.x + 1);
        CHECK_EQ(pair[0].blockId, pair[1].blockId);
    }

    const auto sameTypePair = diagnosticPair(
        generator, BlockGalleryPlacementKind::SameTypeCullingDiagnostic);
    const auto coveragePair = diagnosticPair(
        generator, BlockGalleryPlacementKind::CoverageCullingDiagnostic);
    CHECK_EQ(
        registry.getType(sameTypePair.front().blockId).identifier,
        std::string("invented:joined"));
    CHECK_EQ(
        registry.getType(coveragePair.front().blockId).identifier,
        std::string("invented:coverage"));

    for (const auto* pair : {&sameTypePair, &coveragePair}) {
        const ChunkCoord chunk = worldToChunk(
            pair->front().position.x,
            pair->front().position.y,
            pair->front().position.z);
        const ChunkMesh mesh = buildGeneratedDiagnosticChunk(
            generator, registry, *pair);
        CHECK_EQ(countSharedXPlaneFaces(mesh, chunk, *pair),
                 static_cast<size_t>(0));
    }

    for (const std::string& identifier : {
             std::string("invented:aaa_coverage_mismatch"),
             std::string("invented:aab_joined_mismatch")}) {
        const BlockID blockId = *registry.findByIdentifier(identifier);
        Chunk chunk({0, 0, 0});
        chunk.setBlock(1, 1, 1, BlockState{blockId});
        chunk.setBlock(2, 1, 1, BlockState{blockId});
        const ChunkMesh mesh = MeshBuilder{}.build({
            .chunk = chunk,
            .registry = registry,
            .atlas = nullptr,
            .neighbors = {},
        });
        const std::vector<BlockGalleryBlockPlacement> mismatchedPair = {
            {{1, 1, 1}, blockId,
             BlockGalleryPlacementKind::CoverageCullingDiagnostic},
            {{2, 1, 1}, blockId,
             BlockGalleryPlacementKind::CoverageCullingDiagnostic},
        };
        CHECK_EQ(
            countSharedXPlaneFaces(mesh, {0, 0, 0}, mismatchedPair),
            static_cast<size_t>(2));
    }
}

TEST_CASE(BlockGalleryChunkGenerator_AccountsAcrossChunkBoundaries) {
    BlockRegistry registry;
    populateGalleryRegistry(registry, 100);
    registry.freeze();
    auto catalog = std::make_shared<const BlockGalleryCatalog>(registry);
    const BlockGalleryChunkGenerator gallery(registry, catalog);
    const std::vector<BlockGalleryBlockPlacement> placements =
        gallery.placements();
    std::map<ChunkCoord, size_t> expectedCounts;
    for (const BlockGalleryBlockPlacement& placement : placements) {
        ++expectedCounts[worldToChunk(
            placement.position.x,
            placement.position.y,
            placement.position.z)];
    }
    CHECK(expectedCounts.contains({-1, 0, -1}));
    CHECK(expectedCounts.contains({0, 0, 0}));
    CHECK(std::any_of(
        expectedCounts.begin(), expectedCounts.end(), [](const auto& entry) {
            return entry.first.x > 0 || entry.first.z > 0;
        }));

    for (const auto& [coord, expected] : expectedCounts) {
        ChunkBuffer buffer;
        gallery.generate(coord, buffer);
        Chunk chunk(coord);
        chunk.copyFrom(buffer.blocks, registry);
        CHECK_EQ(chunk.nonAirCount(), expected);
        size_t expectedOpaque = 0;
        for (const BlockGalleryBlockPlacement& placement : placements) {
            if (worldToChunk(
                    placement.position.x,
                    placement.position.y,
                    placement.position.z) == coord &&
                registry.getType(placement.blockId).isOpaque) {
                ++expectedOpaque;
            }
        }
        CHECK_EQ(chunk.opaqueCount(), expectedOpaque);
    }

    ChunkBuffer empty;
    gallery.generate({100, 0, 100}, empty);
    CHECK(std::all_of(
        empty.blocks.begin(), empty.blocks.end(), [](BlockState state) {
            return state.isAir();
        }));
}

TEST_CASE(BlockGalleryChunkGenerator_PublishesMinimalEmptyIdentity) {
    BlockRegistry registry;
    populateGalleryRegistry(registry, 4);
    registry.freeze();
    auto catalog = std::make_shared<const BlockGalleryCatalog>(registry);
    auto gallery = std::make_shared<const BlockGalleryChunkGenerator>(
        registry, catalog);
    const PreparedGeneratorDefinitionSnapshot identity =
        prepareBlockGalleryGeneratorIdentity(
            registry, gallery->worldBounds());
    CHECK_EQ(identity.sourceId, std::string("rigel:block_gallery_empty"));
    CHECK_EQ(identity.data.terrain.solidMaterial, std::string("base:air"));
    CHECK_EQ(identity.data.terrain.waterMaterial, std::string("base:air"));
    CHECK(!identity.data.caves.enabled);
    CHECK(!identity.data.structures.enabled);

    WorldGenerator emptyGenerator(registry, identity.data, 0);
    ChunkBuffer normalOutput;
    emptyGenerator.generate({0, 0, 0}, normalOutput);
    CHECK(std::all_of(
        normalOutput.blocks.begin(),
        normalOutput.blocks.end(),
        [](BlockState state) { return state.isAir(); }));

    WorldGenerator galleryGenerator(registry, identity.data, 0, 1, gallery);
    const ChunkCoord occupied = worldToChunk(
        catalog->entries().front().specimenPosition.x,
        catalog->entries().front().specimenPosition.y,
        catalog->entries().front().specimenPosition.z);
    CHECK(galleryGenerator.shouldPersistGeneratedChunk(occupied));
    CHECK(!galleryGenerator.shouldPersistGeneratedChunk({100, 0, 100}));
    ChunkBuffer galleryOutput;
    galleryGenerator.generate(occupied, galleryOutput);
    CHECK(std::any_of(
        galleryOutput.blocks.begin(),
        galleryOutput.blocks.end(),
        [](BlockState state) { return !state.isAir(); }));
}

TEST_CASE(BlockGalleryChunkGenerator_DirtyEvictionReloadsFromRam) {
    WorldResources resources;
    populateGalleryRegistry(resources.registry(), 100);
    resources.registry().freeze();
    World world(resources);
    WorldView view(world, resources);
    auto catalog = std::make_shared<const BlockGalleryCatalog>(
        resources.registry());
    auto gallery = std::make_shared<const BlockGalleryChunkGenerator>(
        resources.registry(), catalog);
    const PreparedGeneratorDefinitionSnapshot identity =
        prepareBlockGalleryGeneratorIdentity(
            resources.registry(), gallery->worldBounds());

    Persistence::FormatRegistry formats;
    formats.registerFormat(
        Persistence::Backends::Memory::descriptor(),
        Persistence::Backends::Memory::factory(),
        Persistence::Backends::Memory::probe());
    Persistence::PersistenceService persistence(formats);
    auto storage =
        std::make_shared<Persistence::InMemoryStorageBackend>();
    Persistence::PersistenceContext context;
    context.rootPath = "virtual/block-gallery";
    context.preferredFormat = "memory";
    context.storage = storage;
    context.providers = world.persistenceProvidersHandle();
    const Persistence::BootstrappedWorldGeneration bootstrapped =
        Persistence::bootstrapWorldGeneration(
            [&] {
                return Persistence::NewWorldGeneration{
                    "Block gallery", 0, identity};
            },
            persistence,
            resources.registry(),
            context);
    context.preferredFormat = bootstrapped.persistenceFormat;
    auto runtimeGenerator = std::make_shared<const WorldGenerator>(
        resources.registry(),
        bootstrapped.generation.definition,
        bootstrapped.generation.settings.seed,
        bootstrapped.generation.settings.generator.semanticsVersion,
        gallery);
    world.setGenerator(runtimeGenerator);
    view.setGenerator(runtimeGenerator);

    auto loader = std::make_shared<Persistence::AsyncChunkLoader>(
        persistence,
        context,
        world,
        runtimeGenerator->semanticsVersion(),
        0,
        0,
        runtimeGenerator);
    view.setChunkLoader([loader](ChunkLoadRequest request) {
        return loader->request(request);
    });
    view.setChunkPendingCallback([loader](ChunkCoord coord) {
        return loader->isPending(coord);
    });
    view.setChunkLoadDrain([loader](size_t budget) {
        return loader->drainCompletions(budget);
    });
    view.setChunkLoadCancel([loader](ChunkCoord coord) {
        loader->cancel(coord);
    });
    view.setChunkLoadDiagnosticsCallback([loader] {
        return loader->diagnostics();
    });
    view.setChunkLoadExecutionStateCallback([loader](ChunkCoord coord) {
        return loader->executionState(coord);
    });
    view.setChunkEvictionCallback([loader](ChunkCoord coord) {
        return loader->persistChunk(coord);
    });
    StreamingConfig streaming;
    streaming.viewDistanceChunks = 0;
    streaming.unloadDistanceChunks = 0;
    streaming.workerThreads = 0;
    streaming.maxResidentChunks = 1;
    view.setStreamConfig(streaming);
    view.markSpawnDiscoveryComplete();

    const BlockGalleryCatalogEntry& first = catalog->entries().front();
    const BlockGalleryCatalogEntry& last = catalog->entries().back();
    const ChunkCoord firstChunk = worldToChunk(
        first.specimenPosition.x,
        first.specimenPosition.y,
        first.specimenPosition.z);
    const ChunkCoord lastChunk = worldToChunk(
        last.specimenPosition.x,
        last.specimenPosition.y,
        last.specimenPosition.z);
    CHECK_NE(firstChunk, lastChunk);

    pumpStreaming(view, firstChunk.toWorldCenter());
    Chunk* generated = world.chunkManager().getChunk(firstChunk);
    CHECK(generated);
    CHECK(generated->isPersistDirty());
    CHECK_EQ(
        world.getBlock(
            first.specimenPosition.x,
            first.specimenPosition.y,
            first.specimenPosition.z).id,
        first.blockId);

    pumpStreaming(view, lastChunk.toWorldCenter());
    CHECK(!world.chunkManager().getChunk(firstChunk));
    CHECK(storage->exists(
        "virtual/block-gallery/zones/rigel/default/regions"));

    pumpStreaming(view, firstChunk.toWorldCenter());
    Chunk* reloaded = world.chunkManager().getChunk(firstChunk);
    CHECK(reloaded);
    CHECK(reloaded->loadedFromDisk());
    CHECK(!reloaded->isPersistDirty());
    CHECK_EQ(reloaded->worldGenVersion(), runtimeGenerator->semanticsVersion());
    CHECK_EQ(
        world.getBlock(
            first.specimenPosition.x,
            first.specimenPosition.y,
            first.specimenPosition.z).id,
        first.blockId);

    view.setChunkLoader({});
    view.setChunkPendingCallback({});
    view.setChunkLoadDrain({});
    view.setChunkLoadCancel({});
    view.setChunkLoadDiagnosticsCallback({});
    view.setChunkLoadExecutionStateCallback({});
    view.setChunkEvictionCallback({});
    loader.reset();
    view.clear();
}
