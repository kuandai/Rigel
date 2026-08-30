#include "TestFramework.h"

#include "Rigel/Persistence/AsyncChunkLoader.h"
#include "Rigel/Persistence/Backends/Memory/MemoryFormat.h"
#include "Rigel/Persistence/ChunkSerializer.h"
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
#include <array>
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

struct GeneratedBlockPlacement {
    BlockGalleryWorldPosition position;
    BlockID blockId;

    bool operator==(const GeneratedBlockPlacement&) const = default;
};

using GeneratedBlockPair = std::array<GeneratedBlockPlacement, 2>;

int galleryLastZ(const BlockGalleryCatalog& catalog) {
    const BlockGalleryGridDimensions dimensions = catalog.gridDimensions();
    return dimensions.rows == 0
        ? 0
        : static_cast<int>(dimensions.rows - 1) *
            BlockGalleryCatalog::SpecimenSpacing;
}

std::vector<GeneratedBlockPlacement> generatedPlacements(
    const BlockGalleryChunkGenerator& generator,
    const BlockGalleryCatalog& catalog
) {
    const BlockGalleryGridDimensions dimensions = catalog.gridDimensions();
    const int galleryMaxX = dimensions.columns == 0
        ? 0
        : static_cast<int>(dimensions.columns - 1) *
            BlockGalleryCatalog::SpecimenSpacing;
    const int maxX = std::max(galleryMaxX + 1, Chunk::SIZE - 1);
    const int maxZ = galleryLastZ(catalog) + Chunk::SIZE;
    const ChunkCoord first = worldToChunk(-1, 0, -1);
    const ChunkCoord last = worldToChunk(maxX, 1, maxZ);

    std::vector<GeneratedBlockPlacement> result;
    for (int chunkZ = first.z; chunkZ <= last.z; ++chunkZ) {
        for (int chunkX = first.x; chunkX <= last.x; ++chunkX) {
            const ChunkCoord coord{chunkX, 0, chunkZ};
            ChunkBuffer buffer;
            generator.generate(coord, buffer);
            for (int z = 0; z < Chunk::SIZE; ++z) {
                for (int y = 0; y < Chunk::SIZE; ++y) {
                    for (int x = 0; x < Chunk::SIZE; ++x) {
                        const BlockState state = buffer.at(x, y, z);
                        if (state.isAir()) {
                            continue;
                        }
                        result.push_back({
                            {
                                chunkX * Chunk::SIZE + x,
                                y,
                                chunkZ * Chunk::SIZE + z,
                            },
                            state.id,
                        });
                    }
                }
            }
        }
    }
    return result;
}

std::vector<GeneratedBlockPair> diagnosticPairs(
    const std::vector<GeneratedBlockPlacement>& placements,
    const BlockGalleryCatalog& catalog
) {
    std::vector<GeneratedBlockPair> result;
    const int lastSpecimenZ = galleryLastZ(catalog);
    for (const GeneratedBlockPlacement& left : placements) {
        if (left.position.y != BlockGalleryCatalog::SpecimenHeight ||
            left.position.z <= lastSpecimenZ) {
            continue;
        }
        const auto right = std::find_if(
            placements.begin(), placements.end(),
            [&](const GeneratedBlockPlacement& candidate) {
                return candidate.blockId == left.blockId &&
                    candidate.position == BlockGalleryWorldPosition{
                        left.position.x + 1,
                        left.position.y,
                        left.position.z,
                    };
            });
        const auto previous = std::find_if(
            placements.begin(), placements.end(),
            [&](const GeneratedBlockPlacement& candidate) {
                return candidate.blockId == left.blockId &&
                    candidate.position == BlockGalleryWorldPosition{
                        left.position.x - 1,
                        left.position.y,
                        left.position.z,
                    };
            });
        if (right != placements.end() && previous == placements.end()) {
            result.push_back({left, *right});
        }
    }
    return result;
}

