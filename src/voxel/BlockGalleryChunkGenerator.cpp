#include "Rigel/Voxel/BlockGalleryChunkGenerator.h"

#include "Rigel/Voxel/BlockRegistry.h"
#include "Rigel/Voxel/WorldGenerator.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace Rigel::Voxel {
namespace {

constexpr int kFloorBorder = 1;
constexpr float kMaximumOverviewDistance = 128.0f;
constexpr float kMaximumOverviewHeight = 48.0f;

GeneratorDefinitionData::Noise zeroNoise() {
    return {
        .octaves = 1,
        .frequency = 0.01f,
        .lacunarity = 1.0f,
        .persistence = 0.0f,
        .scale = 0.0f,
        .offset = 0.0f,
    };
}

GeneratorDefinitionData::Biome emptyBiome(
    std::string id,
    float continentalness) {
    GeneratorDefinitionData::Biome biome;
    biome.id = std::move(id);
    biome.target.continentalness = continentalness;
    biome.weight = 1.0f;
    biome.surface.push_back({"base:air", 1});
    return biome;
}

} // namespace

BlockGalleryChunkGenerator::BlockGalleryChunkGenerator(
    const BlockRegistry& registry,
    const BlockGalleryCatalog& catalog) {
    if (!registry.frozen()) {
        throw std::invalid_argument(
            "Block gallery chunk generation requires a frozen registry");
    }
    if (catalog.m_sourceRegistry != &registry) {
        throw std::invalid_argument(
            "Block gallery catalog does not belong to the supplied registry");
    }

    BlockID referenceFloorBlock = BlockRegistry::airId();
    for (const BlockGalleryCullingDiagnosticPlacement& placement :
         catalog.cullingDiagnosticPlacements()) {
        if (placement.caseKind ==
                BlockGalleryCullingCaseKind::OpaqueFullCube) {
            referenceFloorBlock = placement.sourceBlockId;
            break;
        }
    }

    const BlockGalleryGridDimensions dimensions =
        catalog.gridDimensions();
    const int galleryMaxX = dimensions.columns == 0
        ? 0
        : static_cast<int>(dimensions.columns - 1) *
            BlockGalleryCatalog::SpecimenSpacing;
    const int galleryMaxZ = dimensions.rows == 0
        ? 0
        : static_cast<int>(dimensions.rows - 1) *
            BlockGalleryCatalog::SpecimenSpacing;

    if (!referenceFloorBlock.isAir() && !catalog.entries().empty()) {
        const int floorMinX = -kFloorBorder;
        const int floorMaxX = galleryMaxX + kFloorBorder;
        const int floorMinZ = -kFloorBorder;
        const int floorMaxZ = galleryMaxZ + kFloorBorder;
        for (int z = floorMinZ; z <= floorMaxZ; ++z) {
            for (int x = floorMinX; x <= floorMaxX; ++x) {
                addPlacement({
                    {x, 0, z},
                    referenceFloorBlock,
                });
            }
        }
    }

    for (const BlockGalleryCatalogEntry& entry : catalog.entries()) {
        addPlacement({
            entry.specimenPosition,
            entry.blockId,
        });
    }

    int diagnosticMinX = 0;
    int diagnosticMaxX = 0;
    int diagnosticMaxZ = galleryMaxZ;
    bool hasDiagnostics = false;
    for (const BlockGalleryCullingDiagnosticPlacement& placement :
         catalog.cullingDiagnosticPlacements()) {
        addPlacement({placement.worldPosition, placement.sourceBlockId});
        if (!hasDiagnostics) {
            diagnosticMinX = placement.worldPosition.x;
            diagnosticMaxX = placement.worldPosition.x;
            hasDiagnostics = true;
        } else {
            diagnosticMinX = std::min(
                diagnosticMinX, placement.worldPosition.x);
            diagnosticMaxX = std::max(
                diagnosticMaxX, placement.worldPosition.x);
        }
        diagnosticMaxZ = std::max(
            diagnosticMaxZ, placement.worldPosition.z);
    }

    if (!referenceFloorBlock.isAir() && hasDiagnostics) {
        const int diagnosticZ =
            catalog.cullingDiagnosticPlacements().front().worldPosition.z;
        for (int x = diagnosticMinX; x <= diagnosticMaxX; ++x) {
            addPlacement({
                {x, 0, diagnosticZ},
                referenceFloorBlock,
            });
        }
    }

    const int extentX = std::max(galleryMaxX, diagnosticMaxX);
    const int extentZ = std::max(galleryMaxZ, diagnosticMaxZ);
    m_overview.centerX = static_cast<float>(extentX) * 0.5f;
    m_overview.centerZ = static_cast<float>(extentZ) * 0.5f;
    const float span = static_cast<float>(std::max(extentX, extentZ) + 1);
    m_overview.cameraDistance = std::clamp(
        span * 0.57f, 24.0f, kMaximumOverviewDistance);
    m_overview.cameraHeight = std::clamp(
        span * 0.25f, 18.0f, kMaximumOverviewHeight);
}

