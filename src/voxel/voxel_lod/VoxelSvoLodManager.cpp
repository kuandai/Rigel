#include "Rigel/Voxel/VoxelLod/VoxelSvoLodManager.h"

#include "Rigel/Voxel/BlockRegistry.h"
#include "Rigel/Voxel/Chunk.h"
#include "Rigel/Voxel/VoxelVertex.h"
#include "Rigel/Voxel/VoxelLod/LoadedChunkSource.h"
#include "Rigel/Voxel/VoxelLod/VoxelSurfaceExtraction.h"
#include "Rigel/Voxel/VoxelLod/VoxelSurfaceMesher.h"
#include "Rigel/Voxel/VoxelLod/VoxelSourceChain.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

namespace Rigel::Voxel {
namespace {

int ceilPow2(int value) {
    int v = std::max(1, value);
    int p = 1;
    while (p < v) {
        p <<= 1;
    }
    return p;
}

int floorDiv(int value, int divisor) {
    // Floor division for negative values (divisor must be positive).
    if (divisor <= 0) {
        return 0;
    }
    int q = value / divisor;
    int r = value % divisor;
    if (r != 0 && ((r < 0) != (divisor < 0))) {
        --q;
    }
    return q;
}

glm::ivec3 floorToVoxel(const glm::vec3& world) {
    return glm::ivec3(
        static_cast<int>(std::floor(world.x)),
        static_cast<int>(std::floor(world.y)),
        static_cast<int>(std::floor(world.z))
    );
}

glm::ivec3 snapToChunkOriginVoxel(const glm::vec3& world) {
    const glm::ivec3 voxel = floorToVoxel(world);
    const ChunkCoord chunk = worldToChunk(voxel.x, voxel.y, voxel.z);
    return glm::ivec3(
        chunk.x * Chunk::SIZE,
        chunk.y * Chunk::SIZE,
        chunk.z * Chunk::SIZE
    );
}

uint64_t clampU64Micros(int64_t value) {
    if (value <= 0) {
        return 0;
    }
    return static_cast<uint64_t>(value);
}

VoxelMaterialClass classifyVoxel(const BlockRegistry* registry, VoxelId id) {
    if (id == kVoxelAir) {
        return VoxelMaterialClass::Air;
    }
    if (!registry) {
        return VoxelMaterialClass::Opaque;
    }
    const BlockType& type = registry->getType(BlockID{static_cast<uint16_t>(id)});
    switch (type.layer) {
        case RenderLayer::Cutout:
            return VoxelMaterialClass::Cutout;
        case RenderLayer::Transparent:
            return VoxelMaterialClass::Transparent;
        default:
            return VoxelMaterialClass::Opaque;
    }
}

constexpr std::array<glm::ivec3, 6> kNeighborOffsets = {
    glm::ivec3(-1, 0, 0),
    glm::ivec3( 1, 0, 0),
    glm::ivec3( 0,-1, 0),
    glm::ivec3( 0, 1, 0),
    glm::ivec3( 0, 0,-1),
    glm::ivec3( 0, 0, 1)
};

int pageScaleForLevel(int level) {
    const int clamped = std::clamp(level, 0, 15);
    return 1 << clamped;
}

bool voxelPageKeyLess(const VoxelPageKey& a, const VoxelPageKey& b) {
    if (a.level != b.level) {
        return a.level < b.level;
    }
    if (a.y != b.y) {
        return a.y < b.y;
    }
    if (a.z != b.z) {
        return a.z < b.z;
    }
    return a.x < b.x;
}

int pageSpanVoxels(const VoxelPageKey& key, int pageSizeVoxels) {
    const int64_t span = static_cast<int64_t>(std::max(1, pageSizeVoxels)) *
        static_cast<int64_t>(pageScaleForLevel(key.level));
    return static_cast<int>(std::clamp<int64_t>(span, 1, std::numeric_limits<int>::max()));
}

glm::vec3 pageWorldMin(const VoxelPageKey& key, int pageSizeVoxels) {
    const int span = pageSpanVoxels(key, pageSizeVoxels);
    return glm::vec3(
        static_cast<float>(key.x * span),
        static_cast<float>(key.y * span),
        static_cast<float>(key.z * span)
    );
}

uint8_t evictionPriority(VoxelPageState state) {
    switch (state) {
        case VoxelPageState::Missing:
            return 0;
        case VoxelPageState::QueuedSample:
            return 1;
        case VoxelPageState::QueuedMesh:
            return 2;
        case VoxelPageState::ReadyCpu:
            return 3;
        case VoxelPageState::ReadyMesh:
            return 4;
        case VoxelPageState::Sampling:
            return 5;
        case VoxelPageState::Meshing:
            return 6;
        default:
            return 7;
    }
}

uint64_t estimateResidentCpuBytesPerPage(int pageSizeVoxels) {
    const int dim = std::max(1, pageSizeVoxels);
    uint64_t l0Cells = static_cast<uint64_t>(dim) * static_cast<uint64_t>(dim) *
        static_cast<uint64_t>(dim);
    uint64_t mipCells = 0;
    int mipDim = dim;
    while (mipDim > 1) {
        mipDim /= 2;
        mipCells += static_cast<uint64_t>(mipDim) * static_cast<uint64_t>(mipDim) *
            static_cast<uint64_t>(mipDim);
    }

    const uint64_t l0Bytes = l0Cells * sizeof(VoxelId);
    const uint64_t mipBytes = mipCells * sizeof(uint32_t);
    const uint64_t base = l0Bytes + mipBytes;
    // Conservative overhead for page tree + mesh metadata during steady-state residency.
    const uint64_t overhead = base / 2;
    return std::max<uint64_t>(1, base + overhead);
}

} // namespace

VoxelSvoLodManager::~VoxelSvoLodManager() {
    reset();
}

uint64_t VoxelSvoLodManager::estimatePageCpuBytes(const PageRecord& record) {
    uint64_t bytes = 0;
    bytes += record.cpu.cpuBytes();
    bytes += record.tree.cpuBytes();
    bytes += record.mesh.vertices.size() * sizeof(VoxelVertex);
    bytes += record.mesh.indices.size() * sizeof(uint32_t);
    return bytes;
}

uint64_t VoxelSvoLodManager::estimatePageGpuBytes(const PageRecord& record) {
    if (record.state != VoxelPageState::ReadyMesh) {
        return 0;
    }
    return record.mesh.vertices.size() * sizeof(VoxelVertex) +
        record.mesh.indices.size() * sizeof(uint32_t);
}

VoxelSvoConfig VoxelSvoLodManager::sanitizeConfig(VoxelSvoConfig config) {
    if (config.nearMeshRadiusChunks < 0) {
        config.nearMeshRadiusChunks = 0;
    }
    if (config.maxRadiusChunks < config.nearMeshRadiusChunks) {
        config.maxRadiusChunks = config.nearMeshRadiusChunks;
    }
    if (config.transitionBandChunks < 0) {
        config.transitionBandChunks = 0;
    }

    if (config.levels < 1) {
        config.levels = 1;
    } else if (config.levels > 16) {
        config.levels = 16;
    }

    config.pageSizeVoxels = ceilPow2(std::max(8, config.pageSizeVoxels));
    config.pageSizeVoxels = std::clamp(config.pageSizeVoxels, 8, 256);

    config.minLeafVoxels = ceilPow2(std::max(1, config.minLeafVoxels));
    if (config.minLeafVoxels > config.pageSizeVoxels) {
        config.minLeafVoxels = config.pageSizeVoxels;
    }

    if (config.buildBudgetPagesPerFrame < 0) {
        config.buildBudgetPagesPerFrame = 0;
    }
    if (config.applyBudgetPagesPerFrame < 0) {
        config.applyBudgetPagesPerFrame = 0;
    }
    if (config.uploadBudgetPagesPerFrame < 0) {
        config.uploadBudgetPagesPerFrame = 0;
    }
    if (config.maxResidentPages < 0) {
        config.maxResidentPages = 0;
    }
    if (config.maxCpuBytes < 0) {
        config.maxCpuBytes = 0;
    }
    if (config.maxGpuBytes < 0) {
        config.maxGpuBytes = 0;
    }

    return config;
}

void VoxelSvoLodManager::setConfig(const VoxelSvoConfig& config) {
    m_config = sanitizeConfig(config);
}

void VoxelSvoLodManager::setBuildThreads(size_t threadCount) {
    m_buildThreads = std::max<size_t>(1, threadCount);
    if (m_buildPool) {
        m_buildPool->stop();
        m_buildPool.reset();
    }
}

void VoxelSvoLodManager::setChunkGenerator(GeneratorSource::ChunkGenerateCallback generator) {
    m_chunkGenerator = std::move(generator);
}

void VoxelSvoLodManager::setPersistenceSource(std::shared_ptr<const IVoxelSource> source) {
    m_persistenceSource = std::move(source);
}

void VoxelSvoLodManager::invalidateChunk(ChunkCoord coord) {
    if (m_persistenceSource) {
        m_persistenceSource->invalidateChunk(coord);
    }

    const int pageSize = std::max(1, m_config.pageSizeVoxels);
    const int worldMinX = coord.x * Chunk::SIZE;
    const int worldMinY = coord.y * Chunk::SIZE;
    const int worldMinZ = coord.z * Chunk::SIZE;
    const int worldMaxX = worldMinX + Chunk::SIZE - 1;
    const int worldMaxY = worldMinY + Chunk::SIZE - 1;
    const int worldMaxZ = worldMinZ + Chunk::SIZE - 1;

    const int px0 = floorDiv(worldMinX, pageSize);
    const int py0 = floorDiv(worldMinY, pageSize);
    const int pz0 = floorDiv(worldMinZ, pageSize);
    const int px1 = floorDiv(worldMaxX, pageSize);
    const int py1 = floorDiv(worldMaxY, pageSize);
    const int pz1 = floorDiv(worldMaxZ, pageSize);

    for (int pz = pz0; pz <= pz1; ++pz) {
        for (int py = py0; py <= py1; ++py) {
            for (int px = px0; px <= px1; ++px) {
                VoxelPageKey key{0, px, py, pz};
                auto it = m_pages.find(key);
                if (it == m_pages.end()) {
                    continue;
                }

                PageRecord& record = it->second;
                uint64_t nextRevision = std::max(record.desiredRevision, record.appliedRevision);
                record.desiredRevision = nextRevision + 1;
                record.lastTouchedFrame = m_frameCounter;
                record.state = VoxelPageState::QueuedSample;
                record.meshQueued = false;
                record.meshQueuedRevision = 0;
                record.queuedRevision = 0;
                if (record.cancel) {
                    record.cancel->store(true, std::memory_order_relaxed);
                    record.cancel.reset();
                }

                if (m_buildQueued.insert(key).second) {
                    m_buildQueue.push_front(key);
                }
            }
        }
    }
}

void VoxelSvoLodManager::bind(const ChunkManager* chunkManager,
                              const BlockRegistry* registry,
                              const TextureAtlas* atlas) {
    m_chunkManager = chunkManager;
    m_registry = registry;
    m_atlas = atlas;
    rebuildFaceTextureLayers();
}

void VoxelSvoLodManager::initialize() {
    ensureBuildPool();
    m_initialized = true;
}

void VoxelSvoLodManager::ensureBuildPool() {
    if (m_buildPool) {
        return;
    }
    m_buildPool = std::make_unique<detail::ThreadPool>(m_buildThreads);
}

VoxelSvoLodManager::PageRecord* VoxelSvoLodManager::findPage(const VoxelPageKey& key) {
    auto it = m_pages.find(key);
    if (it == m_pages.end()) {
        return nullptr;
    }
    return &it->second;
}

const VoxelSvoLodManager::PageRecord* VoxelSvoLodManager::findPage(const VoxelPageKey& key) const {
    auto it = m_pages.find(key);
    if (it == m_pages.end()) {
        return nullptr;
    }
    return &it->second;
}

void VoxelSvoLodManager::rebuildFaceTextureLayers() {
    m_faceTextureLayers.clear();
    if (!m_registry) {
        return;
    }

    m_faceTextureLayers.resize(m_registry->size());
    for (size_t i = 0; i < m_faceTextureLayers.size(); ++i) {
        m_faceTextureLayers[i].fill(0);
    }

    if (!m_atlas) {
        return;
    }

    for (size_t id = 0; id < m_faceTextureLayers.size(); ++id) {
        const BlockType& type = m_registry->getType(BlockID{static_cast<uint16_t>(id)});
        auto& layers = m_faceTextureLayers[id];
        for (size_t face = 0; face < DirectionCount; ++face) {
            const Direction dir = static_cast<Direction>(face);
            const std::string& texture = type.textures.forFace(dir);
            if (texture.empty()) {
                layers[face] = 0;
                continue;
            }
            TextureHandle handle = m_atlas->findTexture(texture);
            if (!handle.isValid()) {
                layers[face] = 0;
                continue;
            }
            const int layer = m_atlas->getLayer(handle);
            layers[face] = static_cast<uint16_t>(std::clamp(layer, 0, 65535));
        }
    }
}

void VoxelSvoLodManager::processBuildCompletions() {
    PageBuildOutput output;
    while (m_buildComplete.tryPop(output)) {
        PageRecord* record = findPage(output.key);
        if (!record) {
            continue;
        }
        if (record->queuedRevision != output.revision) {
            continue;
        }
        if (record->cancel && record->cancel->load(std::memory_order_relaxed)) {
            record->state = VoxelPageState::Missing;
            record->cancel.reset();
            record->queuedRevision = 0;
            continue;
        }
        if (output.sampleStatus == BrickSampleStatus::Cancelled) {
            record->state = VoxelPageState::Missing;
            record->cancel.reset();
            record->queuedRevision = 0;
            continue;
        }

        record->cpu = std::move(output.cpu);
        record->tree = std::move(output.tree);
        record->nodeCount = static_cast<uint32_t>(record->tree.nodes.size());
        record->leafMinVoxels = output.leafMinVoxels;
        record->appliedRevision = output.revision;
        record->cancel.reset();
        record->meshQueued = false;
        record->meshQueuedRevision = 0;
        if (record->meshRevision != output.revision) {
            record->mesh = ChunkMesh{};
            record->meshRevision = 0;
            record->state = VoxelPageState::ReadyCpu;
        } else {
            record->state = VoxelPageState::ReadyMesh;
        }

        // Lifetime sampling counters.
        if (output.sampledVoxels > 0) {
            ++m_telemetry.bricksSampled;
            m_telemetry.voxelsSampled += output.sampledVoxels;
            m_telemetry.loadedHits += output.loadedHits;
            m_telemetry.persistenceHits += output.persistenceHits;
            m_telemetry.generatorHits += output.generatorHits;
        }

        // Per-update mip timing (accumulated across applied pages).
        m_telemetry.mipBuildMicros += output.mipBuildMicros;
    }
}

bool VoxelSvoLodManager::canMeshPage(const VoxelPageKey& key,
                                     uint16_t cellSizeVoxels,
                                     bool* outMissingNeighbors,
                                     bool* outLeafMismatch) const {
    bool missingNeighbors = false;
    bool leafMismatch = false;
    for (const glm::ivec3& offset : kNeighborOffsets) {
        VoxelPageKey neighborKey = key;
        neighborKey.x += offset.x;
        neighborKey.y += offset.y;
        neighborKey.z += offset.z;
        auto it = m_pages.find(neighborKey);
        if (it == m_pages.end()) {
            missingNeighbors = true;
            continue;
        }
        const PageRecord& neighbor = it->second;
        if (neighbor.appliedRevision == 0 || neighbor.cpu.dim <= 0) {
            missingNeighbors = true;
            continue;
        }
        if (neighbor.leafMinVoxels != cellSizeVoxels) {
            leafMismatch = true;
        }
    }
    if (outMissingNeighbors) {
        *outMissingNeighbors = missingNeighbors;
    }
    if (outLeafMismatch) {
        *outLeafMismatch = leafMismatch;
    }
    return !missingNeighbors && !leafMismatch;
}

void VoxelSvoLodManager::enqueueMeshBuilds() {
    int budget = std::max(0, m_config.applyBudgetPagesPerFrame);
    if (budget == 0) {
        return;
    }

    struct Candidate {
        VoxelPageKey key{};
        float distanceSq = 0.0f;
    };
    std::vector<Candidate> candidates;
    candidates.reserve(m_pages.size());

    const int pageSize = std::max(1, m_config.pageSizeVoxels);
    for (const auto& [key, record] : m_pages) {
        if (!record.desiredVisible) {
            continue;
        }
        if (record.state != VoxelPageState::ReadyCpu) {
            continue;
        }
        if (record.appliedRevision == 0) {
            continue;
        }
        if (record.meshQueued) {
            continue;
        }
        if (record.meshRevision == record.appliedRevision) {
            continue;
        }
        bool missingNeighbors = false;
        bool leafMismatch = false;
        if (!canMeshPage(key, record.leafMinVoxels, &missingNeighbors, &leafMismatch)) {
            if (missingNeighbors) {
                ++m_telemetry.meshBlockedMissingNeighbors;
            }
            if (leafMismatch) {
                ++m_telemetry.meshBlockedLeafMismatch;
            }
            queueMissingNeighborsForMesh(key);
            continue;
        }

        const int span = pageSpanVoxels(key, pageSize);
        glm::vec3 center = pageWorldMin(key, pageSize) + glm::vec3(static_cast<float>(span) * 0.5f);
        glm::vec3 delta = center - m_lastCameraPos;
        candidates.push_back(Candidate{key, glm::dot(delta, delta)});
    }

    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        return a.distanceSq < b.distanceSq;
    });

    for (const Candidate& candidate : candidates) {
        if (budget <= 0) {
            break;
        }

        PageRecord* center = findPage(candidate.key);
        if (!center) {
            continue;
        }
        if (center->meshQueued || center->meshRevision == center->appliedRevision) {
            continue;
        }

        const int levelScale = pageScaleForLevel(candidate.key.level);
        const int sampleCellSize = std::max(1, static_cast<int>(center->leafMinVoxels));
        const int worldCellSize = sampleCellSize * levelScale;
        MacroVoxelGrid centerGrid = buildMacroGridFromPage(center->cpu, sampleCellSize);
        if (centerGrid.empty()) {
            center->mesh = ChunkMesh{};
            center->meshRevision = center->appliedRevision;
            center->state = VoxelPageState::ReadyMesh;
            continue;
        }

        std::array<MacroVoxelGrid, 6> neighborGrids;
        bool validNeighbors = true;
        for (size_t i = 0; i < kNeighborOffsets.size(); ++i) {
            VoxelPageKey neighborKey = candidate.key;
            neighborKey.x += kNeighborOffsets[i].x;
            neighborKey.y += kNeighborOffsets[i].y;
            neighborKey.z += kNeighborOffsets[i].z;
            PageRecord* neighbor = findPage(neighborKey);
            if (!neighbor || neighbor->cpu.dim <= 0 || neighbor->leafMinVoxels != center->leafMinVoxels) {
                validNeighbors = false;
                break;
            }
            neighborGrids[i] = buildMacroGridFromPage(neighbor->cpu, sampleCellSize);
            if (neighborGrids[i].empty()) {
                validNeighbors = false;
                break;
            }
        }
        if (!validNeighbors) {
            continue;
        }

        VoxelPageKey key = candidate.key;
        const uint64_t revision = center->appliedRevision;
        center->meshQueued = true;
        center->meshQueuedRevision = revision;
        center->state = VoxelPageState::QueuedMesh;

        const auto faceLayers = m_faceTextureLayers;

        m_buildPool->enqueue([this,
                              key,
                              revision,
                              centerGrid = std::move(centerGrid),
                              neighborNegX = std::move(neighborGrids[0]),
                              neighborPosX = std::move(neighborGrids[1]),
                              neighborNegY = std::move(neighborGrids[2]),
                              neighborPosY = std::move(neighborGrids[3]),
                              neighborNegZ = std::move(neighborGrids[4]),
                              neighborPosZ = std::move(neighborGrids[5]),
                              worldCellSize,
                              faceLayers = std::move(faceLayers)]() mutable {
            MeshBuildOutput output{};
            output.key = key;
            output.revision = revision;

            const MacroVoxelNeighbors workerNeighbors{
                .negX = &neighborNegX,
                .posX = &neighborPosX,
                .negY = &neighborNegY,
                .posY = &neighborPosY,
                .negZ = &neighborNegZ,
                .posZ = &neighborPosZ
            };

            std::vector<SurfaceQuad> quads;
            extractSurfaceQuadsGreedy(centerGrid, workerNeighbors, VoxelBoundaryPolicy::OutsideSolid, quads);
            output.mesh = buildSurfaceMeshFromQuads(quads, worldCellSize, faceLayers);
            m_meshBuildComplete.push(std::move(output));
        });
        center->state = VoxelPageState::Meshing;

        --budget;
    }
}

