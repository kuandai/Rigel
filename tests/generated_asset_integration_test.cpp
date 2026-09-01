#include "TestFramework.h"

#include "GalleryCapturePublication.h"
#include "OpenGLFixture.h"
#include "ResourceRegistry.h"
#include "Rigel/Asset/AssetManager.h"
#include "Rigel/Asset/Types.h"
#include "Rigel/Entity/Entity.h"
#include "Rigel/Render/FrameRenderer.h"
#include "Rigel/Voxel/BlockGalleryCatalog.h"
#include "Rigel/Voxel/BlockGalleryChunkGenerator.h"
#include "Rigel/Voxel/BlockGalleryTargetPresentation.h"
#include "Rigel/Voxel/BlockLoader.h"
#include "Rigel/Voxel/BlockModel.h"
#include "Rigel/Voxel/BlockRegistry.h"
#include "Rigel/Voxel/Chunk.h"
#include "Rigel/Voxel/ChunkRenderer.h"
#include "Rigel/Voxel/GeneratorDefinitionLoader.h"
#include "Rigel/Voxel/MeshBuilder.h"
#include "Rigel/Voxel/TextureAtlas.h"
#include "Rigel/Voxel/WorldGenerator.h"
#include "Rigel/Voxel/WorldMeshStore.h"
#include "Rigel/Voxel/WorldResources.h"
#include "Rigel/Voxel/WorldView.h"
#include "Rigel/Voxel/World.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/vec2.hpp>

