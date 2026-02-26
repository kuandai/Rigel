#pragma once

#include "Rigel/Voxel/BlockRegistry.h"
#include "Rigel/Voxel/ChunkCoord.h"
#include "Rigel/Voxel/World.h"

#include <glm/vec3.hpp>

#include <cstddef>
#include <vector>

namespace Rigel::Core {

struct DebugBlockCatalogOptions {
    int columns = 16;
    int spacing = 2;
    int baseY = 64;
    int originX = 0;
    int originZ = 0;
};

struct DebugBlockCatalogPlacement {
    Voxel::BlockID blockId = Voxel::BlockRegistry::airId();
    int worldX = 0;
    int worldY = 0;
    int worldZ = 0;
};

struct DebugBlockCatalogLayout {
    int blockCount = 0;
    int columns = 0;
    int rows = 0;
    int spacing = 0;
    int baseY = 0;
    int originX = 0;
    int originZ = 0;
    float centerX = 0.0f;
    float centerZ = 0.0f;
};

struct DebugBlockCatalogPlan {
    DebugBlockCatalogLayout layout;
    std::vector<DebugBlockCatalogPlacement> placements;
    std::vector<Voxel::ChunkCoord> remeshChunks;
    glm::vec3 cameraPosition{0.0f};
    glm::vec3 cameraTarget{0.0f};
};

bool isDebugBlockCatalogEnabled(const char* envValue);

bool shouldLoadWorldFromDisk(bool debugBlockCatalogEnabled);
bool shouldCreateChunkLoader(bool debugBlockCatalogEnabled);
bool shouldWireVoxelPersistenceSource(bool debugBlockCatalogEnabled);
bool shouldSaveWorldToDisk(bool debugBlockCatalogEnabled);
bool shouldHandleBlockEdits(bool debugBlockCatalogEnabled);
bool shouldHandleDemoSpawn(bool debugBlockCatalogEnabled);
bool shouldRunWorldStreaming(bool debugBlockCatalogEnabled);

std::vector<Voxel::BlockID> collectDebugBlockCatalogBlockIds(
    const Voxel::BlockRegistry& registry);

DebugBlockCatalogLayout makeDebugBlockCatalogLayout(
    std::size_t blockCount,
    const DebugBlockCatalogOptions& options = {}
);

std::vector<DebugBlockCatalogPlacement> makeDebugBlockCatalogPlacements(
    const Voxel::BlockRegistry& registry,
    const DebugBlockCatalogOptions& options = {}
);

std::vector<Voxel::ChunkCoord> collectDebugBlockCatalogRemeshChunks(
    const std::vector<DebugBlockCatalogPlacement>& placements);

DebugBlockCatalogPlan buildDebugBlockCatalogPlan(
    const Voxel::BlockRegistry& registry,
    const DebugBlockCatalogOptions& options = {}
);

void applyDebugBlockCatalogPlacements(
    Voxel::World& world,
    const std::vector<DebugBlockCatalogPlacement>& placements
);

} // namespace Rigel::Core