void VoxelSvoLodManager::queueMissingNeighborsForMesh(const VoxelPageKey& key) {
    for (const glm::ivec3& offset : kNeighborOffsets) {
        VoxelPageKey neighborKey = key;
        neighborKey.x += offset.x;
        neighborKey.y += offset.y;
        neighborKey.z += offset.z;

        auto it = m_pages.find(neighborKey);
        if (it == m_pages.end()) {
            PageRecord record{};
            record.key = neighborKey;
            record.state = VoxelPageState::Missing;
            record.desiredRevision = 1;
            record.lastTouchedFrame = m_frameCounter;
            record.lastBuildFrame = m_frameCounter;
            record.leafMinVoxels = static_cast<uint16_t>(std::max(1, m_config.minLeafVoxels));
            record.desiredBuild = true;
            it = m_pages.emplace(neighborKey, std::move(record)).first;
        } else {
            it->second.desiredBuild = true;
            it->second.lastTouchedFrame = m_frameCounter;
            it->second.lastBuildFrame = m_frameCounter;
        }

        PageRecord& record = it->second;
        if (record.state == VoxelPageState::Missing &&
            m_buildQueued.find(neighborKey) == m_buildQueued.end()) {
            record.state = VoxelPageState::QueuedSample;
            m_buildQueue.push_front(neighborKey);
            m_buildQueued.insert(neighborKey);
        }
    }
}