const GeneratedBlockPair& diagnosticPairFor(
    const std::vector<GeneratedBlockPair>& pairs,
    BlockID blockId
) {
    const auto found = std::find_if(
        pairs.begin(), pairs.end(), [&](const GeneratedBlockPair& pair) {
            return pair.front().blockId == blockId;
        });
    if (found == pairs.end()) {
        throw Rigel::Test::TestFailure(
            "Missing generated block-gallery diagnostic pair");
    }
    return *found;
}

size_t countSharedXPlaneFaces(
    const ChunkMesh& mesh,
    ChunkCoord chunk,
    const GeneratedBlockPair& pair
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
    const GeneratedBlockPair& pair
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
    const BlockGalleryCatalog catalog(registry);
    const BlockGalleryChunkGenerator generator(registry, catalog);
    const BlockGalleryChunkGenerator repeated(registry, catalog);
    const std::vector<GeneratedBlockPlacement> placements =
        generatedPlacements(generator, catalog);

    CHECK_EQ(placements, generatedPlacements(repeated, catalog));
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
        [&](const GeneratedBlockPlacement& placement) {
            const ChunkCoord placementChunk = worldToChunk(
                placement.position.x,
                placement.position.y,
                placement.position.z);
            const int dx = placementChunk.x - overviewChunk.x;
            const int dy = placementChunk.y - overviewChunk.y;
            const int dz = placementChunk.z - overviewChunk.z;
            return dx * dx + dy * dy + dz * dz <= 4;
        }));

    const BlockID referenceFloorBlock =
        *registry.findByIdentifier("invented:floor");
    const auto referenceFloor = std::find_if(
        placements.begin(), placements.end(),
        [referenceFloorBlock](const GeneratedBlockPlacement& placement) {
            return placement.position == BlockGalleryWorldPosition{-1, 0, -1} &&
                placement.blockId == referenceFloorBlock;
        });
    CHECK(referenceFloor != placements.end());
    CHECK_EQ(
        registry.getType(referenceFloor->blockId).identifier,
        std::string("invented:floor"));
    int floorMinX = 0;
    int floorMinZ = 0;
    for (const GeneratedBlockPlacement& placement : placements) {
        if (placement.position.y != 0 ||
            placement.blockId != referenceFloorBlock) {
            continue;
        }
        floorMinX = std::min(floorMinX, placement.position.x);
        floorMinZ = std::min(floorMinZ, placement.position.z);
    }
    CHECK_EQ(floorMinX, -1);
    CHECK_EQ(floorMinZ, -1);

    for (const BlockGalleryCatalogEntry& entry : catalog.entries()) {
        const BlockState state = generatedBlock(
            generator, entry.specimenPosition);
        CHECK_EQ(state.id, entry.blockId);
        CHECK_EQ(state.skyLight(), static_cast<uint8_t>(15));
    }
    CHECK_EQ(
        generatedBlock(generator, {-1, 0, -1}).id,
        referenceFloor->blockId);

    const std::vector<GeneratedBlockPair> pairs =
        diagnosticPairs(placements, catalog);
    CHECK_EQ(pairs.size(), static_cast<size_t>(3));

    const GeneratedBlockPair& opaquePair =
        diagnosticPairFor(pairs, referenceFloorBlock);
    const int diagnosticZ = opaquePair.front().position.z;
    CHECK(diagnosticZ > galleryLastZ(catalog));

    for (const GeneratedBlockPair& pair : pairs) {
        CHECK_EQ(pair.size(), static_cast<size_t>(2));
        CHECK_EQ(pair[0].position.z, diagnosticZ);
        CHECK_EQ(pair[1].position.z, diagnosticZ);
        CHECK_EQ(pair[0].position.y, BlockGalleryCatalog::SpecimenHeight);
        CHECK_EQ(pair[1].position.y, BlockGalleryCatalog::SpecimenHeight);
        CHECK_EQ(pair[1].position.x, pair[0].position.x + 1);
        CHECK_EQ(pair[0].blockId, pair[1].blockId);
    }

    const GeneratedBlockPair& sameTypePair = diagnosticPairFor(
        pairs, *registry.findByIdentifier("invented:joined"));
    const GeneratedBlockPair& coveragePair = diagnosticPairFor(
        pairs, *registry.findByIdentifier("invented:coverage"));

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
        const GeneratedBlockPair mismatchedPair = {{
            {{1, 1, 1}, blockId},
            {{2, 1, 1}, blockId},
        }};
        CHECK_EQ(
            countSharedXPlaneFaces(mesh, {0, 0, 0}, mismatchedPair),
            static_cast<size_t>(2));
    }
}

