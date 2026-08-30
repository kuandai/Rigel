#include "Rigel/Voxel/BlockGalleryChunkGenerator.h"

#include "BlockModelGeometry.h"
#include "Rigel/Voxel/BlockRegistry.h"
#include "Rigel/Voxel/WorldGenerator.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <utility>

namespace Rigel::Voxel {
namespace {

constexpr int kFloorBorder = 1;
constexpr int kDiagnosticSeparation = 8;
constexpr int kDiagnosticGroupSpacing = 4;
constexpr float kMaximumOverviewDistance = 128.0f;
constexpr float kMaximumOverviewHeight = 48.0f;

bool suitableReferenceFloor(const BlockType& type) {
    return type.isOpaque && type.layer == RenderLayer::Opaque &&
        type.model->isFullCube();
}

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
    std::shared_ptr<const BlockGalleryCatalog> catalog)
    : m_catalog(std::move(catalog)) {
    if (!m_catalog) {
        throw std::invalid_argument(
            "Block gallery chunk generation requires a catalog");
    }
    if (!registry.frozen()) {
        throw std::invalid_argument(
            "Block gallery chunk generation requires a frozen registry");
    }

    const BlockGalleryCatalogEntry* coverageDiagnostic = nullptr;
    const BlockGalleryCatalogEntry* sameTypeDiagnostic = nullptr;
    std::optional<BlockID> referenceFloorBlock;
    for (const BlockGalleryCatalogEntry& entry : m_catalog->entries()) {
        const BlockType& type = registry.getType(entry.blockId);
        if (!referenceFloorBlock && suitableReferenceFloor(type)) {
            referenceFloorBlock = entry.blockId;
        }
        if (!sameTypeDiagnostic &&
            detail::identicalPairCullsBothXBoundaries(
                type, detail::BoundaryCullReason::SameType)) {
            sameTypeDiagnostic = &entry;
        }
        if (!coverageDiagnostic && !type.model->isFullCube() &&
            detail::identicalPairCullsBothXBoundaries(
                type, detail::BoundaryCullReason::OpaqueCoverage)) {
            coverageDiagnostic = &entry;
        }
    }

    const BlockGalleryGridDimensions dimensions =
        m_catalog->gridDimensions();
    const int galleryMaxX = dimensions.columns == 0
        ? 0
        : static_cast<int>(dimensions.columns - 1) *
            BlockGalleryCatalog::SpecimenSpacing;
    const int galleryMaxZ = dimensions.rows == 0
        ? 0
        : static_cast<int>(dimensions.rows - 1) *
            BlockGalleryCatalog::SpecimenSpacing;

    if (referenceFloorBlock && !m_catalog->entries().empty()) {
        const int floorMinX = -kFloorBorder;
        const int floorMaxX = galleryMaxX + kFloorBorder;
        const int floorMinZ = -kFloorBorder;
        const int floorMaxZ = galleryMaxZ + kFloorBorder;
        for (int z = floorMinZ; z <= floorMaxZ; ++z) {
            for (int x = floorMinX; x <= floorMaxX; ++x) {
                addPlacement({
                    {x, 0, z},
                    *referenceFloorBlock,
                    BlockGalleryPlacementKind::ReferenceFloor,
                });
            }
        }
    }

    for (const BlockGalleryCatalogEntry& entry : m_catalog->entries()) {
        addPlacement({
            entry.specimenPosition,
            entry.blockId,
            BlockGalleryPlacementKind::Specimen,
        });
    }

    const BlockGalleryWorldPosition diagnosticOrigin = {
        0,
        BlockGalleryCatalog::SpecimenHeight,
        galleryMaxZ + kDiagnosticSeparation,
    };
    int diagnosticX = diagnosticOrigin.x;
    const auto addPair = [&](BlockID blockId,
                             BlockGalleryPlacementKind kind,
                             int x,
                             int z,
                             auto&& add) {
        add(BlockGalleryBlockPlacement{{x, 1, z}, blockId, kind});
        add(BlockGalleryBlockPlacement{{x + 1, 1, z}, blockId, kind});
    };

    if (referenceFloorBlock) {
        addPair(
            *referenceFloorBlock,
            BlockGalleryPlacementKind::OpaqueCullingDiagnostic,
            diagnosticX,
            diagnosticOrigin.z,
            [this](BlockGalleryBlockPlacement placement) {
                addPlacement(std::move(placement));
            });
        diagnosticX += kDiagnosticGroupSpacing;
    }
    if (sameTypeDiagnostic) {
        addPair(
            sameTypeDiagnostic->blockId,
            BlockGalleryPlacementKind::SameTypeCullingDiagnostic,
            diagnosticX,
            diagnosticOrigin.z,
            [this](BlockGalleryBlockPlacement placement) {
                addPlacement(std::move(placement));
            });
        diagnosticX += kDiagnosticGroupSpacing;
    }
    if (coverageDiagnostic) {
        addPair(
            coverageDiagnostic->blockId,
            BlockGalleryPlacementKind::CoverageCullingDiagnostic,
            diagnosticX,
            diagnosticOrigin.z,
            [this](BlockGalleryBlockPlacement placement) {
                addPlacement(std::move(placement));
            });
    }

    if (referenceFloorBlock) {
        for (int x = diagnosticOrigin.x; x <= diagnosticX + 1; ++x) {
            addPlacement({
                {x, 0, diagnosticOrigin.z},
                *referenceFloorBlock,
                BlockGalleryPlacementKind::ReferenceFloor,
            });
        }
    }

    const int diagnosticMaxX = diagnosticX + 1;
    const int extentX = std::max(galleryMaxX, diagnosticMaxX);
    const int extentZ = std::max(galleryMaxZ, diagnosticOrigin.z);
    m_overview.centerX = static_cast<float>(extentX) * 0.5f;
    m_overview.centerZ = static_cast<float>(extentZ) * 0.5f;
    const float span = static_cast<float>(std::max(extentX, extentZ) + 1);
    m_overview.cameraDistance = std::clamp(
        span * 0.57f, 24.0f, kMaximumOverviewDistance);
    m_overview.cameraHeight = std::clamp(
        span * 0.25f, 18.0f, kMaximumOverviewHeight);
}

void BlockGalleryChunkGenerator::addPlacement(
    BlockGalleryBlockPlacement placement) {
    const ChunkCoord chunk = worldToChunk(
        placement.position.x,
        placement.position.y,
        placement.position.z);
    m_placementsByChunk[chunk].push_back(std::move(placement));
}

std::vector<BlockGalleryBlockPlacement>
BlockGalleryChunkGenerator::placements() const {
    size_t placementCount = 0;
    for (const auto& [coord, placements] : m_placementsByChunk) {
        static_cast<void>(coord);
        placementCount += placements.size();
    }
    std::vector<BlockGalleryBlockPlacement> result;
    result.reserve(placementCount);
    for (const auto& [coord, placements] : m_placementsByChunk) {
        static_cast<void>(coord);
        result.insert(result.end(), placements.begin(), placements.end());
    }
    return result;
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
    for (const BlockGalleryBlockPlacement& placement : found->second) {
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
