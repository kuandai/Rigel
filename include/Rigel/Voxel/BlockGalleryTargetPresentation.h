#pragma once

#include "BlockGalleryCatalog.h"
#include "BlockTargeting.h"

#include <cstddef>
#include <optional>
#include <string>

namespace Rigel::Voxel {

class BlockRegistry;

/** UI-ready metadata for one targeted catalog specimen. */
struct BlockGalleryTargetPresentation {
    std::string blockStateIdentifier;
    size_t catalogPosition = 0;
    size_t catalogSize = 0;
    BlockGalleryGridCoordinate gridCoordinate;
    std::string modelIdentifier;
    size_t cuboidCount = 0;
    std::string orientation;
    std::string renderLayer;
    bool opaque = false;
    bool solid = false;
    size_t textureBindingCount = 0;

    bool operator==(const BlockGalleryTargetPresentation&) const = default;
};

/**
 * Build presentation data only when a whole-cell target is the catalog
 * specimen at that exact world position.
 */
std::optional<BlockGalleryTargetPresentation>
makeBlockGalleryTargetPresentation(
    const BlockGalleryCatalog& catalog,
    const BlockRegistry& registry,
    const BlockTarget& target);

} // namespace Rigel::Voxel