void BlockGalleryChunkGenerator::addPlacement(
    BlockPlacement placement) {
    const ChunkCoord chunk = worldToChunk(
        placement.position.x,
        placement.position.y,
        placement.position.z);
    m_placementsByChunk[chunk].push_back(std::move(placement));
}

bool BlockGalleryChunkGenerator::matchesRuntimeBehavior(
    const BlockGalleryChunkGenerator& other) const {
    return m_placementsByChunk == other.m_placementsByChunk;
}

void BlockGalleryChunkGenerator::validateGeneratorBounds(
    GeneratorDefinitionData::Bounds bounds) const {
    if (bounds != worldBounds()) {
        throw std::invalid_argument(
            "Block gallery bounds must match its published generator identity");
    }
}

bool BlockGalleryChunkGenerator::containsChunk(ChunkCoord coord) const {
    return m_placementsByChunk.contains(coord);
}

void BlockGalleryChunkGenerator::generate(
    ChunkCoord coord,
    ChunkBuffer& out) const {
    out.blocks.fill(BlockState{});
    const auto found = m_placementsByChunk.find(coord);
    if (found == m_placementsByChunk.end()) {
        return;
    }
    for (const BlockPlacement& placement : found->second) {
        int localX = 0;
        int localY = 0;
        int localZ = 0;
        worldToLocal(
            placement.position.x,
            placement.position.y,
            placement.position.z,
            localX,
            localY,
            localZ);
        BlockState state{placement.blockId};
        state.setSkyLight(15);
        out.at(localX, localY, localZ) = state;
    }
}

PreparedGeneratorDefinitionSnapshot prepareBlockGalleryGeneratorIdentity(
    const BlockRegistry& registry,
    GeneratorDefinitionData::Bounds bounds) {
    GeneratorDefinition definition;
    definition.schemaVersion = kGeneratorDefinitionSchemaVersion;
    definition.id = "rigel:block_gallery_empty";
    definition.sourceRevision = 1;
    definition.label = "Block gallery empty space";
    definition.description =
        "Minimal empty generation identity for the developer block gallery.";
    definition.data.bounds = bounds;
    definition.data.terrain.seaLevel = bounds.minY;
    definition.data.terrain.solidMaterial = "base:air";
    definition.data.terrain.waterMaterial = "base:air";
    definition.data.terrain.densityOutput = "terrain";
    definition.data.climate.global.temperature = zeroNoise();
    definition.data.climate.global.humidity = zeroNoise();
    definition.data.climate.global.continentalness = zeroNoise();
    definition.data.climate.local.temperature = zeroNoise();
    definition.data.climate.local.humidity = zeroNoise();
    definition.data.climate.local.continentalness = zeroNoise();
    definition.data.biomes.blendPower = 1.0f;
    definition.data.biomes.epsilon = 0.001f;
    definition.data.biomes.coast = {"coast", 1.0f, 1.0f};
    definition.data.biomes.entries = {
        emptyBiome("empty", 0.0f),
        emptyBiome("coast", 1.0f),
    };
    definition.data.densityGraph.outputs.push_back({"terrain", "empty"});
    definition.data.densityGraph.nodes.push_back({
        .id = "empty",
        .type = "constant",
        .value = -1.0f,
    });
    return prepareGeneratorDefinitionSnapshot(definition, registry);
}

} // namespace Rigel::Voxel