void VoxelSvoLodManager::processMeshCompletions() {
    MeshBuildOutput output;
    while (m_meshBuildComplete.tryPop(output)) {
        PageRecord* record = findPage(output.key);
        if (!record) {
            continue;
        }
        if (!record->meshQueued) {
            continue;
        }
        if (record->meshQueuedRevision != output.revision) {
            continue;
        }
        record->meshQueued = false;
        record->meshQueuedRevision = 0;
        record->meshRevision = output.revision;
        record->mesh = std::move(output.mesh);
        record->state = VoxelPageState::ReadyMesh;
    }
}

void VoxelSvoLodManager::seedDesiredPages(const glm::vec3& cameraPos) {
    if (!m_config.enabled) {
        return;
    }

    const int pageSize = m_config.pageSizeVoxels;
    if (pageSize <= 0) {
        return;
    }

    size_t residentBudget = static_cast<size_t>(std::max(0, m_config.maxResidentPages));
    if (residentBudget == 0) {
        return;
    }
    if (m_config.maxCpuBytes > 0) {
        const uint64_t perPageEstimate = estimateResidentCpuBytesPerPage(pageSize);
        const size_t cpuBudgetPages = static_cast<size_t>(
            static_cast<uint64_t>(m_config.maxCpuBytes) / perPageEstimate);
        residentBudget = std::min(residentBudget, std::max<size_t>(1, cpuBudgetPages));
    }
    if (residentBudget == 0) {
        return;
    }
    const int maxResident = static_cast<int>(
        std::min<size_t>(residentBudget, static_cast<size_t>(std::numeric_limits<int>::max())));

    int maxRadiusVoxels = std::max(0, m_config.maxRadiusChunks) * Chunk::SIZE;
    const int levelCount = std::clamp(m_config.levels, 1, 16);
    if (levelCount > 1 && maxRadiusVoxels <= 0) {
        maxRadiusVoxels = std::max(pageSize * 16, pageSize * 32);
    }
    const int l0MaxRadiusVoxels = (levelCount > 1 && maxRadiusVoxels > 0)
        ? std::max(Chunk::SIZE, maxRadiusVoxels / 2)
        : maxRadiusVoxels;

    const glm::ivec3 cameraVoxel = snapToChunkOriginVoxel(cameraPos);

    auto cubeCount = [](int radius) -> int64_t {
        const int side = radius * 2 + 1;
        return static_cast<int64_t>(side) * side * side;
    };

    struct Candidate {
        VoxelPageKey key{};
        float distanceSq = 0.0f;
    };
    std::array<std::vector<Candidate>, 2> levelCandidates;

    auto collectLevelCandidates = [&](int level, int bandStartVoxels, int bandMaxVoxels) {
        if (level < 0 || level >= 2) {
            return;
        }
        if (bandMaxVoxels > 0 && bandStartVoxels >= bandMaxVoxels) {
            return;
        }

        VoxelPageKey levelKey{};
        levelKey.level = level;
        const int levelPageSpan = pageSpanVoxels(levelKey, pageSize);
        const int baseX = floorDiv(cameraVoxel.x, levelPageSpan);
        const int baseY = floorDiv(cameraVoxel.y, levelPageSpan);
        const int baseZ = floorDiv(cameraVoxel.z, levelPageSpan);

        const int radiusPagesFromStart = static_cast<int>(std::ceil(
            static_cast<float>(std::max(0, bandStartVoxels)) / static_cast<float>(levelPageSpan)));
        const int radiusPagesFromMax = std::max(0, static_cast<int>(std::ceil(
            static_cast<float>(std::max(0, bandMaxVoxels)) / static_cast<float>(levelPageSpan))));

        int radiusPagesFromResident = 0;
        while (cubeCount(radiusPagesFromResident + 1) <= static_cast<int64_t>(maxResident)) {
            ++radiusPagesFromResident;
        }
        int radiusPages = std::max(radiusPagesFromResident, radiusPagesFromStart);
        if (bandMaxVoxels > 0) {
            radiusPages = std::min(radiusPages, radiusPagesFromMax);
        }

        std::vector<Candidate>& candidates = levelCandidates[static_cast<size_t>(level)];
        candidates.reserve(candidates.size() + static_cast<size_t>(cubeCount(radiusPages)));

        for (int dz = -radiusPages; dz <= radiusPages; ++dz) {
            for (int dy = -radiusPages; dy <= radiusPages; ++dy) {
                for (int dx = -radiusPages; dx <= radiusPages; ++dx) {
                    VoxelPageKey key{};
                    key.level = level;
                    key.x = baseX + dx;
                    key.y = baseY + dy;
                    key.z = baseZ + dz;

                    const glm::vec3 pageCenter =
                        pageWorldMin(key, pageSize) + glm::vec3(static_cast<float>(levelPageSpan) * 0.5f);
                    const glm::vec3 delta = pageCenter - glm::vec3(cameraVoxel);
                    const float distSq = glm::dot(delta, delta);
                    const float dist = std::sqrt(std::max(0.0f, distSq));
                    if (dist < static_cast<float>(bandStartVoxels)) {
                        continue;
                    }
                    if (bandMaxVoxels > 0 && dist > static_cast<float>(bandMaxVoxels)) {
                        continue;
                    }

                    candidates.push_back(Candidate{key, distSq});
                }
            }
        }

        std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
            if (a.distanceSq != b.distanceSq) {
                return a.distanceSq < b.distanceSq;
            }
            return voxelPageKeyLess(a.key, b.key);
        });
    };

    collectLevelCandidates(0, 0, l0MaxRadiusVoxels > 0 ? l0MaxRadiusVoxels : maxRadiusVoxels);
    if (levelCount > 1) {
        collectLevelCandidates(1, std::max(0, l0MaxRadiusVoxels), maxRadiusVoxels);
    }

    std::unordered_set<VoxelPageKey, VoxelPageKeyHash> desiredVisible;
    std::unordered_set<VoxelPageKey, VoxelPageKeyHash> desiredBuild;
    desiredVisible.reserve(static_cast<size_t>(maxResident));
    desiredBuild.reserve(static_cast<size_t>(maxResident) * 2);

    const size_t residentBudgetFinal = static_cast<size_t>(maxResident);
    std::vector<Candidate> visibleCandidates;
    visibleCandidates.reserve(levelCandidates[0].size() + levelCandidates[1].size());
    visibleCandidates.insert(visibleCandidates.end(),
                             levelCandidates[0].begin(),
                             levelCandidates[0].end());
    visibleCandidates.insert(visibleCandidates.end(),
                             levelCandidates[1].begin(),
                             levelCandidates[1].end());
    std::sort(visibleCandidates.begin(), visibleCandidates.end(),
              [](const Candidate& a, const Candidate& b) {
                  if (a.distanceSq != b.distanceSq) {
                      return a.distanceSq < b.distanceSq;
                  }
                  return voxelPageKeyLess(a.key, b.key);
              });

    std::unordered_set<VoxelPageKey, VoxelPageKeyHash> closureCritical;
    closureCritical.reserve(residentBudgetFinal * 2);

    auto tryInsertVisibleWithClosure = [&](const VoxelPageKey& visibleKey) {
        if (desiredVisible.find(visibleKey) != desiredVisible.end()) {
            return true;
        }
        const bool alreadyInBuild = desiredBuild.find(visibleKey) != desiredBuild.end();

        std::array<VoxelPageKey, 6> neighbors{};
        size_t newEntries = alreadyInBuild ? 0u : 1u; // visible page itself (if not already closure)
        for (size_t i = 0; i < kNeighborOffsets.size(); ++i) {
            VoxelPageKey neighbor = visibleKey;
            neighbor.x += kNeighborOffsets[i].x;
            neighbor.y += kNeighborOffsets[i].y;
            neighbor.z += kNeighborOffsets[i].z;
            neighbors[i] = neighbor;
            if (desiredBuild.find(neighbor) == desiredBuild.end() &&
                desiredVisible.find(neighbor) == desiredVisible.end()) {
                ++newEntries;
            }
        }

        if (desiredBuild.size() + newEntries > residentBudgetFinal) {
            return false;
        }

        desiredVisible.insert(visibleKey);
        desiredBuild.insert(visibleKey);
        for (const VoxelPageKey& neighbor : neighbors) {
            if (desiredVisible.find(neighbor) == desiredVisible.end()) {
                desiredBuild.insert(neighbor);
                closureCritical.insert(neighbor);
            }
        }
        return true;
    };

    if (residentBudgetFinal < (kNeighborOffsets.size() + 1)) {
        const size_t selected = std::min(residentBudgetFinal, visibleCandidates.size());
        for (size_t i = 0; i < selected; ++i) {
            desiredVisible.insert(visibleCandidates[i].key);
            desiredBuild.insert(visibleCandidates[i].key);
        }
    } else {
        if (levelCount > 1) {
            size_t l0Index = 0;
            size_t l1Index = 0;
            const auto& l0 = levelCandidates[0];
            const auto& l1 = levelCandidates[1];

            while (desiredBuild.size() < residentBudgetFinal &&
                   (l0Index < l0.size() || l1Index < l1.size())) {
                if (l0Index < l0.size()) {
                    (void)tryInsertVisibleWithClosure(l0[l0Index].key);
                    ++l0Index;
                }
                if (desiredBuild.size() >= residentBudgetFinal) {
                    break;
                }
                if (l1Index < l1.size()) {
                    (void)tryInsertVisibleWithClosure(l1[l1Index].key);
                    ++l1Index;
                }
            }
        } else {
            for (const Candidate& candidate : visibleCandidates) {
                (void)tryInsertVisibleWithClosure(candidate.key);
                if (desiredBuild.size() >= residentBudgetFinal) {
                    break;
                }
            }
        }
    }

    for (auto& [key, record] : m_pages) {
        (void)key;
        record.desiredVisible = false;
        record.desiredBuild = false;
    }

    auto distanceSqFor = [pageSize, cameraVoxel](const VoxelPageKey& key) {
        const int span = pageSpanVoxels(key, pageSize);
        glm::vec3 pageCenter = pageWorldMin(key, pageSize) + glm::vec3(static_cast<float>(span) * 0.5f);
        glm::vec3 delta = pageCenter - glm::vec3(cameraVoxel);
        return glm::dot(delta, delta);
    };

    struct QueueCandidate {
        VoxelPageKey key{};
        int priority = 2;
        float distanceSq = 0.0f;
    };
    std::vector<QueueCandidate> queueCandidates;
    queueCandidates.reserve(desiredBuild.size());
    for (const VoxelPageKey& key : desiredBuild) {
        int priority = 2;
        if (closureCritical.find(key) != closureCritical.end()) {
            priority = 0;
        } else if (desiredVisible.find(key) != desiredVisible.end()) {
            priority = 1;
        }
        queueCandidates.push_back(QueueCandidate{
            key,
            priority,
            distanceSqFor(key)
        });
    }
    std::sort(queueCandidates.begin(), queueCandidates.end(), [](const QueueCandidate& a,
                                                                 const QueueCandidate& b) {
        if (a.distanceSq != b.distanceSq) {
            return a.distanceSq < b.distanceSq;
        }
        if (a.priority != b.priority) {
            return a.priority < b.priority;
        }
        return voxelPageKeyLess(a.key, b.key);
    });

    for (const QueueCandidate& candidate : queueCandidates) {
        auto it = m_pages.find(candidate.key);
        if (it == m_pages.end()) {
            PageRecord record{};
            record.key = candidate.key;
            record.state = VoxelPageState::Missing;
            record.desiredRevision = 1;
            record.lastTouchedFrame = m_frameCounter;
            record.leafMinVoxels = static_cast<uint16_t>(std::max(1, m_config.minLeafVoxels));
            it = m_pages.emplace(candidate.key, std::move(record)).first;
        }

        PageRecord& record = it->second;
        record.lastTouchedFrame = m_frameCounter;
        record.desiredBuild = true;
        record.lastBuildFrame = m_frameCounter;
        if (desiredVisible.find(candidate.key) != desiredVisible.end()) {
            record.desiredVisible = true;
            record.lastVisibleFrame = m_frameCounter;
        }

        if (record.state == VoxelPageState::Missing &&
            m_buildQueued.find(candidate.key) == m_buildQueued.end()) {
            record.state = VoxelPageState::QueuedSample;
            m_buildQueue.push_back(candidate.key);
            m_buildQueued.insert(candidate.key);
        }
    }
}

