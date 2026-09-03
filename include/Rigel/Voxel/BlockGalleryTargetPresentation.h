#pragma once

#include "BlockGalleryCatalog.h"
#include "BlockTargeting.h"

#include <cstddef>
#include <optional>
#include <string>

namespace Rigel::Voxel {

class BlockRegistry;

struct BlockGalleryCullingDiagnosticPresentation {
    BlockGalleryCullingCaseKind caseKind =
        BlockGalleryCullingCaseKind::OpaqueFullCube;
    std::string label;
    size_t caseOrdinal = 0;
    size_t caseCount = 0;
    BlockGalleryDiagnosticPairPosition pairPosition =
        BlockGalleryDiagnosticPairPosition::First;
    size_t pairOrdinal = 0;
    size_t pairCount = 0;

    bool operator==(
        const BlockGalleryCullingDiagnosticPresentation&) const = default;
};

/** UI-ready metadata for one targeted gallery block. */
struct BlockGalleryTargetPresentation {
    std::string blockStateIdentifier;
    size_t catalogPosition = 0;
    size_t catalogSize = 0;
    BlockGalleryGridCoordinate gridCoordinate;
    std::string modelIdentifier;
    size_t cuboidCount = 0;
    size_t hitCuboidPosition = 0;
    std::string hitFace;
    float hitDistance = 0.0f;
    std::string orientation;
    std::string renderLayer;
    std::string effectiveRenderLayers;
    std::string textureSlotRenderLayers;
    bool opaque = false;
    std::string collision;
    bool fullCube = false;
    bool cullSameType = false;
    size_t textureBindingCount = 0;
    std::optional<BlockGalleryCullingDiagnosticPresentation>
        cullingDiagnostic;

    bool operator==(const BlockGalleryTargetPresentation&) const = default;
};

/**
 * Build presentation data when the target's owning coordinate matches a
 * catalog specimen or culling diagnostic placement. Geometry may extend into
 * neighboring cells, but catalog identity remains attached to its owner.
 */
std::optional<BlockGalleryTargetPresentation>
makeBlockGalleryTargetPresentation(
    const BlockGalleryCatalog& catalog,
    const BlockRegistry& registry,
    const BlockTarget& target);

} // namespace Rigel::Voxel
