#include "TestFramework.h"

#include "Rigel/Voxel/VoxelLod/VoxelSvoLodManager.h"

#include <chrono>
#include <thread>

using namespace Rigel::Voxel;

TEST_CASE(VoxelSvoLodManager_ConfigIsSanitized) {
    VoxelSvoLodManager manager;
    VoxelSvoConfig config;
    config.enabled = true;
    config.nearMeshRadiusChunks = -1;
    config.startRadiusChunks = -2;
    config.maxRadiusChunks = -3;
    config.transitionBandChunks = -4;
    config.levels = 0;
    config.pageSizeVoxels = 9;
    config.minLeafVoxels = 7;
    config.buildBudgetPagesPerFrame = -1;
    config.applyBudgetPagesPerFrame = -2;
    config.uploadBudgetPagesPerFrame = -3;
    config.maxResidentPages = -4;
    config.maxCpuBytes = -5;
    config.maxGpuBytes = -6;

    manager.setConfig(config);

    const auto& effective = manager.config();
    CHECK(effective.enabled);
    CHECK_EQ(effective.nearMeshRadiusChunks, 0);
    CHECK_EQ(effective.startRadiusChunks, 0);
    CHECK_EQ(effective.maxRadiusChunks, 0);
    CHECK_EQ(effective.transitionBandChunks, 0);
    CHECK_EQ(effective.levels, 1);
    CHECK_EQ(effective.pageSizeVoxels, 16);
    CHECK_EQ(effective.minLeafVoxels, 8);
    CHECK_EQ(effective.buildBudgetPagesPerFrame, 0);
    CHECK_EQ(effective.applyBudgetPagesPerFrame, 0);
    CHECK_EQ(effective.uploadBudgetPagesPerFrame, 0);
    CHECK_EQ(effective.maxResidentPages, 0);
    CHECK_EQ(effective.maxCpuBytes, 0);
    CHECK_EQ(effective.maxGpuBytes, 0);
}

TEST_CASE(VoxelSvoLodManager_UpdateStaysInertWhenDisabled) {
    VoxelSvoLodManager manager;
    VoxelSvoConfig config;
    config.enabled = false;
    manager.setConfig(config);

    manager.initialize();
    manager.update(glm::vec3(0.0f));
    manager.uploadRenderResources();

    const auto& telemetry = manager.telemetry();
    CHECK_EQ(telemetry.updateCalls, 0u);
    CHECK_EQ(telemetry.uploadCalls, 0u);
    CHECK_EQ(telemetry.bricksSampled, 0u);
    CHECK_EQ(telemetry.voxelsSampled, 0u);
    CHECK_EQ(telemetry.loadedHits, 0u);
    CHECK_EQ(telemetry.persistenceHits, 0u);
    CHECK_EQ(telemetry.generatorHits, 0u);
    CHECK_EQ(telemetry.mipBuildMicros, 0u);
    CHECK_EQ(telemetry.activePages, 0u);
    CHECK_EQ(telemetry.pagesQueued, 0u);
    CHECK_EQ(telemetry.pagesBuilding, 0u);
    CHECK_EQ(telemetry.pagesReadyCpu, 0u);
    CHECK_EQ(telemetry.pagesUploaded, 0u);
    for (size_t i = 0; i < telemetry.readyCpuPagesPerLevel.size(); ++i) {
        CHECK_EQ(telemetry.readyCpuPagesPerLevel[i], 0u);
        CHECK_EQ(telemetry.readyCpuNodesPerLevel[i], 0u);
    }
}

TEST_CASE(VoxelSvoLodManager_ResetAndReinitialize_IsIdempotent) {
    VoxelSvoLodManager manager;
    VoxelSvoConfig config;
    config.enabled = true;
    manager.setConfig(config);

    CHECK_NO_THROW(manager.initialize());
    CHECK_NO_THROW(manager.update(glm::vec3(0.0f)));
    CHECK_NO_THROW(manager.uploadRenderResources());
    CHECK(manager.telemetry().updateCalls > 0u);

    CHECK_NO_THROW(manager.reset());
    CHECK_EQ(manager.telemetry().updateCalls, 0u);

    CHECK_NO_THROW(manager.initialize());
    CHECK_NO_THROW(manager.update(glm::vec3(1.0f)));
    CHECK(manager.telemetry().updateCalls > 0u);
}