void VoxelSvoLodManager::enqueueBuild(const VoxelPageKey& key, uint64_t revision) {
    PageRecord* record = findPage(key);
    if (!record) {
        return;
    }
    if (!m_chunkGenerator) {
        return;
    }

    record->state = VoxelPageState::Sampling;
    record->queuedRevision = revision;
    record->cancel = std::make_shared<std::atomic_bool>(false);

    const int pageSize = m_config.pageSizeVoxels;
    const int pageScale = pageScaleForLevel(key.level);
    const int pageSpan = pageSpanVoxels(key, pageSize);
    const int minLeaf = std::max(1, m_config.minLeafVoxels);
    const BlockRegistry* registry = m_registry;
    GeneratorSource::ChunkGenerateCallback generator = m_chunkGenerator;
    std::shared_ptr<const IVoxelSource> persistenceSource = m_persistenceSource;
    std::shared_ptr<std::atomic_bool> cancel = record->cancel;

    BrickSampleDesc desc;
    desc.worldMinVoxel = glm::ivec3(key.x * pageSpan, key.y * pageSpan, key.z * pageSpan);
    desc.brickDimsVoxels = glm::ivec3(pageSpan);
    desc.stepVoxels = pageScale;

    std::vector<LoadedChunkSource::ChunkSnapshot> loadedSnapshots;
    if (m_chunkManager) {
        loadedSnapshots = LoadedChunkSource::snapshotForBrick(*m_chunkManager, desc);
    }

    m_buildPool->enqueue([this,
                          key,
                          revision,
                          pageSize,
                          minLeaf,
                          registry,
                          generator,
                          persistenceSource = std::move(persistenceSource),
                          cancel,
                          desc,
                          loadedSnapshots = std::move(loadedSnapshots)]() {
        PageBuildOutput output{};
        output.key = key;
        output.revision = revision;
        output.leafMinVoxels = static_cast<uint16_t>(std::clamp(minLeaf, 1, 65535));

        if (!cancel || cancel->load(std::memory_order_relaxed)) {
            output.sampleStatus = BrickSampleStatus::Cancelled;
            m_buildComplete.push(std::move(output));
            return;
        }

        std::vector<VoxelId> l0(desc.outVoxelCount(), kVoxelAir);
        LoadedChunkSource loaded(std::move(loadedSnapshots));
        GeneratorSource generated(generator);
        VoxelSourceChain chain;
        chain.setLoaded(&loaded);
        chain.setPersistence(persistenceSource.get());
        chain.setGenerator(&generated);

        output.sampleStatus = chain.sampleBrick(desc, l0, cancel.get());
        output.sampledVoxels = l0.size();
        const auto& chainTelemetry = chain.telemetry();
        output.loadedHits = chainTelemetry.loadedHits * output.sampledVoxels;
        output.persistenceHits = chainTelemetry.persistenceHits * output.sampledVoxels;
        output.generatorHits = chainTelemetry.generatorHits * output.sampledVoxels;

        if (output.sampleStatus == BrickSampleStatus::Cancelled) {
            m_buildComplete.push(std::move(output));
            return;
        }

        const auto mipStart = std::chrono::steady_clock::now();
        output.cpu = buildVoxelPageCpu(key, l0, pageSize);
        const auto mipEnd = std::chrono::steady_clock::now();
        output.mipBuildMicros = clampU64Micros(
            std::chrono::duration_cast<std::chrono::microseconds>(mipEnd - mipStart).count());

        VoxelMaterialClassifier classifier = [registry](VoxelId id) {
            return classifyVoxel(registry, id);
        };
        output.tree = buildVoxelPageTree(output.cpu, minLeaf, classifier);

        m_buildComplete.push(std::move(output));
    });
}

