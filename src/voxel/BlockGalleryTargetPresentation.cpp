#include "Rigel/Voxel/BlockGalleryTargetPresentation.h"

#include "Rigel/Voxel/BlockRegistry.h"

#include <algorithm>
#include <array>
#include <string_view>
#include <utility>

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

struct TextureLayerPresentation {
    size_t bindingCount = 0;
    std::string effectiveLayers;
    std::string slotMappings;
};

TextureLayerPresentation textureLayerPresentation(const BlockType& type) {
    constexpr size_t maxPresentedSlotMappings = 4;

    TextureLayerPresentation result;
    std::array<bool, RenderLayerCount> presentedLayers{};
    presentedLayers[static_cast<size_t>(type.layer)] = true;
    for (const std::string& slot : type.model->textureSlots()) {
        const std::string* binding = type.textures.find(slot);
        if (binding && !binding->empty()) {
            ++result.bindingCount;
        }

        const RenderLayer layer = type.renderLayerForTextureSlot(slot);
        presentedLayers[static_cast<size_t>(layer)] = true;
    }

    size_t effectiveLayerCount = 0;
    for (size_t index = 0; index < presentedLayers.size(); ++index) {
        if (!presentedLayers[index]) {
            continue;
        }
        if (!result.effectiveLayers.empty()) {
            result.effectiveLayers += " + ";
        }
        result.effectiveLayers +=
            renderLayerName(static_cast<RenderLayer>(index));
        ++effectiveLayerCount;
    }

    if (effectiveLayerCount == 1) {
        return result;
    }

    const auto& slots = type.model->textureSlots();
    const size_t presentedSlotCount =
        std::min(slots.size(), maxPresentedSlotMappings);
    for (size_t index = 0; index < presentedSlotCount; ++index) {
        if (!result.slotMappings.empty()) {
            result.slotMappings += ", ";
        }
        const std::string& slot = slots[index];
        result.slotMappings += slot;
        result.slotMappings += '=';
        result.slotMappings +=
            renderLayerName(type.renderLayerForTextureSlot(slot));
    }
    if (slots.size() > presentedSlotCount) {
        result.slotMappings += ", ... (+";
        result.slotMappings +=
            std::to_string(slots.size() - presentedSlotCount);
        result.slotMappings += " more)";
    }
    return result;
}

} // namespace

std::optional<BlockGalleryTargetPresentation>
makeBlockGalleryTargetPresentation(
    const BlockGalleryCatalog& catalog,
    const BlockRegistry& registry,
    const BlockTarget& target
) {
    const BlockGalleryWorldPosition worldPosition{
        target.block.x,
        target.block.y,
        target.block.z,
    };
    const BlockGalleryCatalogEntry* entry = catalog.findBySpecimenPosition({
        worldPosition.x,
        worldPosition.y,
        worldPosition.z,
    });
    const BlockGalleryCullingDiagnosticPlacement* diagnostic =
        catalog.findCullingDiagnosticByPosition(worldPosition);
    if ((entry && diagnostic) || (!entry && !diagnostic)) {
        return std::nullopt;
    }

    const BlockID expectedBlockId = entry
        ? entry->blockId
        : diagnostic->sourceBlockId;
    const std::string& expectedIdentifier = entry
        ? entry->identifier
        : diagnostic->sourceIdentifier;
    if (target.state.id != expectedBlockId ||
        expectedBlockId.type >= registry.size()) {
        return std::nullopt;
    }

    const BlockType& type = registry.getType(expectedBlockId);
    if (type.identifier != expectedIdentifier) {
        return std::nullopt;
    }

    const TextureLayerPresentation textureLayers =
        textureLayerPresentation(type);
    std::optional<BlockGalleryCullingDiagnosticPresentation>
        diagnosticPresentation;
    if (diagnostic) {
        diagnosticPresentation = {
            .caseKind = diagnostic->caseKind,
            .label = diagnostic->label,
            .caseOrdinal = diagnostic->caseOrdinal,
            .caseCount = diagnostic->caseCount,
            .pairPosition = diagnostic->pairPosition,
            .pairOrdinal = diagnostic->pairOrdinal,
            .pairCount = diagnostic->pairCount,
        };
    }

    return BlockGalleryTargetPresentation{
        .blockStateIdentifier = type.identifier,
        .catalogPosition = entry ? entry->catalogIndex + 1 : 0,
        .catalogSize = catalog.entries().size(),
        .gridCoordinate = entry
            ? entry->gridCoordinate
            : BlockGalleryGridCoordinate{},
        .modelIdentifier = type.model->identifier(),
        .cuboidCount = type.model->cuboids().size(),
        .orientation = std::string(orientationName(type.model.orientation)),
        .renderLayer = std::string(renderLayerName(type.layer)),
        .effectiveRenderLayers = textureLayers.effectiveLayers,
        .textureSlotRenderLayers = textureLayers.slotMappings,
        .opaque = type.isOpaque,
        .solid = type.isSolid,
        .fullCube = type.model->isFullCube(),
        .cullSameType = type.cullSameType,
        .textureBindingCount = textureLayers.bindingCount,
        .cullingDiagnostic = std::move(diagnosticPresentation),
    };
}

} // namespace Rigel::Voxel
