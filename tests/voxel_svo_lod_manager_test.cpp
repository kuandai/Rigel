#include "TestFramework.h"

#include "Rigel/Voxel/VoxelLod/VoxelSvoLodManager.h"

#include <atomic>
#include <chrono>
#include <limits>
#include <thread>
#include <unordered_set>

using namespace Rigel::Voxel;

namespace {

class TogglePatternSource final : public IVoxelSource {
public:
    enum class Mode : int {
        AllAir = 0,
        Checkerboard = 1
    };

    void setMode(Mode mode) {
        m_mode.store(static_cast<int>(mode), std::memory_order_relaxed);
    }

    BrickSampleStatus sampleBrick(const BrickSampleDesc& desc,
                                  std::span<VoxelId> out,
                                  const std::atomic_bool* cancel = nullptr) const override {
        if (!desc.isValid() || out.size() != desc.outVoxelCount()) {
            return BrickSampleStatus::Miss;
        }

        const glm::ivec3 dims = desc.outDims();
        const int mode = m_mode.load(std::memory_order_relaxed);
        size_t idx = 0;
        for (int z = 0; z < dims.z; ++z) {
            for (int y = 0; y < dims.y; ++y) {
                for (int x = 0; x < dims.x; ++x) {
                    if (cancel && cancel->load(std::memory_order_relaxed)) {
                        return BrickSampleStatus::Cancelled;
                    }
                    if (mode == static_cast<int>(Mode::AllAir)) {
                        out[idx++] = kVoxelAir;
                        continue;
                    }

                    const int wx = desc.worldMinVoxel.x + x * desc.stepVoxels;
                    const int wy = desc.worldMinVoxel.y + y * desc.stepVoxels;
                    const int wz = desc.worldMinVoxel.z + z * desc.stepVoxels;
                    const bool solid = ((wx ^ wy ^ wz) & 1) != 0;
                    out[idx++] = solid ? static_cast<VoxelId>(5) : kVoxelAir;
                }
            }
        }
        return BrickSampleStatus::Hit;
    }

private:
    std::atomic<int> m_mode{static_cast<int>(Mode::AllAir)};
};

} // namespace

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
    config.maxResidentPages = 7;
    config.buildBudgetPagesPerFrame = 1;
    config.applyBudgetPagesPerFrame = 0;
    manager.setConfig(config);

    manager.initialize();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    bool centerReady = false;
    while (std::chrono::steady_clock::now() < deadline) {
        manager.update(glm::vec3(0.0f));
        auto centerInfo = manager.pageInfo(VoxelPageKey{0, 0, 0, 0});
        if (centerInfo &&
            centerInfo->appliedRevision > 0 &&
            (centerInfo->state == VoxelPageState::ReadyCpu ||
             centerInfo->state == VoxelPageState::ReadyMesh)) {
            centerReady = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    CHECK(manager.telemetry().activePages >= 1u);
    CHECK(manager.telemetry().pagesReadyCpu >= 1u);
    CHECK(manager.telemetry().readyCpuPagesPerLevel[0] >= 1u);
    CHECK(manager.telemetry().readyCpuNodesPerLevel[0] > 0u);

    auto info = manager.pageInfo(VoxelPageKey{0, 0, 0, 0});
    CHECK(centerReady);
    CHECK(info.has_value());
    CHECK(info->state == VoxelPageState::ReadyCpu || info->state == VoxelPageState::ReadyMesh);
    CHECK(info->appliedRevision > 0);
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
    config.maxResidentPages = 512;
    config.buildBudgetPagesPerFrame = 16;
    config.applyBudgetPagesPerFrame = 16;
    manager.setConfig(config);
    manager.initialize();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
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
    CHECK(manager.telemetry().pagesUploaded > 0u);
    auto centerInfo = manager.pageInfo(VoxelPageKey{0, 0, 0, 0});
    CHECK(centerInfo.has_value());
    if (centerInfo) {
        CHECK_EQ(centerInfo->state, VoxelPageState::ReadyMesh);
    }
}

TEST_CASE(VoxelSvoLodManager_EnforcesCpuByteBudget) {
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
    config.maxResidentPages = 64;
    config.buildBudgetPagesPerFrame = 16;
    config.applyBudgetPagesPerFrame = 16;
    config.maxCpuBytes = 0;
    manager.setConfig(config);
    manager.initialize();

    bool builtEnough = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
    while (std::chrono::steady_clock::now() < deadline) {
        manager.update(glm::vec3(0.0f));
        const auto& telemetry = manager.telemetry();
        if (telemetry.activePages >= 6u && telemetry.cpuBytesCurrent > 0u) {
            builtEnough = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(builtEnough);
    CHECK(manager.telemetry().cpuBytesCurrent > 0u);

    VoxelSvoConfig clamped = manager.config();
    clamped.maxCpuBytes = static_cast<int64_t>(std::max<uint64_t>(1u, manager.telemetry().cpuBytesCurrent / 4u));
    manager.setConfig(clamped);

    bool enforced = false;
    deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(600);
    while (std::chrono::steady_clock::now() < deadline) {
        manager.update(glm::vec3(0.0f));
        if (manager.telemetry().cpuBytesCurrent <= static_cast<uint64_t>(clamped.maxCpuBytes)) {
            enforced = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(enforced);
    CHECK(manager.telemetry().cpuBytesCurrent <= static_cast<uint64_t>(clamped.maxCpuBytes));
}

TEST_CASE(VoxelSvoLodManager_EnforcesGpuByteBudget) {
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
    config.maxResidentPages = 64;
    config.buildBudgetPagesPerFrame = 16;
    config.applyBudgetPagesPerFrame = 16;
    config.maxGpuBytes = 0;
    manager.setConfig(config);
    manager.initialize();

    bool builtMeshes = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1200);
    while (std::chrono::steady_clock::now() < deadline) {
        manager.update(glm::vec3(0.0f));
        if (manager.telemetry().gpuBytesCurrent > 0u) {
            builtMeshes = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(builtMeshes);
    CHECK(manager.telemetry().gpuBytesCurrent > 0u);

    VoxelSvoConfig clamped = manager.config();
    clamped.maxGpuBytes = static_cast<int64_t>(std::max<uint64_t>(1u, manager.telemetry().gpuBytesCurrent / 4u));
    manager.setConfig(clamped);

    bool enforced = false;
    deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(600);
    while (std::chrono::steady_clock::now() < deadline) {
        manager.update(glm::vec3(0.0f));
        if (manager.telemetry().gpuBytesCurrent <= static_cast<uint64_t>(clamped.maxGpuBytes)) {
            enforced = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(enforced);
    CHECK(manager.telemetry().gpuBytesCurrent <= static_cast<uint64_t>(clamped.maxGpuBytes));
}

TEST_CASE(VoxelSvoLodManager_InvalidateChunkBumpsRevisionAndRequeuesPage) {
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
    config.maxRadiusChunks = 0;
    config.levels = 1;
    config.pageSizeVoxels = 16;
    config.minLeafVoxels = 4;
    config.maxResidentPages = 7;
    config.buildBudgetPagesPerFrame = 1;
    config.applyBudgetPagesPerFrame = 1;
    manager.setConfig(config);
    manager.initialize();

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(600);
    uint64_t firstRevision = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        manager.update(glm::vec3(0.0f));
        auto info = manager.pageInfo(VoxelPageKey{0, 0, 0, 0});
        if (info && info->appliedRevision > 0) {
            firstRevision = info->appliedRevision;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(firstRevision > 0);

    manager.invalidateChunk(ChunkCoord{0, 0, 0});
    auto queuedInfo = manager.pageInfo(VoxelPageKey{0, 0, 0, 0});
    CHECK(queuedInfo.has_value());
    if (queuedInfo) {
        CHECK_EQ(queuedInfo->state, VoxelPageState::QueuedSample);
    }

    uint64_t rebuiltRevision = 0;
    deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(800);
    while (std::chrono::steady_clock::now() < deadline) {
        manager.update(glm::vec3(0.0f));
        auto info = manager.pageInfo(VoxelPageKey{0, 0, 0, 0});
        if (info && info->appliedRevision > firstRevision) {
            rebuiltRevision = info->appliedRevision;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    CHECK(rebuiltRevision > firstRevision);
}

TEST_CASE(VoxelSvoLodManager_SeedsPagesWhenStartRadiusExceedsResidentCubeExtent) {
    VoxelSvoLodManager manager;
    manager.setBuildThreads(1);

    VoxelSvoConfig config;
    config.enabled = true;
    config.nearMeshRadiusChunks = 8;
    config.startRadiusChunks = 12;
    config.maxRadiusChunks = 64;
    config.levels = 1;
    config.pageSizeVoxels = 64;
    config.minLeafVoxels = 1;
    config.maxResidentPages = 8;
    config.buildBudgetPagesPerFrame = 0; // verify pure seeding, independent of worker execution
    config.applyBudgetPagesPerFrame = 0;
    manager.setConfig(config);
    manager.initialize();

    manager.update(glm::vec3(0.0f));

    const auto& telemetry = manager.telemetry();
    CHECK(telemetry.activePages >= 8u);
    CHECK(telemetry.pagesQueued >= 8u);
}

TEST_CASE(VoxelSvoLodManager_ResidentCapKeepsReadyPagesWhenCameraMoves) {
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
                    outBlocks[static_cast<size_t>(x + y * Chunk::SIZE + z * Chunk::SIZE * Chunk::SIZE)] =
                        state;
                }
            }
        }
    });

    VoxelSvoConfig config;
    config.enabled = true;
    config.nearMeshRadiusChunks = 0;
    config.startRadiusChunks = 1; // ensure >1 desired candidate under low resident cap
    config.maxRadiusChunks = 16;
    config.levels = 1;
    config.pageSizeVoxels = 16;
    config.minLeafVoxels = 4;
    config.maxResidentPages = 2;
    config.buildBudgetPagesPerFrame = 1;
    config.applyBudgetPagesPerFrame = 0;
    manager.setConfig(config);
    manager.initialize();

    VoxelPageKey readyKey{};
    bool foundReady = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(800);
    std::vector<std::pair<VoxelPageKey, VoxelSvoPageInfo>> pages;
    while (std::chrono::steady_clock::now() < deadline) {
        manager.update(glm::vec3(0.0f));
        manager.collectDebugPages(pages);
        for (const auto& [key, info] : pages) {
            if ((info.state == VoxelPageState::ReadyCpu || info.state == VoxelPageState::ReadyMesh) &&
                info.appliedRevision > 0) {
                readyKey = key;
                foundReady = true;
                break;
            }
        }
        if (foundReady) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(foundReady);

    // Move far enough to seed a different desired set while the resident cap is saturated.
    for (int i = 0; i < 6; ++i) {
        manager.update(glm::vec3(1024.0f, 0.0f, 0.0f));
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    std::vector<std::pair<VoxelPageKey, VoxelSvoPageInfo>> movedPages;
    manager.collectDebugPages(movedPages);
    bool hasAnyReady = false;
    for (const auto& [key, info] : movedPages) {
        (void)key;
        if ((info.state == VoxelPageState::ReadyCpu || info.state == VoxelPageState::ReadyMesh) &&
            info.appliedRevision > 0) {
            hasAnyReady = true;
            break;
        }
    }
    CHECK(hasAnyReady);
}

TEST_CASE(VoxelSvoLodManager_DesiredBuildIncludesClosureRingForVisiblePages) {
    VoxelSvoLodManager manager;
    manager.setBuildThreads(1);

    VoxelSvoConfig config;
    config.enabled = true;
    config.nearMeshRadiusChunks = 0;
    config.startRadiusChunks = 0;
    config.maxRadiusChunks = 4;
    config.levels = 1;
    config.pageSizeVoxels = 16;
    config.minLeafVoxels = 4;
    config.maxResidentPages = 1;
    config.buildBudgetPagesPerFrame = 0;
    config.applyBudgetPagesPerFrame = 0;
    manager.setConfig(config);
    manager.initialize();

    manager.update(glm::vec3(0.0f));

    // One visible page + six direct-neighbor closure pages.
    CHECK_EQ(manager.pageCount(), static_cast<size_t>(7));
}

TEST_CASE(VoxelSvoLodManager_OnlyDesiredVisiblePagesAreReturnedForFarDraw) {
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
                    outBlocks[static_cast<size_t>(x + y * Chunk::SIZE + z * Chunk::SIZE * Chunk::SIZE)] =
                        state;
                }
            }
        }
    });

    VoxelSvoConfig config;
    config.enabled = true;
    config.nearMeshRadiusChunks = 0;
    config.startRadiusChunks = 0;
    config.maxRadiusChunks = 8;
    config.levels = 1;
    config.pageSizeVoxels = 16;
    config.minLeafVoxels = 4;
    config.maxResidentPages = 64;
    config.buildBudgetPagesPerFrame = 32;
    config.applyBudgetPagesPerFrame = 32;
    manager.setConfig(config);
    manager.initialize();

    std::vector<VoxelSvoLodManager::OpaqueMeshEntry> initialMeshes;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
    while (std::chrono::steady_clock::now() < deadline) {
        manager.update(glm::vec3(0.0f));
        manager.collectOpaqueMeshes(initialMeshes);
        if (!initialMeshes.empty()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(!initialMeshes.empty());

    std::unordered_set<VoxelPageKey, VoxelPageKeyHash> initialKeys;
    for (const auto& entry : initialMeshes) {
        initialKeys.insert(entry.key);
    }
    CHECK(!initialKeys.empty());

    // Move camera enough to change desired-visible set.
    for (int i = 0; i < 8; ++i) {
        manager.update(glm::vec3(1024.0f, 0.0f, 0.0f));
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    std::vector<VoxelSvoLodManager::OpaqueMeshEntry> movedMeshes;
    manager.collectOpaqueMeshes(movedMeshes);
    for (const auto& entry : movedMeshes) {
        CHECK(initialKeys.find(entry.key) == initialKeys.end());
    }
}

TEST_CASE(VoxelSvoLodManager_VisiblePagesEventuallyReachReadyMeshWhenGeneratorAvailable) {
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
    config.maxRadiusChunks = 8;
    config.levels = 1;
    config.pageSizeVoxels = 16;
    config.minLeafVoxels = 4;
    config.maxResidentPages = 256;
    config.buildBudgetPagesPerFrame = 32;
    config.applyBudgetPagesPerFrame = 32;
    manager.setConfig(config);
    manager.initialize();

    bool reachedVisibleReadyMesh = false;
    std::vector<VoxelSvoLodManager::OpaqueMeshEntry> meshes;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
    while (std::chrono::steady_clock::now() < deadline) {
        manager.update(glm::vec3(0.0f));
        manager.collectOpaqueMeshes(meshes);
        if (manager.telemetry().visibleReadyMeshCount > 0u && !meshes.empty()) {
            reachedVisibleReadyMesh = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    CHECK(reachedVisibleReadyMesh);
    CHECK(manager.telemetry().pagesUploaded > 0u);
    CHECK(manager.telemetry().visibleReadyMeshCount > 0u);
}

TEST_CASE(VoxelSvoLodManager_MovementDoesNotCollapseReadyMeshToZeroUnderResidentCap) {
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
    config.maxRadiusChunks = 8;
    config.levels = 1;
    config.pageSizeVoxels = 16;
    config.minLeafVoxels = 4;
    config.maxResidentPages = 128;
    config.buildBudgetPagesPerFrame = 24;
    config.applyBudgetPagesPerFrame = 24;
    manager.setConfig(config);
    manager.initialize();

    const auto warmupDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2500);
    while (std::chrono::steady_clock::now() < warmupDeadline) {
        manager.update(glm::vec3(0.0f));
        if (manager.telemetry().visibleReadyMeshCount >= 8u) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(manager.telemetry().visibleReadyMeshCount >= 8u);

    uint32_t minVisibleReadyMesh = std::numeric_limits<uint32_t>::max();
    bool sampled = false;
    for (int step = 1; step <= 20; ++step) {
        const glm::vec3 pos(static_cast<float>(step * 8), 0.0f, 0.0f);
        for (int i = 0; i < 3; ++i) {
            manager.update(pos);
            minVisibleReadyMesh = std::min(minVisibleReadyMesh, manager.telemetry().visibleReadyMeshCount);
            sampled = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    CHECK(sampled);
    CHECK(minVisibleReadyMesh > 0u);
}

TEST_CASE(VoxelSvoLodManager_EvictionPrefersNonDesiredAndLowValueStates) {
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
    config.maxRadiusChunks = 8;
    config.levels = 1;
    config.pageSizeVoxels = 16;
    config.minLeafVoxels = 4;
    config.maxResidentPages = 256;
    config.buildBudgetPagesPerFrame = 32;
    config.applyBudgetPagesPerFrame = 32;
    manager.setConfig(config);
    manager.initialize();

    const auto warmupDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2500);
    while (std::chrono::steady_clock::now() < warmupDeadline) {
        manager.update(glm::vec3(0.0f));
        if (manager.telemetry().pagesUploaded >= 4u) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(manager.telemetry().pagesUploaded >= 4u);

    const size_t baselinePages = manager.pageCount();
    CHECK(baselinePages > 0u);

    VoxelSvoConfig pressured = manager.config();
    pressured.maxResidentPages = static_cast<int>(std::min<size_t>(baselinePages, static_cast<size_t>(std::numeric_limits<int>::max())));
    pressured.buildBudgetPagesPerFrame = 0;
    pressured.applyBudgetPagesPerFrame = 0;
    manager.setConfig(pressured);

    manager.update(glm::vec3(2048.0f, 0.0f, 0.0f));

    const auto& telemetry = manager.telemetry();
    CHECK((telemetry.evictedMissing + telemetry.evictedQueued) > 0u);
    CHECK_EQ(telemetry.evictedReadyCpu, 0u);
    CHECK_EQ(telemetry.evictedReadyMesh, 0u);
}

TEST_CASE(VoxelSvoLodManager_ResetCancelsInFlightBuildJobs) {
    VoxelSvoLodManager manager;
    manager.setBuildThreads(1);
    manager.setChunkGenerator([](ChunkCoord,
                                 std::array<BlockState, Chunk::VOLUME>& outBlocks,
                                 const std::atomic_bool* cancel) {
        outBlocks.fill(BlockState{});
        for (int i = 0; i < 100; ++i) {
            if (cancel && cancel->load(std::memory_order_relaxed)) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    VoxelSvoConfig config;
    config.enabled = true;
    config.nearMeshRadiusChunks = 0;
    config.startRadiusChunks = 0;
    config.maxRadiusChunks = 0;
    config.levels = 1;
    config.pageSizeVoxels = 16;
    config.minLeafVoxels = 4;
    config.maxResidentPages = 1;
    config.buildBudgetPagesPerFrame = 1;
    config.applyBudgetPagesPerFrame = 0;
    manager.setConfig(config);
    manager.initialize();

    manager.update(glm::vec3(0.0f)); // schedules a build
    CHECK(manager.pageCount() > 0);

    // Must not crash/hang with an in-flight worker job.
    CHECK_NO_THROW(manager.reset());

    // Manager should be reusable after reset.
    CHECK_NO_THROW(manager.initialize());
    CHECK_NO_THROW(manager.update(glm::vec3(0.0f)));
    CHECK(manager.telemetry().updateCalls > 0u);
}

TEST_CASE(VoxelSvoLodManager_PersistenceSource_InvalidationRebuildsFromUpdatedData) {
    auto source = std::make_shared<TogglePatternSource>();

    VoxelSvoLodManager manager;
    manager.setBuildThreads(1);
    manager.setChunkGenerator([](ChunkCoord,
                                 std::array<BlockState, Chunk::VOLUME>& outBlocks,
                                 const std::atomic_bool*) {
        outBlocks.fill(BlockState{}); // fallback is all-air; persistence source must supply data.
    });
    manager.setPersistenceSource(source);

    VoxelSvoConfig config;
    config.enabled = true;
    config.nearMeshRadiusChunks = 0;
    config.startRadiusChunks = 0;
    config.maxRadiusChunks = 0;
    config.levels = 1;
    config.pageSizeVoxels = 8;
    config.minLeafVoxels = 1;
    config.maxResidentPages = 7;
    config.buildBudgetPagesPerFrame = 1;
    config.applyBudgetPagesPerFrame = 0;
    manager.setConfig(config);
    manager.initialize();

    uint64_t firstRevision = 0;
    uint32_t firstNodeCount = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (std::chrono::steady_clock::now() < deadline) {
        manager.update(glm::vec3(0.0f));
        auto info = manager.pageInfo(VoxelPageKey{0, 0, 0, 0});
        if (info && info->state == VoxelPageState::ReadyCpu && info->appliedRevision > 0) {
            firstRevision = info->appliedRevision;
            firstNodeCount = info->nodeCount;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(firstRevision > 0);
    CHECK_EQ(firstNodeCount, static_cast<uint32_t>(1)); // all-air collapses to one node
    CHECK(manager.telemetry().persistenceHits > 0u);

    source->setMode(TogglePatternSource::Mode::Checkerboard);
    manager.invalidateChunk(ChunkCoord{0, 0, 0});

    uint64_t secondRevision = 0;
    uint32_t secondNodeCount = 0;
    deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(900);
    while (std::chrono::steady_clock::now() < deadline) {
        manager.update(glm::vec3(0.0f));
        auto info = manager.pageInfo(VoxelPageKey{0, 0, 0, 0});
        if (info && info->state == VoxelPageState::ReadyCpu && info->appliedRevision > firstRevision) {
            secondRevision = info->appliedRevision;
            secondNodeCount = info->nodeCount;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    CHECK(secondRevision > firstRevision);
    CHECK(secondNodeCount > firstNodeCount);
}