void VoxelSvoLodManager::enforcePageLimit(const glm::vec3& cameraPos) {
    const size_t maxResident = static_cast<size_t>(std::max(0, m_config.maxResidentPages));
    const uint64_t maxCpuBytes = static_cast<uint64_t>(std::max<int64_t>(0, m_config.maxCpuBytes));
    const uint64_t maxGpuBytes = static_cast<uint64_t>(std::max<int64_t>(0, m_config.maxGpuBytes));

    if ((maxResident == 0 && maxCpuBytes == 0 && maxGpuBytes == 0) || m_pages.empty()) {
        return;
    }

    uint64_t totalCpuBytes = 0;
    uint64_t totalGpuBytes = 0;
    for (const auto& [key, record] : m_pages) {
        (void)key;
        totalCpuBytes += estimatePageCpuBytes(record);
        totalGpuBytes += estimatePageGpuBytes(record);
    }

    auto overLimits = [&]() {
        const bool overResident = (maxResident > 0) && (m_pages.size() > maxResident);
        const bool overCpu = (maxCpuBytes > 0) && (totalCpuBytes > maxCpuBytes);
        const bool overGpu = (maxGpuBytes > 0) && (totalGpuBytes > maxGpuBytes);
        return overResident || overCpu || overGpu;
    };

    if (!overLimits()) {
        return;
    }

    const glm::ivec3 cameraVoxel = snapToChunkOriginVoxel(cameraPos);
    const int pageSize = std::max(1, m_config.pageSizeVoxels);

    struct Entry {
        VoxelPageKey key{};
        uint8_t priority = 0;
        bool desiredBuild = false;
        bool desiredVisible = false;
        uint64_t lastTouchedFrame = 0;
        float distSq = 0.0f;
    };
    std::vector<Entry> entries;
    entries.reserve(m_pages.size());
    for (const auto& [key, record] : m_pages) {
        const int span = pageSpanVoxels(key, pageSize);
        glm::vec3 pageCenter = glm::vec3(
            static_cast<float>(key.x * span + span / 2),
            static_cast<float>(key.y * span + span / 2),
            static_cast<float>(key.z * span + span / 2)
        );
        glm::vec3 delta = pageCenter - glm::vec3(cameraVoxel);
        entries.push_back(Entry{
            key,
            evictionPriority(record.state),
            record.desiredBuild,
            record.desiredVisible,
            record.lastTouchedFrame,
            glm::dot(delta, delta)
        });
    }

    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if (a.priority != b.priority) {
            return a.priority < b.priority;
        }
        if (a.desiredBuild != b.desiredBuild) {
            return !a.desiredBuild && b.desiredBuild;
        }
        if (a.lastTouchedFrame != b.lastTouchedFrame) {
            return a.lastTouchedFrame < b.lastTouchedFrame;
        }
        return a.distSq > b.distSq;
    });

    auto accountEviction = [this](VoxelPageState state) {
        switch (state) {
            case VoxelPageState::Missing:
                ++m_telemetry.evictedMissing;
                break;
            case VoxelPageState::QueuedSample:
            case VoxelPageState::QueuedMesh:
            case VoxelPageState::Sampling:
            case VoxelPageState::Meshing:
                ++m_telemetry.evictedQueued;
                break;
            case VoxelPageState::ReadyCpu:
                ++m_telemetry.evictedReadyCpu;
                break;
            case VoxelPageState::ReadyMesh:
                ++m_telemetry.evictedReadyMesh;
                break;
            default:
                break;
        }
    };

    auto evictPass = [&](bool includeDesiredVisible) {
        for (const Entry& entry : entries) {
            if (!overLimits()) {
                break;
            }
            if (!includeDesiredVisible && entry.desiredVisible) {
                continue;
            }

            auto it = m_pages.find(entry.key);
            if (it == m_pages.end()) {
                continue;
            }

            totalCpuBytes -= estimatePageCpuBytes(it->second);
            totalGpuBytes -= estimatePageGpuBytes(it->second);
            if (it->second.cancel) {
                it->second.cancel->store(true, std::memory_order_relaxed);
            }
            accountEviction(it->second.state);
            m_buildQueued.erase(entry.key);
            m_pages.erase(it);
        }
    };

    // Hard guard: do not evict desired-visible pages unless still over budget.
    evictPass(false);
    if (overLimits()) {
        evictPass(true);
    }
}