TEST_CASE(VoxelSvoLodManager_BuildsSinglePageToReadyCpu) {
    VoxelSvoLodManager manager;
    manager.setBuildThreads(1);
    manager.setChunkGenerator([](ChunkCoord coord,
                                 std::array<BlockState, Chunk::VOLUME>& outBlocks,
                                 const std::atomic_bool* cancel) {
        (void)cancel;
        for (int z = 0; z < Chunk::SIZE; ++z) {
            for (int y = 0; y < Chunk::SIZE; ++y) {
                const int worldY = coord.y * Chunk::SIZE + y;
                for (int x = 0; x < Chunk::SIZE; ++x) {
                    const bool solid = worldY < 8;
                    BlockState state;
                    state.id.type = solid ? 1 : 0;
                    outBlocks[static_cast<size_t>(x + y * Chunk::SIZE + z * Chunk::SIZE * Chunk::SIZE)] = state;
                }
            }
        }
    });

    VoxelSvoConfig config;
    config.enabled = true;
    config.nearMeshRadiusChunks = 0;
    config.startRadiusChunks = 0;
    config.maxRadiusChunks = 0;
    config.levels = 1;
    config.pageSizeVoxels = 8;
    config.minLeafVoxels = 4;
    config.maxResidentPages = 1;
    config.buildBudgetPagesPerFrame = 1;
    manager.setConfig(config);

    manager.initialize();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    while (std::chrono::steady_clock::now() < deadline) {
        manager.update(glm::vec3(0.0f));
        if (manager.telemetry().pagesReadyCpu >= 1u) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    CHECK_EQ(manager.telemetry().activePages, 1u);
    CHECK_EQ(manager.telemetry().pagesReadyCpu, 1u);
    CHECK_EQ(manager.telemetry().readyCpuPagesPerLevel[0], 1u);
    CHECK(manager.telemetry().readyCpuNodesPerLevel[0] > 0u);

    auto info = manager.pageInfo(VoxelPageKey{0, 0, 0, 0});
    CHECK(info.has_value());
    CHECK_EQ(info->state, VoxelPageState::ReadyCpu);
    CHECK(info->nodeCount > 0u);
    CHECK_EQ(info->leafMinVoxels, static_cast<uint16_t>(4));
}

TEST_CASE(VoxelSvoLodManager_BuildsCenterPageMeshWhenNeighborsReady) {
    VoxelSvoLodManager manager;
    manager.setBuildThreads(1);
    manager.setChunkGenerator([](ChunkCoord coord,
                                 std::array<BlockState, Chunk::VOLUME>& outBlocks,
                                 const std::atomic_bool* cancel) {
        (void)cancel;
        for (int z = 0; z < Chunk::SIZE; ++z) {
            for (int y = 0; y < Chunk::SIZE; ++y) {
                const int worldY = coord.y * Chunk::SIZE + y;
                for (int x = 0; x < Chunk::SIZE; ++x) {
                    BlockState state;
                    state.id.type = (worldY < 8) ? 1 : 0;
                    outBlocks[static_cast<size_t>(x + y * Chunk::SIZE + z * Chunk::SIZE * Chunk::SIZE)] = state;
                }
            }
        }
    });

    VoxelSvoConfig config;
    config.enabled = true;
    config.nearMeshRadiusChunks = 0;
    config.startRadiusChunks = 0;
    config.maxRadiusChunks = 2;
    config.levels = 1;
    config.pageSizeVoxels = 16;
    config.minLeafVoxels = 4;
    config.maxResidentPages = 27;
    config.buildBudgetPagesPerFrame = 16;
    config.applyBudgetPagesPerFrame = 16;
    manager.setConfig(config);
    manager.initialize();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(750);
    std::vector<VoxelSvoLodManager::OpaqueMeshEntry> meshes;
    while (std::chrono::steady_clock::now() < deadline) {
        manager.update(glm::vec3(0.0f));
        manager.collectOpaqueMeshes(meshes);
        bool foundCenter = false;
        for (const auto& entry : meshes) {
            if (entry.key == VoxelPageKey{0, 0, 0, 0}) {
                foundCenter = true;
                break;
            }
        }
        if (foundCenter) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    bool foundCenter = false;
    for (const auto& entry : meshes) {
        if (entry.key == VoxelPageKey{0, 0, 0, 0}) {
            foundCenter = true;
            CHECK(entry.mesh != nullptr);
            CHECK(!entry.mesh->isEmpty());
            CHECK(entry.mesh->layers[static_cast<size_t>(RenderLayer::Opaque)].indexCount > 0);
            break;
        }
    }
    CHECK(foundCenter);
}