namespace {
using namespace Rigel::Voxel;

constexpr std::array<std::string_view, 5> RequiredMaterials = {
    "base:dirt",
    "base:grass",
    "base:sand",
    "base:stone_shale",
    "base:water[type=source]",
};

constexpr std::string_view SlabId =
    "base:wood_planks[slab_type=bottom]";
constexpr std::string_view TopSlabId =
    "base:wood_planks[slab_type=top]";
constexpr std::string_view StairId =
    "base:wood_planks[stair_type=bottom_PosX]";
constexpr std::string_view DoorId =
    "base:door_steel[part=bottom,power=on,direction=PosX]";
constexpr std::string_view LadderId =
    "base:ladder_steel[direction=PosX]";
constexpr std::string_view LeavesId = "base:leaves[type=permament]";
constexpr std::string_view WalkwayId = "base:steel_walkway";
constexpr std::string_view HandrailId =
    "base:steel_handrail[direction=PosX]";
constexpr std::string_view GlassId = "base:glass";
constexpr std::string_view LavaId = "base:lava[type=source]";
constexpr std::string_view VegetationId = "base:grass_blades";
constexpr std::string_view PistonHeadId =
    "base:piston[direction=PosX,type=advancing,part=head]";
constexpr std::string_view MultiCuboidId = "base:table_pedestal_wood";
constexpr std::string_view CroppedUvId =
    "base:laser_emitter[type=single,direction=NegZ]";
constexpr std::string_view TransparentId = "base:water[type=source]";
constexpr std::string_view AlphaCutoutId =
    "base:maize[type=farm,growth=4,part=bottom]";

constexpr int GalleryCaptureWidth = 256;
constexpr int GalleryCaptureHeight = 256;

class CollisionTestEntity : public Rigel::Entity::Entity {
public:
    CollisionTestEntity() : Entity("rigel:collision_test") {
        m_gravityModifier = 0.0f;
    }
};

struct GalleryVisualRepresentative {
    std::string_view captureName;
    std::string_view identifier;
};

constexpr std::array<GalleryVisualRepresentative, 9>
    GalleryVisualRepresentatives = {{
        {"01_cube", "base:stone_shale"},
        {"02_slab", SlabId},
        {"03_stair", StairId},
        {"04_multi_cuboid", MultiCuboidId},
        {"05_rotated", DoorId},
        {"06_cropped_uv", CroppedUvId},
        {"07_transparent", TransparentId},
        {"08_out_of_cell", PistonHeadId},
        {"09_alpha_cutout", AlphaCutoutId},
    }};

size_t countResources(std::string_view prefix, std::string_view suffix) {
    return static_cast<size_t>(std::count_if(
        ResourceRegistry::Paths().begin(), ResourceRegistry::Paths().end(),
        [&](std::string_view path) {
            return path.starts_with(prefix) && path.ends_with(suffix);
        }));
}

BlockID requireBlockId(
    const BlockRegistry& registry, std::string_view identifier
) {
    const auto id = registry.findByIdentifier(std::string(identifier));
    if (!id) {
        throw Rigel::Test::TestFailure(
            "Missing representative generated block: " +
            std::string(identifier));
    }
    return *id;
}

const BlockType& requireBlock(
    const BlockRegistry& registry, std::string_view identifier
) {
    return registry.getType(requireBlockId(registry, identifier));
}

BlockGalleryTargetPresentation requireGalleryPresentation(
    const BlockGalleryCatalog& catalog,
    const BlockRegistry& registry,
    std::string_view identifier
) {
    const BlockID blockId = requireBlockId(registry, identifier);
    const BlockGalleryCatalogEntry* entry = catalog.findByBlockId(blockId);
    CHECK(entry);
    const auto presentation = makeBlockGalleryTargetPresentation(
        catalog,
        registry,
        BlockTarget{
            .block = {
                entry->specimenPosition.x,
                entry->specimenPosition.y,
                entry->specimenPosition.z,
            },
            .state = BlockState{blockId},
        });
    CHECK(presentation);
    return *presentation;
}

size_t faceCount(const BlockModel& model) {
    size_t count = 0;
    for (const BlockModelCuboid& cuboid : model.cuboids()) {
        count += static_cast<size_t>(std::count_if(
            cuboid.faces.begin(), cuboid.faces.end(),
            [](const auto& face) { return face.has_value(); }));
    }
    return count;
}

bool hasCroppedUv(const BlockModel& model) {
    for (const BlockModelCuboid& cuboid : model.cuboids()) {
        for (const auto& optionalFace : cuboid.faces) {
            if (!optionalFace) continue;
            const BlockModelUvRect& uv = optionalFace->uv;
            if (uv.u0 != 0.0f || uv.v0 != 0.0f ||
                uv.u1 != 1.0f || uv.v1 != 1.0f) {
                return true;
            }
        }
    }
    return false;
}

bool hasReversedUv(const BlockModel& model) {
    for (const BlockModelCuboid& cuboid : model.cuboids()) {
        for (const auto& optionalFace : cuboid.faces) {
            if (optionalFace &&
                (optionalFace->uv.u0 > optionalFace->uv.u1 ||
                 optionalFace->uv.v0 > optionalFace->uv.v1)) {
                return true;
            }
        }
    }
    return false;
}

bool hasQuarterTurnedUv(const BlockModel& model) {
    for (const BlockModelCuboid& cuboid : model.cuboids()) {
        for (const auto& optionalFace : cuboid.faces) {
            if (optionalFace &&
                optionalFace->rotation != BlockModelUvRotation::None) {
                return true;
            }
        }
    }
    return false;
}

bool hasOutOfCellGeometry(const BlockModel& model) {
    return std::any_of(
        model.cuboids().begin(),
        model.cuboids().end(),
        [](const BlockModelCuboid& cuboid) {
            for (size_t axis = 0; axis < cuboid.bounds.min.size(); ++axis) {
                if (cuboid.bounds.min[axis] < 0.0f ||
                    cuboid.bounds.max[axis] > 1.0f) {
                    return true;
                }
            }
            return false;
        });
}

void checkTextureBindings(
    const BlockType& block, const TextureAtlas& atlas
) {
    CHECK(block.model.geometry);
    CHECK_EQ(block.textures.named().size(), block.model->textureSlots().size());
    for (const std::string& slot : block.model->textureSlots()) {
        const std::string* path = block.textures.find(slot);
        CHECK(path);
        CHECK(atlas.findTexture(*path).isValid());
    }
}

ChunkMesh buildOne(
    const BlockRegistry& registry,
    const TextureAtlas& atlas,
    std::string_view identifier
) {
    Chunk chunk({0, 0, 0});
    chunk.setBlock(1, 1, 1, BlockState{requireBlockId(registry, identifier)});
    return MeshBuilder{}.build({
        .chunk = chunk,
        .registry = registry,
        .atlas = &atlas,
        .neighbors = {},
    });
}

struct PositionRange {
    std::array<float, 3> min = {
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
    };
    std::array<float, 3> max = {
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest(),
    };
};

PositionRange positionRange(const ChunkMesh& mesh) {
    PositionRange result;
    for (const VoxelVertex& vertex : mesh.vertices) {
        const std::array position = {vertex.x, vertex.y, vertex.z};
        for (size_t axis = 0; axis < position.size(); ++axis) {
            result.min[axis] = std::min(result.min[axis], position[axis]);
            result.max[axis] = std::max(result.max[axis], position[axis]);
        }
    }
    return result;
}

void checkMeshCardinality(
    const ChunkMesh& mesh,
    size_t vertexCount,
    size_t indexCount,
    RenderLayer layer
) {
    CHECK_EQ(mesh.vertexCount(), vertexCount);
    CHECK_EQ(mesh.indexCount(), indexCount);
    CHECK_EQ(
        mesh.layers[static_cast<size_t>(layer)].indexCount,
        static_cast<uint32_t>(indexCount));
}

void checkExclusiveMeshLayer(
    const ChunkMesh& mesh,
    RenderLayer expectedLayer
) {
    CHECK(mesh.indexCount() > 0);
    for (size_t index = 0; index < mesh.layers.size(); ++index) {
        CHECK_EQ(
            mesh.layers[index].indexCount,
            index == static_cast<size_t>(expectedLayer)
                ? static_cast<uint32_t>(mesh.indexCount())
                : static_cast<uint32_t>(0));
    }
}

const BlockGalleryCatalogEntry& requireGalleryEntry(
    const BlockGalleryCatalog& catalog,
    const BlockRegistry& registry,
    std::string_view identifier
) {
    const BlockGalleryCatalogEntry* entry =
        catalog.findByBlockId(requireBlockId(registry, identifier));
    if (!entry) {
        throw Rigel::Test::TestFailure(
            "Missing representative gallery specimen: " +
            std::string(identifier));
    }
    return *entry;
}

struct GeneratedGalleryBlock {
    BlockGalleryWorldPosition position;
    BlockID blockId;
};

using GeneratedGalleryPair = std::array<GeneratedGalleryBlock, 2>;

BlockState generatedGalleryBlock(
    const BlockGalleryChunkGenerator& gallery,
    BlockGalleryWorldPosition position
) {
    const ChunkCoord coord = worldToChunk(
        position.x, position.y, position.z);
    ChunkBuffer buffer;
    gallery.generate(coord, buffer);
    int localX = 0;
    int localY = 0;
    int localZ = 0;
    worldToLocal(
        position.x,
        position.y,
        position.z,
        localX,
        localY,
        localZ);
    return buffer.at(localX, localY, localZ);
}

std::vector<GeneratedGalleryPair> generatedDiagnosticPairs(
    const BlockGalleryChunkGenerator& gallery,
    const BlockGalleryCatalog& catalog
) {
    const auto& placements = catalog.cullingDiagnosticPlacements();
    CHECK_EQ(placements.size() % 2, static_cast<size_t>(0));
    std::vector<GeneratedGalleryPair> result;
    result.reserve(placements.size() / 2);
    for (size_t index = 0; index < placements.size(); index += 2) {
        const BlockGalleryCullingDiagnosticPlacement& first =
            placements[index];
        const BlockGalleryCullingDiagnosticPlacement& second =
            placements[index + 1];
        CHECK_EQ(first.caseKind, second.caseKind);
        CHECK_EQ(
            first.pairPosition,
            BlockGalleryDiagnosticPairPosition::First);
        CHECK_EQ(
            second.pairPosition,
            BlockGalleryDiagnosticPairPosition::Second);
        CHECK_EQ(first.sourceBlockId, second.sourceBlockId);
        const BlockState generatedFirst = generatedGalleryBlock(
            gallery, first.worldPosition);
        const BlockState generatedSecond = generatedGalleryBlock(
            gallery, second.worldPosition);
        CHECK_EQ(generatedFirst.id, first.sourceBlockId);
        CHECK_EQ(generatedSecond.id, second.sourceBlockId);
        result.push_back({{
            {first.worldPosition, generatedFirst.id},
            {second.worldPosition, generatedSecond.id},
        }});
    }
    return result;
}

size_t countSharedXPlaneFaces(
    const ChunkMesh& mesh,
    ChunkCoord chunk,
    const GeneratedGalleryPair& pair
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

using FramebufferCapture = Rigel::Test::GalleryFramebufferCapture;

FramebufferCapture readFramebuffer(int width, int height) {
    FramebufferCapture capture{
        width,
        height,
        std::vector<uint8_t>(
            static_cast<size_t>(width) * static_cast<size_t>(height) * 4),
        std::vector<float>(
            static_cast<size_t>(width) * static_cast<size_t>(height)),
    };
    glFinish();
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(
        0,
        0,
        width,
        height,
        GL_RGBA,
        GL_UNSIGNED_BYTE,
        capture.rgba.data());
    glReadPixels(
        0,
        0,
        width,
        height,
        GL_DEPTH_COMPONENT,
        GL_FLOAT,
        capture.depth.data());
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
    return capture;
}

size_t countRenderedCenterPixels(const FramebufferCapture& capture) {
    size_t rendered = 0;
    for (int y = capture.height * 5 / 16;
         y < capture.height * 11 / 16;
         ++y) {
        for (int x = capture.width * 5 / 16;
             x < capture.width * 11 / 16;
             ++x) {
            const size_t offset = static_cast<size_t>(
                x + y * capture.width) * 4;
            const int red = capture.rgba[offset];
            const int green = capture.rgba[offset + 1];
            const int blue = capture.rgba[offset + 2];
            if (std::abs(red - 51) > 3 ||
                std::abs(green - 77) > 3 ||
                std::abs(blue - 77) > 3) {
                ++rendered;
            }
        }
    }
    return rendered;
}

void checkOpaqueWoodOccludesFloor(const FramebufferCapture& capture) {
    const size_t offset = static_cast<size_t>(
        capture.width / 2 + capture.height / 2 * capture.width) * 4;
    const int red = capture.rgba[offset];
    const int green = capture.rgba[offset + 1];
    const int blue = capture.rgba[offset + 2];

    // The floor is pale blue while the slab is saturated brown. Applying the
    // transparent pass to the slab blends those colors at its covered center.
    CHECK(red - green >= 20);
    CHECK(red - blue >= 40);
}

void checkAlphaCutoutRevealsFloor(const FramebufferCapture& capture) {
    const auto colorOffset = [&](int x, int topDownY) {
        const int y = capture.height - 1 - topDownY;
        return static_cast<size_t>(x + y * capture.width) * 4;
    };
    const auto depthOffset = [&](int x, int topDownY) {
        const int y = capture.height - 1 - topDownY;
        return static_cast<size_t>(x + y * capture.width);
    };

    // These fixed production-camera samples cross a transparent opening in
    // the maize texture between two opaque leaves. The pale floor color and
    // its farther depth prove that the transparent texel wrote neither target.
    const size_t leftColor = colorOffset(128, 128);
    const size_t openingColor = colorOffset(132, 128);
    const size_t rightColor = colorOffset(136, 128);
    CHECK(capture.rgba[leftColor + 1] > capture.rgba[leftColor] + 10);
    CHECK(capture.rgba[leftColor + 1] > capture.rgba[leftColor + 2] + 30);
    CHECK(capture.rgba[rightColor] > capture.rgba[rightColor + 1] + 50);
    CHECK(capture.rgba[rightColor + 1] > capture.rgba[rightColor + 2] + 50);
    CHECK(capture.rgba[openingColor] >= 100);
    CHECK(capture.rgba[openingColor + 1] >= 100);
    CHECK(capture.rgba[openingColor + 2] >= 100);
    const float leftDepth = capture.depth[depthOffset(128, 128)];
    const float openingDepth = capture.depth[depthOffset(132, 128)];
    const float rightDepth = capture.depth[depthOffset(136, 128)];
    CHECK(openingDepth > leftDepth);
    CHECK(openingDepth > rightDepth);
}

void checkAuthoredWaterAlpha(const FramebufferCapture& capture) {
    const int x = 145;
    const int topDownY = 128;
    const int y = capture.height - 1 - topDownY;
    const size_t offset = static_cast<size_t>(x + y * capture.width) * 4;
    const int red = capture.rgba[offset];
    const int blue = capture.rgba[offset + 2];

    // This front-face sample blends the constant-alpha water texture over the
    // clear color. The authored alpha produces a materially stronger blue shift
    // than the removed global 0.5 transparent multiplier.
    CHECK(blue >= 90);
    CHECK(blue >= red + 50);
}

void checkMixedTableMaterials(const FramebufferCapture& capture) {
    const auto colorOffset = [&](int x, int topDownY) {
        const int y = capture.height - 1 - topDownY;
        return static_cast<size_t>(x + y * capture.width) * 4;
    };
    const auto depthAt = [&](int x, int topDownY) {
        const int y = capture.height - 1 - topDownY;
        return capture.depth[static_cast<size_t>(x + y * capture.width)];
    };

    // The cropped glass texture contributes low partial alpha over the wood at
    // this fixed production-camera sample. In the cutout regression it matched
    // the opaque wood reference because the glass texel was discarded.
    const size_t glassOverWood = colorOffset(103, 104);
    const size_t woodBorder = colorOffset(128, 110);
    CHECK(capture.rgba[glassOverWood] >=
          capture.rgba[woodBorder] + 8);
    CHECK(capture.rgba[glassOverWood + 1] >=
          capture.rgba[woodBorder + 1] + 15);
    CHECK(capture.rgba[glassOverWood + 2] >=
          capture.rgba[woodBorder + 2] + 20);
    CHECK(capture.rgba[glassOverWood] < 145);
    CHECK(capture.rgba[glassOverWood + 2] < 110);

    // A second partial-alpha glass sample remains visibly blended instead of
    // being either discarded to the pale floor or drawn as opaque cyan. These
    // bounds require the texture's authored alpha; applying another 0.5 scale
    // puts every channel below 145.
    const size_t glassOverFloor = colorOffset(148, 104);
    CHECK(capture.rgba[glassOverFloor] >= 170);
    CHECK(capture.rgba[glassOverFloor] <= 188);
    CHECK(capture.rgba[glassOverFloor + 1] >= 165);
    CHECK(capture.rgba[glassOverFloor + 1] <= 183);
    CHECK(capture.rgba[glassOverFloor + 2] >= 160);
    CHECK(capture.rgba[glassOverFloor + 2] <= 176);

    const auto checkOpaqueWood = [&](int x, int topDownY) {
        const size_t color = colorOffset(x, topDownY);
        CHECK(capture.rgba[color] >= capture.rgba[color + 1] + 20);
        CHECK(capture.rgba[color] >= capture.rgba[color + 2] + 40);
    };
    checkOpaqueWood(128, 110);
    checkOpaqueWood(128, 145);
    CHECK(depthAt(128, 110) + 0.005f < depthAt(180, 110));
    CHECK(depthAt(128, 145) + 0.001f < depthAt(180, 145));
}

bool pathIsWithin(
    const std::filesystem::path& path,
    const std::filesystem::path& directory
) {
    auto pathPart = path.begin();
    for (auto directoryPart = directory.begin();
         directoryPart != directory.end();
         ++directoryPart, ++pathPart) {
        if (pathPart == path.end() || *pathPart != *directoryPart) {
            return false;
        }
    }
    return true;
}

std::optional<std::filesystem::path> galleryCaptureDirectory() {
    const char* configured = std::getenv("RIGEL_GALLERY_CAPTURE_DIRECTORY");
    if (!configured || configured[0] == '\0') {
        return std::nullopt;
    }

    const std::filesystem::path requested(configured);
    if (!requested.is_absolute()) {
        throw Rigel::Test::TestFailure(
            "Gallery capture directory must be an absolute path");
    }

    const std::filesystem::path source = std::filesystem::weakly_canonical(
        std::filesystem::path(RIGEL_TEST_SOURCE_DIRECTORY));
    const std::filesystem::path destination =
        std::filesystem::weakly_canonical(requested);
    if (pathIsWithin(destination, source)) {
        throw Rigel::Test::TestFailure(
            "Gallery captures must be stored outside the source tree");
    }

    std::error_code error;
    std::filesystem::create_directories(destination.parent_path(), error);
    if (error) {
        throw Rigel::Test::TestFailure(
            "Failed to create gallery capture parent directory: " +
            error.message());
    }
    return destination;
}

} // namespace

TEST_CASE(GeneratedAssets_LoadNormalizedBlockDefinitions) {
    Rigel::Asset::AssetManager assets;
    assets.loadManifest("manifest.yaml");

    BlockModelRegistry models;
    BlockRegistry preparedRegistry;
    TextureAtlas preparedAtlas;
    const BlockLoadReport report = BlockLoader{}.loadFromManifest(
        assets, models, preparedRegistry, preparedAtlas);
    CHECK(report.modelsLoaded > 0);
    CHECK_EQ(report.modelsFailed, static_cast<size_t>(0));
    CHECK_EQ(report.failed, static_cast<size_t>(0));
    CHECK(report.loaded > 0);
    for (const std::string_view identifier : RequiredMaterials) {
        CHECK(preparedRegistry.findByIdentifier(std::string(identifier)));
    }

#ifdef RIGEL_EXPECT_COSMIC_REACH_0_6_1_ASSETS
    CHECK_EQ(countResources("blocks/", ".yaml"), static_cast<size_t>(2021));
    CHECK_EQ(
        countResources("models/blocks/", ".yaml"), static_cast<size_t>(51));
    CHECK_EQ(
        countResources("models/entities/", ".json"), static_cast<size_t>(16));
    CHECK_EQ(
        countResources("animations/entities/", ".json"), static_cast<size_t>(7));
    CHECK_EQ(countResources("textures/", ".png"), static_cast<size_t>(438));
    CHECK_EQ(countResources("sounds/", ".ogg"), static_cast<size_t>(59));
    CHECK_EQ(report.modelsDiscovered, static_cast<size_t>(51));
    CHECK_EQ(report.modelsLoaded, static_cast<size_t>(51));
    CHECK_EQ(models.size(), static_cast<size_t>(53));
    CHECK_EQ(report.discovered, static_cast<size_t>(2021));
    CHECK_EQ(report.loaded, static_cast<size_t>(2020));
    CHECK_EQ(report.skipped, static_cast<size_t>(1));
    CHECK_EQ(preparedRegistry.size(), static_cast<size_t>(2021));
    CHECK_EQ(preparedAtlas.textureCount(), static_cast<size_t>(276));
#endif

    preparedRegistry.freeze();
    const BlockGalleryCatalog catalog(preparedRegistry);
    CHECK_EQ(
        catalog.diagnostics().loadedRegistrationCount,
        preparedRegistry.size());
    CHECK_EQ(
        catalog.entries().size() +
            catalog.emptyGeometryExclusions().size(),
        preparedRegistry.size());

    std::vector<bool> accounted(preparedRegistry.size(), false);
    for (const BlockGalleryCatalogEntry& entry : catalog.entries()) {
        CHECK(entry.blockId.type < accounted.size());
        CHECK(!accounted[entry.blockId.type]);
        accounted[entry.blockId.type] = true;
        const BlockType& type = preparedRegistry.getType(entry.blockId);
        CHECK_EQ(entry.identifier, type.identifier);
        CHECK(!type.model->isEmpty());
        CHECK_EQ(catalog.findBySpecimenPosition(entry.specimenPosition), &entry);
    }
    for (const BlockGalleryEmptyGeometryExclusion& exclusion :
         catalog.emptyGeometryExclusions()) {
        CHECK(exclusion.blockId.type < accounted.size());
        CHECK(!accounted[exclusion.blockId.type]);
        accounted[exclusion.blockId.type] = true;
        const BlockType& type = preparedRegistry.getType(exclusion.blockId);
        CHECK_EQ(exclusion.identifier, type.identifier);
        CHECK(type.model->isEmpty());
    }
    CHECK(std::all_of(accounted.begin(), accounted.end(), [](bool value) {
        return value;
    }));

#ifdef RIGEL_EXPECT_COSMIC_REACH_0_6_1_ASSETS
    CHECK_EQ(catalog.entries().size(), static_cast<size_t>(2020));
    CHECK_EQ(
        catalog.gridDimensions(),
        (BlockGalleryGridDimensions{45, 45}));
    for (size_t row = 0; row < 44; ++row) {
        for (size_t column = 0; column < 45; ++column) {
            CHECK(catalog.findByGridCoordinate({column, row}));
        }
    }
    size_t finalRowSpecimenCount = 0;
    for (size_t column = 0; column < 45; ++column) {
        finalRowSpecimenCount +=
            catalog.findByGridCoordinate({column, 44}) ? 1 : 0;
    }
    CHECK_EQ(finalRowSpecimenCount, static_cast<size_t>(40));
    CHECK_EQ(
        catalog.emptyGeometryExclusions().size(),
        static_cast<size_t>(1));
    CHECK_EQ(
        catalog.emptyGeometryExclusions().front().identifier,
        std::string("base:air"));

    for (const std::string_view identifier : {
             LeavesId, WalkwayId, LadderId, HandrailId, VegetationId}) {
        const auto presentation = requireGalleryPresentation(
            catalog, preparedRegistry, identifier);
        CHECK_EQ(
            presentation.effectiveRenderLayers,
            std::string("cutout"));
        CHECK(presentation.textureSlotRenderLayers.empty());
    }
    for (const std::string_view identifier : {GlassId, TransparentId}) {
        const auto presentation = requireGalleryPresentation(
            catalog, preparedRegistry, identifier);
        CHECK_EQ(
            presentation.effectiveRenderLayers,
            std::string("transparent"));
        CHECK(presentation.textureSlotRenderLayers.empty());
    }
    const auto lavaPresentation = requireGalleryPresentation(
        catalog, preparedRegistry, LavaId);
    CHECK_EQ(
        lavaPresentation.effectiveRenderLayers,
        std::string("opaque"));
    CHECK(lavaPresentation.textureSlotRenderLayers.empty());

    const auto mixedTablePresentation = requireGalleryPresentation(
        catalog, preparedRegistry, MultiCuboidId);
    CHECK_EQ(mixedTablePresentation.renderLayer, std::string("opaque"));
    CHECK_EQ(
        mixedTablePresentation.effectiveRenderLayers,
        std::string("opaque + transparent"));
    CHECK_EQ(
        mixedTablePresentation.textureSlotRenderLayers,
        std::string("border=opaque, top=transparent"));
#endif
}

TEST_CASE(GeneratedAssets_QueryAndCollideWithNormalizedShapes) {
#ifndef RIGEL_EXPECT_COSMIC_REACH_0_6_1_ASSETS
    SKIP_TEST("representative collision expectations target Cosmic Reach 0.6.1");
#else
    Rigel::Asset::AssetManager assets;
    assets.loadManifest("manifest.yaml");

    WorldResources resources;
    BlockModelRegistry models;
    TextureAtlas atlas;
    const BlockLoadReport report = BlockLoader{}.loadFromManifest(
        assets, models, resources.registry(), atlas);
    CHECK_EQ(report.failed, static_cast<size_t>(0));
    CHECK_EQ(report.loaded, static_cast<size_t>(2020));
    resources.registry().freeze();

    struct StairRepresentative {
        std::string_view identifier;
        BlockCollisionBox upper;
        glm::vec2 upperFootprint;
        glm::vec2 lowerFootprint;
    };
    const std::array stairs = {
        StairRepresentative{
            "base:wood_planks[stair_type=bottom_PosX]",
            {{0.5f, 0.5f, 0.0f}, {1.0f, 1.0f, 1.0f}},
            {0.75f, 0.5f},
            {0.25f, 0.5f}},
        StairRepresentative{
            "base:wood_planks[stair_type=bottom_NegX]",
            {{0.0f, 0.5f, 0.0f}, {0.5f, 1.0f, 1.0f}},
            {0.25f, 0.5f},
            {0.75f, 0.5f}},
        StairRepresentative{
            "base:wood_planks[stair_type=bottom_PosZ]",
            {{0.0f, 0.5f, 0.5f}, {1.0f, 1.0f, 1.0f}},
            {0.5f, 0.75f},
            {0.5f, 0.25f}},
        StairRepresentative{
            "base:wood_planks[stair_type=bottom_NegZ]",
            {{0.0f, 0.5f, 0.0f}, {1.0f, 1.0f, 0.5f}},
            {0.5f, 0.25f},
            {0.5f, 0.75f}},
    };

    World world(resources);
    world.setBlock(
        0, 0, 0,
        BlockState{requireBlockId(resources.registry(), SlabId)});
    world.setBlock(
        2, 0, 0,
        BlockState{requireBlockId(resources.registry(), TopSlabId)});
    for (size_t index = 0; index < stairs.size(); ++index) {
        world.setBlock(
            4 + static_cast<int>(index) * 2,
            0,
            0,
            BlockState{requireBlockId(
                resources.registry(), stairs[index].identifier)});
    }
    world.setBlock(
        12, 0, 2,
        BlockState{requireBlockId(resources.registry(), VegetationId)});

    const auto queryAt = [&](int x, int z = 0) {
        std::vector<BlockCollisionBox> boxes;
        const bool accepted = world.forEachCollisionBox(
            BlockCollisionBox{
                {static_cast<float>(x) - 0.1f, -0.1f,
                 static_cast<float>(z) - 0.1f},
                {static_cast<float>(x) + 1.1f, 1.1f,
                 static_cast<float>(z) + 1.1f}},
            [&](const BlockCollisionBox& box) { boxes.push_back(box); });
        CHECK(accepted);
        return boxes;
    };
    CHECK_EQ(
        queryAt(0),
        (std::vector<BlockCollisionBox>{
            {{0.0f, 0.0f, 0.0f}, {1.0f, 0.5f, 1.0f}}}));
    CHECK_EQ(
        queryAt(2),
        (std::vector<BlockCollisionBox>{
            {{2.0f, 0.5f, 0.0f}, {3.0f, 1.0f, 1.0f}}}));
    for (size_t index = 0; index < stairs.size(); ++index) {
        const int x = 4 + static_cast<int>(index) * 2;
        const std::vector<BlockCollisionBox> boxes = queryAt(x);
        CHECK_EQ(boxes.size(), static_cast<size_t>(2));
        BlockCollisionBox expectedUpper = stairs[index].upper;
        expectedUpper.min[0] += static_cast<float>(x);
        expectedUpper.max[0] += static_cast<float>(x);
        CHECK(
            std::find(boxes.begin(), boxes.end(), expectedUpper) !=
            boxes.end());
    }

    const auto fall = [&](float x, float z) {
        CollisionTestEntity entity;
        entity.setLocalBounds(Rigel::Entity::Aabb{
            glm::vec3(-0.1f, -0.2f, -0.1f),
            glm::vec3(0.1f, 0.2f, 0.1f)});
        entity.setPosition(x, 3.0f, z);
        entity.setVelocity(glm::vec3(0.0f, -10.0f, 0.0f));
        entity.update(world, 0.5f);
        CHECK(entity.collidedY());
        CHECK(entity.isOnGround());
        return entity.position().y;
    };
    CHECK_NEAR(
        fall(0.5f, 0.5f),
        0.7f + BlockCollisionContactTolerance,
        0.00001f);
    CHECK_NEAR(
        fall(2.5f, 0.5f),
        1.2f + BlockCollisionContactTolerance,
        0.00001f);
    for (size_t index = 0; index < stairs.size(); ++index) {
        const float x = 4.0f + static_cast<float>(index) * 2.0f;
        CHECK_NEAR(
            fall(
                x + stairs[index].upperFootprint.x,
                stairs[index].upperFootprint.y),
            1.2f + BlockCollisionContactTolerance,
            0.00001f);
        CHECK_NEAR(
            fall(
                x + stairs[index].lowerFootprint.x,
                stairs[index].lowerFootprint.y),
            0.7f + BlockCollisionContactTolerance,
            0.00001f);
    }

    const BlockType& vegetation =
        requireBlock(resources.registry(), VegetationId);
    CHECK(!vegetation.model->isEmpty());
    CHECK(vegetation.collision.isEmpty());
    CHECK(queryAt(12, 2).empty());
    CollisionTestEntity passThrough;
    passThrough.setLocalBounds(Rigel::Entity::Aabb{
        glm::vec3(-0.1f), glm::vec3(0.1f)});
    passThrough.setPosition(10.0f, 0.5f, 2.5f);
    passThrough.setVelocity(glm::vec3(4.0f, 0.0f, 0.0f));
    passThrough.update(world, 1.0f);
    CHECK(!passThrough.collidedX());
    CHECK_EQ(passThrough.position().x, 14.0f);
#endif
}

TEST_CASE(GeneratedAssets_BuildUploadAndSubmitRepresentativeModels) {
#ifndef RIGEL_EXPECT_COSMIC_REACH_0_6_1_ASSETS
    SKIP_TEST("representative model expectations target Cosmic Reach 0.6.1");
#else
    Rigel::Test::HiddenOpenGLContext context;
    context.require();

    Rigel::Asset::AssetManager assets;
    assets.loadManifest("manifest.yaml");

    WorldResources resources;
    resources.initialize(assets);
    CHECK(resources.initialized());
    CHECK(resources.registry().frozen());
    CHECK_EQ(resources.registry().size(), static_cast<size_t>(2021));
    CHECK_EQ(resources.textureAtlas().textureCount(), static_cast<size_t>(276));

    const auto generator = loadPreparedGeneratorDefinitionSnapshot(
        assets, resources.registry(), "rigel:default");
    WorldGenerator worldGenerator(resources.registry(), generator.data, 1337u);
    ChunkBuffer generatedChunk;
    worldGenerator.generate({0, 3, 0}, generatedChunk);
    CHECK(!generatedChunk.blocks.empty());

    const BlockType& slab = requireBlock(resources.registry(), SlabId);
    const BlockType& stair = requireBlock(resources.registry(), StairId);
    const BlockType& door = requireBlock(resources.registry(), DoorId);
    const BlockType& ladder = requireBlock(resources.registry(), LadderId);
    const BlockType& alphaCutout =
        requireBlock(resources.registry(), AlphaCutoutId);
    const BlockType& mixedTable =
        requireBlock(resources.registry(), MultiCuboidId);
    const BlockType& pistonHead =
        requireBlock(resources.registry(), PistonHeadId);
    const BlockType& leaves = requireBlock(resources.registry(), LeavesId);
    const BlockType& walkway = requireBlock(resources.registry(), WalkwayId);
    const BlockType& handrail = requireBlock(resources.registry(), HandrailId);
    const BlockType& glass = requireBlock(resources.registry(), GlassId);
    const BlockType& water = requireBlock(resources.registry(), TransparentId);
    const BlockType& lava = requireBlock(resources.registry(), LavaId);
    const BlockType& vegetation =
        requireBlock(resources.registry(), VegetationId);
    for (const BlockType* block : {
             &slab, &stair, &door, &ladder, &alphaCutout, &mixedTable,
             &pistonHead}) {
        CHECK(!block->model->isFullCube());
        checkTextureBindings(*block, resources.textureAtlas());
    }

    CHECK_EQ(slab.model->cuboids().size(), static_cast<size_t>(1));
    CHECK_EQ(faceCount(*slab.model.geometry), static_cast<size_t>(6));
    CHECK(hasCroppedUv(*slab.model.geometry));
    CHECK_EQ(stair.model->cuboids().size(), static_cast<size_t>(4));
    CHECK_EQ(faceCount(*stair.model.geometry), static_cast<size_t>(10));
    CHECK(hasQuarterTurnedUv(*stair.model.geometry));
    CHECK_EQ(door.model.orientation, BlockModelOrientation::RotateY90);
    CHECK_EQ(faceCount(*door.model.geometry), static_cast<size_t>(6));
    CHECK_EQ(ladder.model.orientation, BlockModelOrientation::RotateY90);
    CHECK_EQ(faceCount(*ladder.model.geometry), static_cast<size_t>(2));
    CHECK_EQ(pistonHead.model.orientation, BlockModelOrientation::RotateY90);
    CHECK_EQ(pistonHead.model->cuboids().size(), static_cast<size_t>(2));
    CHECK_EQ(faceCount(*pistonHead.model.geometry), static_cast<size_t>(12));
    CHECK(hasCroppedUv(*pistonHead.model.geometry));
    CHECK(hasReversedUv(*pistonHead.model.geometry));
    CHECK(hasQuarterTurnedUv(*pistonHead.model.geometry));
    CHECK_EQ(pistonHead.model->cuboids().back().bounds.max[2], 1.25f);

    for (const BlockType* cutout : {
             &leaves, &walkway, &ladder, &handrail}) {
        CHECK(!cutout->isOpaque);
        CHECK_EQ(cutout->layer, RenderLayer::Cutout);
    }
    CHECK(!glass.isOpaque);
    CHECK_EQ(glass.layer, RenderLayer::Transparent);
    CHECK(!water.isOpaque);
    CHECK_EQ(water.layer, RenderLayer::Transparent);
    CHECK(!lava.isOpaque);
    CHECK(lava.cullSameType);
    CHECK_EQ(lava.layer, RenderLayer::Opaque);
    CHECK(!vegetation.isOpaque);
    CHECK_EQ(vegetation.layer, RenderLayer::Cutout);

    for (const auto& [identifier, expectedLayer] :
         std::array<std::pair<std::string_view, RenderLayer>, 8>{
             std::pair{LeavesId, RenderLayer::Cutout},
             std::pair{WalkwayId, RenderLayer::Cutout},
             std::pair{LadderId, RenderLayer::Cutout},
             std::pair{HandrailId, RenderLayer::Cutout},
             std::pair{GlassId, RenderLayer::Transparent},
             std::pair{TransparentId, RenderLayer::Transparent},
             std::pair{LavaId, RenderLayer::Opaque},
             std::pair{VegetationId, RenderLayer::Cutout},
         }) {
        checkExclusiveMeshLayer(
            buildOne(resources.registry(), resources.textureAtlas(), identifier),
            expectedLayer);
    }

    const ChunkMesh slabMesh = buildOne(
        resources.registry(), resources.textureAtlas(), SlabId);
    checkMeshCardinality(slabMesh, 24, 36, RenderLayer::Opaque);
    const PositionRange slabRange = positionRange(slabMesh);
    CHECK_EQ(slabRange.min[1], 1.0f);
    CHECK_EQ(slabRange.max[1], 1.5f);

    const ChunkMesh stairMesh = buildOne(
        resources.registry(), resources.textureAtlas(), StairId);
    checkMeshCardinality(stairMesh, 40, 60, RenderLayer::Opaque);

    const ChunkMesh doorMesh = buildOne(
        resources.registry(), resources.textureAtlas(), DoorId);
    checkMeshCardinality(doorMesh, 24, 36, RenderLayer::Opaque);
    const PositionRange doorRange = positionRange(doorMesh);
    CHECK_NEAR(doorRange.max[2] - doorRange.min[2], 0.125f, 0.00001f);

    const ChunkMesh ladderMesh = buildOne(
        resources.registry(), resources.textureAtlas(), LadderId);
    checkMeshCardinality(ladderMesh, 8, 12, RenderLayer::Cutout);
    const PositionRange ladderRange = positionRange(ladderMesh);
    CHECK_NEAR(
        ladderRange.max[0] - ladderRange.min[0], 0.00625f, 0.00001f);

    const ChunkMesh alphaCutoutMesh = buildOne(
        resources.registry(), resources.textureAtlas(), AlphaCutoutId);
    checkMeshCardinality(
        alphaCutoutMesh, 16, 24, RenderLayer::Cutout);

    CHECK_EQ(mixedTable.layer, RenderLayer::Opaque);
    CHECK_EQ(
        mixedTable.renderLayerForTextureSlot("border"),
        RenderLayer::Opaque);
    CHECK_EQ(
        mixedTable.renderLayerForTextureSlot("top"),
        RenderLayer::Transparent);
    const ChunkMesh mixedTableMesh = buildOne(
        resources.registry(), resources.textureAtlas(), MultiCuboidId);
    CHECK_EQ(mixedTableMesh.vertexCount(), static_cast<size_t>(144));
    CHECK_EQ(mixedTableMesh.indexCount(), static_cast<size_t>(216));
    CHECK_EQ(
        mixedTableMesh.layers[
            static_cast<size_t>(RenderLayer::Opaque)].indexCount,
        static_cast<uint32_t>(180));
    CHECK_EQ(
        mixedTableMesh.layers[
            static_cast<size_t>(RenderLayer::Transparent)].indexCount,
        static_cast<uint32_t>(36));

    const ChunkMesh pistonMesh = buildOne(
        resources.registry(), resources.textureAtlas(), PistonHeadId);
    checkMeshCardinality(pistonMesh, 48, 72, RenderLayer::Opaque);
    const PositionRange pistonRange = positionRange(pistonMesh);
    CHECK_EQ(pistonRange.min[0], 0.75f);
    CHECK_EQ(pistonRange.max[0], 2.0f);
    CHECK(std::any_of(
        pistonMesh.vertices.begin(), pistonMesh.vertices.end(),
        [](const VoxelVertex& vertex) {
            return vertex.u == 0.375f || vertex.u == 0.625f ||
                   vertex.v == 0.375f || vertex.v == 0.625f;
        }));

    Chunk chunk({0, 0, 0});
    chunk.setBlock(
        2, 2, 2, BlockState{requireBlockId(resources.registry(), SlabId)});
    chunk.setBlock(
        5, 2, 2, BlockState{requireBlockId(resources.registry(), StairId)});
    chunk.setBlock(
        8, 2, 2, BlockState{requireBlockId(resources.registry(), DoorId)});
    chunk.setBlock(
        11, 2, 2, BlockState{requireBlockId(resources.registry(), LadderId)});
    chunk.setBlock(
        13, 2, 2,
        BlockState{requireBlockId(resources.registry(), AlphaCutoutId)});
    chunk.setBlock(
        15, 2, 2,
        BlockState{requireBlockId(resources.registry(), PistonHeadId)});
    ChunkMesh mesh = MeshBuilder{}.build({
        .chunk = chunk,
        .registry = resources.registry(),
        .atlas = &resources.textureAtlas(),
        .neighbors = {},
    });
    CHECK_EQ(mesh.vertexCount(), static_cast<size_t>(160));
    CHECK_EQ(mesh.indexCount(), static_cast<size_t>(240));
    CHECK_EQ(
        mesh.layers[static_cast<size_t>(RenderLayer::Opaque)].indexCount,
        static_cast<uint32_t>(204));
    CHECK_EQ(
        mesh.layers[static_cast<size_t>(RenderLayer::Cutout)].indexCount,
        static_cast<uint32_t>(36));
    CHECK_EQ(
        mesh.layers[static_cast<size_t>(RenderLayer::Transparent)].indexCount,
        static_cast<uint32_t>(0));

    const std::array<std::string_view, 6> expectedTextures = {
        "textures/blocks/foliage/wood_planks.png",
        "textures/blocks/furniture/steel_door_bottom.png",
        "textures/blocks/furniture/ladder_steel.png",
        "textures/blocks/machines/machine_top.png",
        "textures/blocks/machines/piston_advancing_side.png",
        "textures/blocks/crops/maize/maize_bottom_growth_4.png",
    };
    std::set<uint16_t> expectedLayers;
    for (const std::string_view path : expectedTextures) {
        const TextureHandle handle =
            resources.textureAtlas().findTexture(std::string(path));
        CHECK(handle.isValid());
        expectedLayers.insert(handle.index);
    }
    std::set<uint16_t> actualLayers;
    for (const VoxelVertex& vertex : mesh.vertices) {
        actualLayers.insert(vertex.textureLayer);
    }
    CHECK_EQ(actualLayers, expectedLayers);

    WorldMeshStore store;
    const ChunkCoord coord{0, 0, 0};
    store.set(coord, std::move(mesh));
    const auto installed = store.snapshot(coord);
    CHECK(installed.has_value());
    CHECK(!installed->empty);

    WorldRenderContext renderContext;
    renderContext.meshes = &store;
    renderContext.atlas = &resources.textureAtlas();
    renderContext.shader =
        assets.get<Rigel::Asset::ShaderAsset>("shaders/voxel");
    renderContext.renderDistanceWorldUnits = 128.0f;
    glViewport(0, 0, 64, 64);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    ChunkRenderer renderer;
    renderer.render(renderContext);
    CHECK_EQ(renderer.cachedMeshCount(), static_cast<size_t>(1));
    CHECK(renderer.hasDrawnMesh(
        store.storeId(), coord, installed->revision));
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));

    renderer.releaseResources();
    resources.releaseRenderResources();