void VoxelSvoLodManager::update(const glm::vec3& cameraPos) {
    m_lastCameraPos = cameraPos;
    if (!m_config.enabled) {
        m_telemetry.activePages = 0;
        m_telemetry.pagesQueued = 0;
        m_telemetry.pagesBuilding = 0;
        m_telemetry.pagesReadyCpu = 0;
        m_telemetry.pagesUploaded = 0;
        m_telemetry.evictedMissing = 0;
        m_telemetry.evictedQueued = 0;
        m_telemetry.evictedReadyCpu = 0;
        m_telemetry.evictedReadyMesh = 0;
        m_telemetry.meshBlockedMissingNeighbors = 0;
        m_telemetry.meshBlockedLeafMismatch = 0;
        m_telemetry.desiredVisibleCount = 0;
        m_telemetry.desiredBuildCount = 0;
        m_telemetry.visibleReadyMeshCount = 0;
        m_telemetry.readyCpuPagesPerLevel = {};
        m_telemetry.readyCpuNodesPerLevel = {};
        m_telemetry.bricksSampled = 0;
        m_telemetry.voxelsSampled = 0;
        m_telemetry.loadedHits = 0;
        m_telemetry.persistenceHits = 0;
        m_telemetry.generatorHits = 0;
        m_telemetry.mipBuildMicros = 0;
        m_telemetry.cpuBytesCurrent = 0;
        m_telemetry.gpuBytesCurrent = 0;
        return;
    }

    ++m_telemetry.updateCalls;
    ++m_frameCounter;
    ensureBuildPool();

    m_telemetry.mipBuildMicros = 0;
    m_telemetry.evictedMissing = 0;
    m_telemetry.evictedQueued = 0;
    m_telemetry.evictedReadyCpu = 0;
    m_telemetry.evictedReadyMesh = 0;
    m_telemetry.meshBlockedMissingNeighbors = 0;
    m_telemetry.meshBlockedLeafMismatch = 0;
    processBuildCompletions();
    processMeshCompletions();
    seedDesiredPages(cameraPos);

    enforcePageLimit(cameraPos);

    // Rebuild queued-sample order every frame from current desired pages so
    // sampling remains strictly center-out even after camera movement.
    const glm::ivec3 cameraVoxel = snapToChunkOriginVoxel(cameraPos);
    const int pageSize = std::max(1, m_config.pageSizeVoxels);
    struct SampleQueueEntry {
        VoxelPageKey key{};
        int priority = 1;
        float distanceSq = 0.0f;
    };
    std::vector<SampleQueueEntry> sampleQueue;
    sampleQueue.reserve(m_pages.size());
    for (auto& [key, record] : m_pages) {
        if (record.state != VoxelPageState::QueuedSample) {
            continue;
        }
        if (!record.desiredBuild) {
            record.state = VoxelPageState::Missing;
            continue;
        }
        const int span = pageSpanVoxels(key, pageSize);
        const glm::vec3 pageCenter =
            pageWorldMin(key, pageSize) + glm::vec3(static_cast<float>(span) * 0.5f);
        const glm::vec3 delta = pageCenter - glm::vec3(cameraVoxel);
        sampleQueue.push_back(SampleQueueEntry{
            key,
            record.desiredVisible ? 0 : 1,
            glm::dot(delta, delta)
        });
    }
    std::sort(sampleQueue.begin(), sampleQueue.end(), [](const SampleQueueEntry& a,
                                                         const SampleQueueEntry& b) {
        if (a.distanceSq != b.distanceSq) {
            return a.distanceSq < b.distanceSq;
        }
        if (a.priority != b.priority) {
            return a.priority < b.priority;
        }
        return voxelPageKeyLess(a.key, b.key);
    });
    m_buildQueue.clear();
    m_buildQueued.clear();
    m_buildQueue.resize(sampleQueue.size());
    for (size_t i = 0; i < sampleQueue.size(); ++i) {
        m_buildQueue[i] = sampleQueue[i].key;
        m_buildQueued.insert(sampleQueue[i].key);
    }

    // Enqueue new builds (budgeted).
    int budget = std::max(0, m_config.buildBudgetPagesPerFrame);
    while (budget > 0 && !m_buildQueue.empty()) {
        VoxelPageKey key = m_buildQueue.front();
        m_buildQueue.pop_front();
        m_buildQueued.erase(key);
        PageRecord* record = findPage(key);
        if (!record) {
            continue;
        }
        if (record->state != VoxelPageState::QueuedSample) {
            continue;
        }

        enqueueBuild(key, record->desiredRevision);
        --budget;
    }

    enqueueMeshBuilds();

    // Update telemetry (current state).
    m_telemetry.activePages = static_cast<uint32_t>(m_pages.size());
    m_telemetry.pagesQueued = 0;
    m_telemetry.pagesBuilding = 0;
    m_telemetry.pagesReadyCpu = 0;
    m_telemetry.pagesUploaded = 0;
    m_telemetry.desiredVisibleCount = 0;
    m_telemetry.desiredBuildCount = 0;
    m_telemetry.visibleReadyMeshCount = 0;
    m_telemetry.readyCpuPagesPerLevel = {};
    m_telemetry.readyCpuNodesPerLevel = {};
    m_telemetry.cpuBytesCurrent = 0;
    m_telemetry.gpuBytesCurrent = 0;

    for (const auto& [key, record] : m_pages) {
        (void)key;
        if (record.desiredVisible) {
            ++m_telemetry.desiredVisibleCount;
        }
        if (record.desiredBuild) {
            ++m_telemetry.desiredBuildCount;
        }

        switch (record.state) {
            case VoxelPageState::QueuedSample:
            case VoxelPageState::QueuedMesh:
                ++m_telemetry.pagesQueued;
                break;
            case VoxelPageState::Sampling:
            case VoxelPageState::Meshing:
                ++m_telemetry.pagesBuilding;
                break;
            case VoxelPageState::ReadyCpu:
                ++m_telemetry.pagesReadyCpu;
                m_telemetry.cpuBytesCurrent += estimatePageCpuBytes(record);
                m_telemetry.gpuBytesCurrent += estimatePageGpuBytes(record);
                if (record.key.level >= 0 && record.key.level < static_cast<int>(m_telemetry.readyCpuPagesPerLevel.size())) {
                    m_telemetry.readyCpuPagesPerLevel[static_cast<size_t>(record.key.level)] += 1;
                    m_telemetry.readyCpuNodesPerLevel[static_cast<size_t>(record.key.level)] += record.tree.nodes.size();
                }
                break;
            case VoxelPageState::ReadyMesh:
                ++m_telemetry.pagesUploaded;
                if (record.desiredVisible) {
                    ++m_telemetry.visibleReadyMeshCount;
                }
                m_telemetry.cpuBytesCurrent += estimatePageCpuBytes(record);
                m_telemetry.gpuBytesCurrent += estimatePageGpuBytes(record);
                if (record.key.level >= 0 &&
                    record.key.level < static_cast<int>(m_telemetry.readyCpuPagesPerLevel.size())) {
                    m_telemetry.readyCpuPagesPerLevel[static_cast<size_t>(record.key.level)] += 1;
                    m_telemetry.readyCpuNodesPerLevel[static_cast<size_t>(record.key.level)] +=
                        record.tree.nodes.size();
                }
                break;
            default:
                break;
        }
    }

    (void)m_chunkManager;
    (void)m_registry;
}