TEST_CASE(BlockGalleryChunkGenerator_AccountsAcrossChunkBoundaries) {
    BlockRegistry registry;
    populateGalleryRegistry(registry, 100);
    registry.freeze();
    const BlockGalleryCatalog catalog(registry);
    const BlockGalleryChunkGenerator gallery(registry, catalog);
    const std::vector<GeneratedBlockPlacement> placements =
        generatedPlacements(gallery, catalog);
    std::map<ChunkCoord, size_t> expectedCounts;
    for (const GeneratedBlockPlacement& placement : placements) {
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
        CHECK(gallery.containsChunk(coord));
        ChunkBuffer buffer;
        gallery.generate(coord, buffer);
        Chunk chunk(coord);
        chunk.copyFrom(buffer.blocks, registry);
        CHECK_EQ(chunk.nonAirCount(), expected);
        size_t expectedOpaque = 0;
        for (const GeneratedBlockPlacement& placement : placements) {
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
    CHECK(!gallery.containsChunk({100, 0, 100}));
    gallery.generate({100, 0, 100}, empty);
    CHECK(std::all_of(
        empty.blocks.begin(), empty.blocks.end(), [](BlockState state) {
            return state.isAir();
        }));
}

TEST_CASE(BlockGalleryChunkGenerator_PublishesMinimalEmptyIdentity) {
    WorldResources resources;
    BlockRegistry& registry = resources.registry();
    populateGalleryRegistry(registry, 4);
    registry.freeze();
    const BlockGalleryCatalog catalog(registry);
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

    auto emptyGenerator = std::make_shared<const WorldGenerator>(
        registry, identity.data, 0);
    ChunkBuffer normalOutput;
    emptyGenerator->generate({0, 0, 0}, normalOutput);
    CHECK(std::all_of(
        normalOutput.blocks.begin(),
        normalOutput.blocks.end(),
        [](BlockState state) { return state.isAir(); }));

    auto galleryGenerator = std::make_shared<const WorldGenerator>(
        registry, identity.data, 0, 1, gallery);
    const ChunkCoord occupied = worldToChunk(
        catalog.entries().front().specimenPosition.x,
        catalog.entries().front().specimenPosition.y,
        catalog.entries().front().specimenPosition.z);
    CHECK(galleryGenerator->shouldPersistGeneratedChunk(occupied));
    CHECK(!galleryGenerator->shouldPersistGeneratedChunk({100, 0, 100}));
    CHECK(emptyGenerator->matchesGenerationInputs(
        galleryGenerator->definition(),
        galleryGenerator->seed(),
        galleryGenerator->semanticsVersion()));
    CHECK(!emptyGenerator->matchesRuntimeGenerator(*galleryGenerator));
    CHECK(!galleryGenerator->matchesRuntimeGenerator(*emptyGenerator));
    auto equivalentGallery =
        std::make_shared<const BlockGalleryChunkGenerator>(registry, catalog);
    auto equivalentGalleryGenerator = std::make_shared<const WorldGenerator>(
        registry, identity.data, 0, 1, equivalentGallery);
    CHECK(galleryGenerator->matchesRuntimeGenerator(
        *equivalentGalleryGenerator));
    ChunkBuffer galleryOutput;
    galleryGenerator->generate(occupied, galleryOutput);
    CHECK(std::any_of(
        galleryOutput.blocks.begin(),
        galleryOutput.blocks.end(),
        [](BlockState state) { return !state.isAir(); }));

    World world(resources);
    WorldView view(world, resources);
    world.setGenerator(emptyGenerator);
    view.setGenerator(emptyGenerator);

    CHECK_THROWS(world.setGenerator(galleryGenerator));
    CHECK_EQ(world.generator(), emptyGenerator);
    CHECK_THROWS(view.setGenerator(galleryGenerator));
    CHECK_EQ(view.generator(), emptyGenerator);
}

TEST_CASE(BlockGalleryChunkGenerator_RejectsCatalogFromAnotherRegistry) {
    BlockRegistry registry;
    populateGalleryRegistry(registry, 1);
    registry.freeze();
    BlockRegistry otherRegistry;
    populateGalleryRegistry(otherRegistry, 1);
    otherRegistry.freeze();
    const BlockGalleryCatalog otherCatalog(otherRegistry);

    CHECK_THROWS((void)BlockGalleryChunkGenerator(registry, otherCatalog));
}

TEST_CASE(BlockGalleryChunkGenerator_RuntimeIdentityProtectsPartialSpanFill) {
    WorldResources resources;
    populateGalleryRegistry(resources.registry(), 100);
    resources.registry().freeze();
    const BlockGalleryCatalog catalog(resources.registry());
    auto gallery = std::make_shared<const BlockGalleryChunkGenerator>(
        resources.registry(), catalog);

    WorldResources alternateResources;
    populateGalleryRegistry(alternateResources.registry(), 0);
    alternateResources.registry().freeze();
    const BlockGalleryCatalog alternateCatalog(alternateResources.registry());
    auto alternateGallery =
        std::make_shared<const BlockGalleryChunkGenerator>(
            alternateResources.registry(), alternateCatalog);

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
    World world(resources);
    WorldView view(world, resources);
    Persistence::PersistenceContext context;
    context.rootPath = "virtual/runtime-identity";
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
    auto alternateGenerator = std::make_shared<const WorldGenerator>(
        alternateResources.registry(),
        bootstrapped.generation.definition,
        bootstrapped.generation.settings.seed,
        bootstrapped.generation.settings.generator.semanticsVersion,
        alternateGallery);
    CHECK(alternateGenerator->matchesGenerationInputs(
        runtimeGenerator->definition(),
        runtimeGenerator->seed(),
        runtimeGenerator->semanticsVersion()));
    CHECK(!runtimeGenerator->matchesRuntimeGenerator(*alternateGenerator));
    CHECK(!alternateGenerator->matchesRuntimeGenerator(*runtimeGenerator));

    const BlockGalleryCatalogEntry& specimen = catalog.entries().back();
    const ChunkCoord specimenChunk = worldToChunk(
        specimen.specimenPosition.x,
        specimen.specimenPosition.y,
        specimen.specimenPosition.z);
    ChunkBuffer expectedBase;
    ChunkBuffer alternateBase;
    runtimeGenerator->generate(specimenChunk, expectedBase);
    alternateGenerator->generate(specimenChunk, alternateBase);
    int localX = 0;
    int localY = 0;
    int localZ = 0;
    worldToLocal(
        specimen.specimenPosition.x,
        specimen.specimenPosition.y,
        specimen.specimenPosition.z,
        localX,
        localY,
        localZ);
    CHECK_EQ(expectedBase.at(localX, localY, localZ).id, specimen.blockId);
    CHECK_NE(
        alternateBase.at(localX, localY, localZ).id,
        expectedBase.at(localX, localY, localZ).id);

    world.setGenerator(runtimeGenerator);
    view.setGenerator(runtimeGenerator);
    CHECK_THROWS(world.setGenerator(alternateGenerator));
    CHECK_EQ(world.generator(), runtimeGenerator);
    CHECK_THROWS(view.setGenerator(alternateGenerator));
    CHECK_EQ(view.generator(), runtimeGenerator);
    CHECK_THROWS((void)Persistence::AsyncChunkLoader(
        persistence,
        context,
        world,
        runtimeGenerator->semanticsVersion(),
        0,
        0,
        alternateGenerator));

    Persistence::ChunkSpan partialSpan;
    partialSpan.chunkX = specimenChunk.x;
    partialSpan.chunkY = specimenChunk.y;
    partialSpan.chunkZ = specimenChunk.z;
    partialSpan.offsetX = Chunk::SIZE - 1;
    partialSpan.offsetY = Chunk::SIZE - 1;
    partialSpan.offsetZ = Chunk::SIZE - 1;
    partialSpan.sizeX = 1;
    partialSpan.sizeY = 1;
    partialSpan.sizeZ = 1;
    const Chunk partialChunk(specimenChunk);
    const Persistence::ChunkData partialData =
        Persistence::serializeChunkSpan(partialChunk, partialSpan);
    auto format = persistence.openFormat(context);
    const std::string zoneId = "rigel:default";
    Persistence::ChunkRegionSnapshot region;
    region.key = format->regionLayout().regionForChunk(
        zoneId, specimenChunk);
    region.chunks.push_back({
        {zoneId, specimenChunk.x, specimenChunk.y, specimenChunk.z},
        partialData,
        {},
    });
    format->chunkContainer().saveRegion(region);
    CHECK(format->descriptor().capabilities.fillMissingChunkSpans);

    Persistence::AsyncChunkLoader loader(
        persistence,
        context,
        world,
        runtimeGenerator->semanticsVersion(),
        0,
        0,
        runtimeGenerator);
    CHECK_EQ(
        loader.request({specimenChunk, 1}),
        ChunkLoadRequestResult::Queued);
    const std::vector<ChunkLoadCompletion> completions =
        loader.drainCompletions(1);
    CHECK_EQ(completions.size(), static_cast<size_t>(1));
    CHECK_EQ(completions.front().outcome, ChunkLoadOutcome::Loaded);
    const Chunk* loaded = world.chunkManager().getChunk(specimenChunk);
    CHECK(loaded);
    CHECK(loaded->loadedFromDisk());
    CHECK_EQ(
        world.getBlock(
            specimen.specimenPosition.x,
            specimen.specimenPosition.y,
            specimen.specimenPosition.z).id,
        specimen.blockId);
}

TEST_CASE(BlockGalleryChunkGenerator_DirtyEvictionReloadsFromRam) {
    WorldResources resources;
    populateGalleryRegistry(resources.registry(), 100);
    resources.registry().freeze();
    World world(resources);
    WorldView view(world, resources);
    const BlockGalleryCatalog catalog(resources.registry());
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

    auto emptyRuntimeGenerator = std::make_shared<const WorldGenerator>(
        resources.registry(),
        bootstrapped.generation.definition,
        bootstrapped.generation.settings.seed,
        bootstrapped.generation.settings.generator.semanticsVersion);
    CHECK(emptyRuntimeGenerator->matchesGenerationInputs(
        runtimeGenerator->definition(),
        runtimeGenerator->seed(),
        runtimeGenerator->semanticsVersion()));
    CHECK_THROWS((void)Persistence::AsyncChunkLoader(
        persistence,
        context,
        world,
        runtimeGenerator->semanticsVersion(),
        0,
        0,
        emptyRuntimeGenerator));
    CHECK_EQ(world.generator(), runtimeGenerator);

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

    const BlockGalleryCatalogEntry& first = catalog.entries().front();
    const BlockGalleryCatalogEntry& last = catalog.entries().back();
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