#endif
}

TEST_CASE(GeneratedAssets_RenderGallerySpecimensThroughFrameRenderer) {
#ifndef RIGEL_EXPECT_COSMIC_REACH_0_6_1_ASSETS
    SKIP_TEST("representative model expectations target Cosmic Reach 0.6.1");
#else
    Rigel::Test::HiddenOpenGLContext context(
        GalleryCaptureWidth, GalleryCaptureHeight);
    context.require();

    Rigel::Asset::AssetManager assets;
    assets.loadManifest("manifest.yaml");
    WorldResources resources;
    resources.initialize(assets);

    const BlockGalleryCatalog catalog(resources.registry());
    auto gallery = std::make_shared<const BlockGalleryChunkGenerator>(
        resources.registry(), catalog);
    const std::vector<GeneratedGalleryPair> diagnosticPairs =
        generatedDiagnosticPairs(*gallery, catalog);
    CHECK_EQ(diagnosticPairs.size(), static_cast<size_t>(3));
    std::set<std::string> diagnosticLabels;
    for (const BlockGalleryCullingDiagnosticPlacement& placement :
         catalog.cullingDiagnosticPlacements()) {
        const auto presentation = makeBlockGalleryTargetPresentation(
            catalog,
            resources.registry(),
            BlockTarget{
                .block = {
                    placement.worldPosition.x,
                    placement.worldPosition.y,
                    placement.worldPosition.z,
                },
                .state = BlockState{placement.sourceBlockId},
            });
        CHECK(presentation);
        CHECK(presentation->cullingDiagnostic);
        CHECK_EQ(
            presentation->cullingDiagnostic->caseKind,
            placement.caseKind);
        CHECK_EQ(presentation->cullingDiagnostic->label, placement.label);
        CHECK_EQ(
            presentation->cullingDiagnostic->caseOrdinal,
            placement.caseOrdinal);
        CHECK_EQ(
            presentation->cullingDiagnostic->pairOrdinal,
            placement.pairOrdinal);
        CHECK_EQ(presentation->catalogPosition, static_cast<size_t>(0));
        CHECK(!presentation->effectiveRenderLayers.empty());
        diagnosticLabels.insert(placement.label);
    }
    CHECK_EQ(diagnosticLabels.size(), diagnosticPairs.size());
    const PreparedGeneratorDefinitionSnapshot identity =
        prepareBlockGalleryGeneratorIdentity(
            resources.registry(), gallery->worldBounds());
    auto generator = std::make_shared<const WorldGenerator>(
        resources.registry(), identity.data, 0, 1, gallery);

    size_t partialDiagnosticCount = 0;
    for (const GeneratedGalleryPair& pair : diagnosticPairs) {
        if (resources.registry().getType(pair.front().blockId)
                .model->isFullCube()) {
            continue;
        }
        ++partialDiagnosticCount;
        CHECK_EQ(pair.size(), static_cast<size_t>(2));
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

        ChunkBuffer generated;
        generator->generate(chunkCoord, generated);
        Chunk chunk(chunkCoord);
        chunk.copyFrom(generated.blocks, resources.registry());
        const ChunkMesh mesh = MeshBuilder{}.build({
            .chunk = chunk,
            .registry = resources.registry(),
            .atlas = &resources.textureAtlas(),
            .neighbors = {},
        });
        CHECK_EQ(
            countSharedXPlaneFaces(mesh, chunkCoord, pair),
            static_cast<size_t>(0));

        const int localY = pair.front().position.y -
            chunkCoord.y * Chunk::SIZE;
        const int localZ = pair.front().position.z -
            chunkCoord.z * Chunk::SIZE;
        const int leftX = pair.front().position.x -
            chunkCoord.x * Chunk::SIZE;
        const int rightX = pair.back().position.x -
            chunkCoord.x * Chunk::SIZE;
        for (const int localX : {leftX, rightX}) {
            Chunk isolated(chunkCoord);
            isolated.setBlock(
                localX, localY, localZ,
                BlockState{pair.front().blockId});
            const ChunkMesh isolatedMesh = MeshBuilder{}.build({
                .chunk = isolated,
                .registry = resources.registry(),
                .atlas = &resources.textureAtlas(),
                .neighbors = {},
            });
            CHECK(
                countSharedXPlaneFaces(
                    isolatedMesh, chunkCoord, pair) > 0);
        }
    }
    CHECK_EQ(partialDiagnosticCount, static_cast<size_t>(2));

    World world(resources);
    world.setGenerator(generator);
    WorldView view(world, resources);
    view.initialize(assets);
    view.setGenerator(generator);
    StreamingConfig streaming;
    streaming.viewDistanceChunks = 1;
    streaming.unloadDistanceChunks = 64;
    streaming.workerThreads = 0;
    view.setStreamConfig(streaming);
    view.markSpawnDiscoveryComplete();

    Rigel::Render::FrameRenderer renderer;
    renderer.initialize(assets);
    renderer.setVerticalFovDegrees(70.0);

    CHECK(requireBlock(resources.registry(), "base:stone_shale")
              .model->isFullCube());
    CHECK_EQ(
        requireBlock(resources.registry(), SlabId).model->cuboids().size(),
        static_cast<size_t>(1));
    CHECK(
        requireBlock(resources.registry(), StairId).model->cuboids().size() >
        1);
    CHECK(
        requireBlock(resources.registry(), MultiCuboidId)
                .model->cuboids().size() >
        1);
    CHECK_NE(
        requireBlock(resources.registry(), DoorId).model.orientation,
        BlockModelOrientation::Identity);
    CHECK(hasCroppedUv(
        *requireBlock(resources.registry(), CroppedUvId).model.geometry));
    CHECK_EQ(
        requireBlock(resources.registry(), TransparentId).layer,
        RenderLayer::Transparent);
    CHECK(!requireBlock(resources.registry(), AlphaCutoutId).isOpaque);
    CHECK_EQ(
        requireBlock(resources.registry(), AlphaCutoutId).layer,
        RenderLayer::Cutout);
    CHECK(!requireBlock(resources.registry(), SlabId).isOpaque);
    CHECK_EQ(
        requireBlock(resources.registry(), SlabId).layer,
        RenderLayer::Opaque);
    CHECK(!requireBlock(resources.registry(), StairId).isOpaque);
    CHECK_EQ(
        requireBlock(resources.registry(), StairId).layer,
        RenderLayer::Opaque);
    CHECK(!requireBlock(resources.registry(), DoorId).isOpaque);
    CHECK_EQ(
        requireBlock(resources.registry(), DoorId).layer,
        RenderLayer::Opaque);
    CHECK(!requireBlock(resources.registry(), MultiCuboidId).isOpaque);
    CHECK_EQ(
        requireBlock(resources.registry(), MultiCuboidId).layer,
        RenderLayer::Opaque);
    CHECK_EQ(
        requireBlock(resources.registry(), MultiCuboidId)
            .renderLayerForTextureSlot("border"),
        RenderLayer::Opaque);
    CHECK_EQ(
        requireBlock(resources.registry(), MultiCuboidId)
            .renderLayerForTextureSlot("top"),
        RenderLayer::Transparent);
    CHECK(hasOutOfCellGeometry(
        *requireBlock(resources.registry(), PistonHeadId).model.geometry));

    const std::optional<std::filesystem::path> captureDirectory =
        galleryCaptureDirectory();
    std::vector<Rigel::Test::NamedGalleryFramebufferCapture> captures;
    if (captureDirectory) {
        captures.reserve(GalleryVisualRepresentatives.size() + 2);
    }
    const auto renderGalleryView = [&](const glm::vec3& camera,
                                       const glm::vec3& target) {
        const ChunkCoord targetChunk = worldToChunk(
            static_cast<int>(std::floor(target.x)),
            static_cast<int>(std::floor(target.y)),
            static_cast<int>(std::floor(target.z)));
        for (size_t attempt = 0;
             attempt < 24 && !view.meshStore().snapshot(targetChunk);
             ++attempt) {
            view.updateStreaming(camera);
            view.updateMeshes();
        }
        const auto installed = view.meshStore().snapshot(targetChunk);
        CHECK(installed.has_value());
        CHECK(!installed->empty);

        for (int frame = 0; frame < 3; ++frame) {
            renderer.render({
                .world = world,
                .worldView = view,
                .cameraPosition = camera,
                .cameraTarget = target,
                .cameraForward = glm::normalize(target - camera),
                .viewportWidth = GalleryCaptureWidth,
                .viewportHeight = GalleryCaptureHeight,
                .deltaTime = 1.0f / 60.0f,
            });
        }
        return readFramebuffer(GalleryCaptureWidth, GalleryCaptureHeight);
    };
    for (const GalleryVisualRepresentative& representative :
         GalleryVisualRepresentatives) {
        const BlockGalleryCatalogEntry& entry = requireGalleryEntry(
            catalog, resources.registry(), representative.identifier);
        const glm::vec3 target{
            static_cast<float>(entry.specimenPosition.x) + 0.5f,
            static_cast<float>(entry.specimenPosition.y) + 0.5f,
            static_cast<float>(entry.specimenPosition.z) + 0.5f,
        };
        const glm::vec3 camera = target + glm::vec3{2.5f, 1.5f, 2.5f};
        FramebufferCapture capture = renderGalleryView(camera, target);
        CHECK(countRenderedCenterPixels(capture) >= static_cast<size_t>(4));
        if (representative.identifier == SlabId) {
            checkOpaqueWoodOccludesFloor(capture);
        }
        if (representative.identifier == AlphaCutoutId) {
            checkAlphaCutoutRevealsFloor(capture);
        }
        if (representative.identifier == TransparentId) {
            checkAuthoredWaterAlpha(capture);
        }
        if (representative.identifier == MultiCuboidId) {
            checkMixedTableMaterials(capture);
        }
        if (captureDirectory) {
            captures.push_back({
                std::string(representative.captureName),
                std::move(capture),
            });
        }
    }

    const BlockGalleryGridDimensions dimensions = catalog.gridDimensions();
    CHECK(dimensions.columns > 0);
    CHECK(dimensions.rows > 1);
    const BlockGalleryCatalogEntry* rowEnd =
        catalog.findByIndex(dimensions.columns - 1);
    const BlockGalleryCatalogEntry* nextRowBegin =
        catalog.findByIndex(dimensions.columns);
    CHECK(rowEnd);
    CHECK(nextRowBegin);
    CHECK_EQ(nextRowBegin->catalogIndex, rowEnd->catalogIndex + 1);
    CHECK_EQ(nextRowBegin->gridCoordinate.column, rowEnd->gridCoordinate.column);
    CHECK_EQ(nextRowBegin->gridCoordinate.row, rowEnd->gridCoordinate.row + 1);
    CHECK_EQ(nextRowBegin->specimenPosition.x, rowEnd->specimenPosition.x);
    CHECK_EQ(
        nextRowBegin->specimenPosition.z,
        rowEnd->specimenPosition.z + BlockGalleryCatalog::SpecimenSpacing);
    const glm::vec3 rowTransitionTarget{
        static_cast<float>(rowEnd->specimenPosition.x) + 0.5f,
        static_cast<float>(BlockGalleryCatalog::SpecimenHeight) + 0.5f,
        (static_cast<float>(rowEnd->specimenPosition.z) +
         static_cast<float>(nextRowBegin->specimenPosition.z)) * 0.5f + 0.5f,
    };
    FramebufferCapture rowTransitionCapture = renderGalleryView(
        rowTransitionTarget + glm::vec3{12.0f, 10.0f, 12.0f},
        rowTransitionTarget);
    CHECK(
        countRenderedCenterPixels(rowTransitionCapture) >=
        static_cast<size_t>(4));
    if (captureDirectory) {
        captures.push_back({
            "10_dense_row_transition",
            std::move(rowTransitionCapture),
        });
    }

    const auto& diagnosticPlacements = catalog.cullingDiagnosticPlacements();
    CHECK(!diagnosticPlacements.empty());
    const float diagnosticMinX = static_cast<float>(
        diagnosticPlacements.front().worldPosition.x);
    const float diagnosticMaxX = static_cast<float>(
        diagnosticPlacements.back().worldPosition.x);
    const glm::vec3 diagnosticTarget{
        (diagnosticMinX + diagnosticMaxX) * 0.5f + 0.5f,
        static_cast<float>(BlockGalleryCatalog::SpecimenHeight) + 0.5f,
        static_cast<float>(diagnosticPlacements.front().worldPosition.z) + 0.5f,
    };
    FramebufferCapture diagnosticCapture = renderGalleryView(
        diagnosticTarget + glm::vec3{10.0f, 8.0f, 12.0f},
        diagnosticTarget);
    CHECK(
        countRenderedCenterPixels(diagnosticCapture) >=
        static_cast<size_t>(4));
    if (captureDirectory) {
        captures.push_back({
            "11_culling_diagnostics",
            std::move(diagnosticCapture),
        });
    }
    if (captureDirectory) {
        Rigel::Test::publishGalleryCaptureSet(captures, *captureDirectory);
    }

    renderer.release();
    view.clear();
    view.releaseRenderResources();
    resources.releaseRenderResources();
#endif
}