void VoxelSvoLodManager::uploadRenderResources() {
    if (!m_config.enabled) {
        return;
    }
    ++m_telemetry.uploadCalls;
}

void VoxelSvoLodManager::reset() {
    releaseRenderResources();
    m_lastCameraPos = glm::vec3(0.0f);
    m_telemetry = {};
    for (auto& [key, record] : m_pages) {
        (void)key;
        if (record.cancel) {
            record.cancel->store(true, std::memory_order_relaxed);
        }
    }
    m_pages.clear();
    m_buildQueue.clear();
    m_buildQueued.clear();
    if (m_buildPool) {
        m_buildPool->stop();
        m_buildPool.reset();
    }
    m_frameCounter = 0;
    m_initialized = false;
}

void VoxelSvoLodManager::releaseRenderResources() {
    // Skeleton: no GL resources are owned yet.
}

size_t VoxelSvoLodManager::pageCount() const {
    return m_pages.size();
}

std::optional<VoxelSvoPageInfo> VoxelSvoLodManager::pageInfo(const VoxelPageKey& key) const {
    const PageRecord* record = findPage(key);
    if (!record) {
        return std::nullopt;
    }
    VoxelSvoPageInfo info{};
    info.state = record->state;
    info.desiredRevision = record->desiredRevision;
    info.queuedRevision = record->queuedRevision;
    info.appliedRevision = record->appliedRevision;
    info.nodeCount = record->nodeCount;
    info.leafMinVoxels = record->leafMinVoxels;
    return info;
}

