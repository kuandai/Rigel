#include "Rigel/Core/DebugBlockCatalog.h"

#include "Rigel/Voxel/Chunk.h"

#include <algorithm>
#include <unordered_set>

namespace Rigel::Core {

namespace {

bool isEdgeLocalCoord(int local) {
    return local == 0 || local == (Voxel::Chunk::SIZE - 1);
}

} // namespace

bool isDebugBlockCatalogEnabled(const char* envValue) {
    return envValue != nullptr && envValue[0] != '\0' && envValue[0] != '0';
}

bool shouldLoadWorldFromDisk(bool debugBlockCatalogEnabled) {
    return !debugBlockCatalogEnabled;
}

bool shouldCreateChunkLoader(bool debugBlockCatalogEnabled) {
    return !debugBlockCatalogEnabled;
}

bool shouldWireVoxelPersistenceSource(bool debugBlockCatalogEnabled) {
    return !debugBlockCatalogEnabled;
}

bool shouldSaveWorldToDisk(bool debugBlockCatalogEnabled) {
    return !debugBlockCatalogEnabled;
}

bool shouldHandleBlockEdits(bool debugBlockCatalogEnabled) {
    return !debugBlockCatalogEnabled;
}

bool shouldHandleDemoSpawn(bool debugBlockCatalogEnabled) {
    return !debugBlockCatalogEnabled;
}

bool shouldRunWorldStreaming(bool debugBlockCatalogEnabled) {
    return !debugBlockCatalogEnabled;
}

std::vector<Voxel::BlockID> collectDebugBlockCatalogBlockIds(
    const Voxel::BlockRegistry& registry) {
    std::vector<Voxel::BlockID> ids;
    if (registry.size() <= 1) {
        return ids;
    }
    ids.reserve(registry.size() - 1);
    for (std::size_t i = 1; i < registry.size(); ++i) {
        ids.push_back(Voxel::BlockID{static_cast<uint16_t>(i)});
    }
    return ids;
}

DebugBlockCatalogLayout makeDebugBlockCatalogLayout(
    std::size_t blockCount,
    const DebugBlockCatalogOptions& options) {
    DebugBlockCatalogLayout layout;
    layout.blockCount = static_cast<int>(blockCount);
    layout.columns = std::max(1, options.columns);
    layout.spacing = std::max(1, options.spacing);
    layout.baseY = options.baseY;
    layout.originX = options.originX;
    layout.originZ = options.originZ;

    if (blockCount == 0) {
        layout.rows = 0;
        layout.centerX = static_cast<float>(layout.originX);
        layout.centerZ = static_cast<float>(layout.originZ);
        return layout;
    }

    layout.rows = static_cast<int>((blockCount + static_cast<std::size_t>(layout.columns) - 1) /
                                   static_cast<std::size_t>(layout.columns));

    const int widthSpan = (layout.columns - 1) * layout.spacing;
    const int depthSpan = (std::max(1, layout.rows) - 1) * layout.spacing;
    layout.centerX = static_cast<float>(layout.originX) + static_cast<float>(widthSpan) * 0.5f;
    layout.centerZ = static_cast<float>(layout.originZ) + static_cast<float>(depthSpan) * 0.5f;
    return layout;
}

std::vector<DebugBlockCatalogPlacement> makeDebugBlockCatalogPlacements(
    const Voxel::BlockRegistry& registry,
    const DebugBlockCatalogOptions& options) {
    const auto ids = collectDebugBlockCatalogBlockIds(registry);
    const auto layout = makeDebugBlockCatalogLayout(ids.size(), options);

    std::vector<DebugBlockCatalogPlacement> placements;
    placements.reserve(ids.size());

    for (std::size_t index = 0; index < ids.size(); ++index) {
        const int col = static_cast<int>(index % static_cast<std::size_t>(layout.columns));
        const int row = static_cast<int>(index / static_cast<std::size_t>(layout.columns));

        DebugBlockCatalogPlacement placement;
        placement.blockId = ids[index];
        placement.worldX = layout.originX + col * layout.spacing;
        placement.worldY = layout.baseY;
        placement.worldZ = layout.originZ + row * layout.spacing;
        placements.push_back(placement);
    }

    return placements;
}

std::vector<Voxel::ChunkCoord> collectDebugBlockCatalogRemeshChunks(
    const std::vector<DebugBlockCatalogPlacement>& placements) {
    std::unordered_set<Voxel::ChunkCoord, Voxel::ChunkCoordHash> touched;
    touched.reserve(placements.size() * 2);

    for (const auto& placement : placements) {
        Voxel::ChunkCoord coord = Voxel::worldToChunk(placement.worldX,
                                                      placement.worldY,
                                                      placement.worldZ);
        touched.insert(coord);

        int localX = 0;
        int localY = 0;
        int localZ = 0;
        Voxel::worldToLocal(placement.worldX, placement.worldY, placement.worldZ,
                            localX, localY, localZ);

        if (isEdgeLocalCoord(localX)) {
            touched.insert(coord.offset(localX == 0 ? -1 : 1, 0, 0));
        }
        if (isEdgeLocalCoord(localY)) {
            touched.insert(coord.offset(0, localY == 0 ? -1 : 1, 0));
        }
        if (isEdgeLocalCoord(localZ)) {
            touched.insert(coord.offset(0, 0, localZ == 0 ? -1 : 1));
        }
    }

    std::vector<Voxel::ChunkCoord> result;
    result.reserve(touched.size());
    for (const auto& coord : touched) {
        result.push_back(coord);
    }
    std::sort(result.begin(), result.end(), [](const Voxel::ChunkCoord& a, const Voxel::ChunkCoord& b) {
        if (a.x != b.x) {
            return a.x < b.x;
        }
        if (a.y != b.y) {
            return a.y < b.y;
        }
        return a.z < b.z;
    });
    return result;
}

DebugBlockCatalogPlan buildDebugBlockCatalogPlan(
    const Voxel::BlockRegistry& registry,
    const DebugBlockCatalogOptions& options) {
    DebugBlockCatalogPlan plan;
    plan.placements = makeDebugBlockCatalogPlacements(registry, options);
    plan.layout = makeDebugBlockCatalogLayout(plan.placements.size(), options);
    plan.remeshChunks = collectDebugBlockCatalogRemeshChunks(plan.placements);

    const float rowSpan = static_cast<float>(std::max(0, plan.layout.rows - 1) * plan.layout.spacing);
    const float colSpan = static_cast<float>(std::max(0, plan.layout.columns - 1) * plan.layout.spacing);
    const float radius = std::max(8.0f, std::max(rowSpan, colSpan) * 0.75f + 6.0f);

    plan.cameraTarget = glm::vec3(plan.layout.centerX,
                                  static_cast<float>(plan.layout.baseY),
                                  plan.layout.centerZ);
    plan.cameraPosition = glm::vec3(plan.layout.centerX + radius,
                                    static_cast<float>(plan.layout.baseY) + radius * 0.85f,
                                    plan.layout.centerZ + radius);

    return plan;
}

void applyDebugBlockCatalogPlacements(
    Voxel::World& world,
    const std::vector<DebugBlockCatalogPlacement>& placements) {
    for (const auto& placement : placements) {
        Voxel::BlockState state;
        state.id = placement.blockId;
        world.setBlock(placement.worldX, placement.worldY, placement.worldZ, state);
    }
}

} // namespace Rigel::Core
