#include "Rigel/Voxel/BlockGalleryTargetPresentation.h"

#include "Rigel/Voxel/BlockRegistry.h"

#include <string_view>

namespace Rigel::Voxel {
namespace {

std::string_view orientationName(BlockModelOrientation orientation) {
    switch (orientation) {
        case BlockModelOrientation::Identity: return "identity";
        case BlockModelOrientation::RotateX90: return "rotate_x_90";
        case BlockModelOrientation::RotateX270: return "rotate_x_270";
        case BlockModelOrientation::RotateY90: return "rotate_y_90";
        case BlockModelOrientation::RotateY180: return "rotate_y_180";
        case BlockModelOrientation::RotateY270: return "rotate_y_270";
        case BlockModelOrientation::RotateZ90: return "rotate_z_90";
    }
    return "unknown";
}

std::string_view renderLayerName(RenderLayer layer) {
    switch (layer) {
        case RenderLayer::Opaque: return "opaque";
        case RenderLayer::Cutout: return "cutout";
        case RenderLayer::Transparent: return "transparent";
        case RenderLayer::Emissive: return "emissive";
    }
    return "unknown";
}

size_t textureBindingCount(const BlockType& type) {
    size_t count = 0;
    for (const std::string& slot : type.model->textureSlots()) {
        const std::string* binding = type.textures.find(slot);
        if (binding && !binding->empty()) {
            ++count;
        }
    }
    return count;
}

} // namespace

std::optional<BlockGalleryTargetPresentation>
makeBlockGalleryTargetPresentation(
    const BlockGalleryCatalog& catalog,
    const BlockRegistry& registry,
    const BlockTarget& target
) {
    const BlockGalleryCatalogEntry* entry = catalog.findBySpecimenPosition({
        target.block.x,
        target.block.y,
        target.block.z,
    });
    if (!entry || target.state.id != entry->blockId ||
        entry->blockId.type >= registry.size()) {
        return std::nullopt;
    }

    const BlockType& type = registry.getType(entry->blockId);
    if (type.identifier != entry->identifier) {
        return std::nullopt;
    }

    return BlockGalleryTargetPresentation{
        .blockStateIdentifier = type.identifier,
        .catalogPosition = entry->catalogIndex + 1,
        .catalogSize = catalog.entries().size(),
        .gridCoordinate = entry->gridCoordinate,
        .modelIdentifier = type.model->identifier(),
        .cuboidCount = type.model->cuboids().size(),
        .orientation = std::string(orientationName(type.model.orientation)),
        .renderLayer = std::string(renderLayerName(type.layer)),
        .opaque = type.isOpaque,
        .solid = type.isSolid,
        .textureBindingCount = textureBindingCount(type),
    };
}

} // namespace Rigel::Voxel