void VoxelSvoLodManager::collectDebugPages(
    std::vector<std::pair<VoxelPageKey, VoxelSvoPageInfo>>& out) const {
    out.clear();
    out.reserve(m_pages.size());
    for (const auto& [key, record] : m_pages) {
        VoxelSvoPageInfo info{};
        info.state = record.state;
        info.desiredRevision = record.desiredRevision;
        info.queuedRevision = record.queuedRevision;
        info.appliedRevision = record.appliedRevision;
        info.nodeCount = record.nodeCount;
        info.leafMinVoxels = record.leafMinVoxels;
        out.push_back({key, info});
    }
}

void VoxelSvoLodManager::collectOpaqueMeshes(std::vector<OpaqueMeshEntry>& out) const {
    out.clear();
    out.reserve(m_pages.size());
    const int pageSize = std::max(1, m_config.pageSizeVoxels);
    for (const auto& [key, record] : m_pages) {
        if (!record.desiredVisible) {
            continue;
        }
        if (record.state != VoxelPageState::ReadyMesh) {
            continue;
        }
        if (record.meshRevision == 0 || record.mesh.isEmpty()) {
            continue;
        }
        const auto& opaque = record.mesh.layers[static_cast<size_t>(RenderLayer::Opaque)];
        if (opaque.isEmpty()) {
            continue;
        }
        OpaqueMeshEntry entry{};
        entry.key = key;
        entry.revision = record.meshRevision;
        entry.worldMin = pageWorldMin(key, pageSize);
        entry.mesh = &record.mesh;
        out.push_back(entry);
    }
}

} // namespace Rigel::Voxel
