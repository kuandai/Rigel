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

glm::vec3 pageWorldMin(const VoxelPageKey& key, int pageSizeVoxels) {
    return glm::vec3(
        static_cast<float>(key.x * pageSizeVoxels),
        static_cast<float>(key.y * pageSizeVoxels),
        static_cast<float>(key.z * pageSizeVoxels)
    );
}

} // namespace

VoxelSvoConfig VoxelSvoLodManager::sanitizeConfig(VoxelSvoConfig config) {
    if (config.nearMeshRadiusChunks < 0) {
        config.nearMeshRadiusChunks = 0;
    }
    if (config.startRadiusChunks < config.nearMeshRadiusChunks) {
        config.startRadiusChunks = config.nearMeshRadiusChunks;
    }
    if (config.maxRadiusChunks < config.startRadiusChunks) {
        config.maxRadiusChunks = config.startRadiusChunks;
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

bool VoxelSvoLodManager::canMeshPage(const VoxelPageKey& key, uint16_t cellSizeVoxels) const {
    for (const glm::ivec3& offset : kNeighborOffsets) {
        VoxelPageKey neighborKey = key;
        neighborKey.x += offset.x;
        neighborKey.y += offset.y;
        neighborKey.z += offset.z;
        auto it = m_pages.find(neighborKey);
        if (it == m_pages.end()) {
            return false;
        }
        const PageRecord& neighbor = it->second;
        if (neighbor.appliedRevision == 0 || neighbor.cpu.dim <= 0) {
            return false;
        }
        if (neighbor.leafMinVoxels != cellSizeVoxels) {
            return false;
        }
    }
    return true;
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
        if (!canMeshPage(key, record.leafMinVoxels)) {
            continue;
        }

        glm::vec3 center = pageWorldMin(key, pageSize) + glm::vec3(static_cast<float>(pageSize) * 0.5f);
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

        const int cellSize = std::max(1, static_cast<int>(center->leafMinVoxels));
        MacroVoxelGrid centerGrid = buildMacroGridFromPage(center->cpu, cellSize);
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
            neighborGrids[i] = buildMacroGridFromPage(neighbor->cpu, cellSize);
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
                              cellSize,
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
            output.mesh = buildSurfaceMeshFromQuads(quads, cellSize, faceLayers);
            m_meshBuildComplete.push(std::move(output));
        });
        center->state = VoxelPageState::Meshing;

        --budget;
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

    const int maxResident = std::max(0, m_config.maxResidentPages);
    if (maxResident == 0) {
        return;
    }

    // Sprint 4 MVP: seed only level 0 pages.
    const int level = 0;
    const int startRadiusVoxels = std::max(0, m_config.startRadiusChunks) * Chunk::SIZE;
    const int maxRadiusVoxels = std::max(0, m_config.maxRadiusChunks) * Chunk::SIZE;

    const glm::ivec3 cameraVoxel = floorToVoxel(cameraPos);
    const int baseX = floorDiv(cameraVoxel.x, pageSize);
    const int baseY = floorDiv(cameraVoxel.y, pageSize);
    const int baseZ = floorDiv(cameraVoxel.z, pageSize);

    auto cubeCount = [](int radius) -> int64_t {
        const int side = radius * 2 + 1;
        return static_cast<int64_t>(side) * side * side;
    };

    int radiusPages = 0;
    while (cubeCount(radiusPages + 1) <= static_cast<int64_t>(maxResident)) {
        ++radiusPages;
    }

    struct Candidate {
        VoxelPageKey key{};
        float distanceSq = 0.0f;
    };
    std::vector<Candidate> candidates;
    candidates.reserve(static_cast<size_t>(cubeCount(radiusPages)));

    for (int dz = -radiusPages; dz <= radiusPages; ++dz) {
        for (int dy = -radiusPages; dy <= radiusPages; ++dy) {
            for (int dx = -radiusPages; dx <= radiusPages; ++dx) {
                VoxelPageKey key{};
                key.level = level;
                key.x = baseX + dx;
                key.y = baseY + dy;
                key.z = baseZ + dz;

                const glm::vec3 pageCenter = glm::vec3(
                    static_cast<float>(key.x * pageSize + pageSize / 2),
                    static_cast<float>(key.y * pageSize + pageSize / 2),
                    static_cast<float>(key.z * pageSize + pageSize / 2)
                );
                const glm::vec3 delta = pageCenter - glm::vec3(cameraVoxel);
                const float distSq = delta.x * delta.x + delta.y * delta.y + delta.z * delta.z;
                const float dist = std::sqrt(std::max(0.0f, distSq));
                if (dist < static_cast<float>(startRadiusVoxels)) {
                    continue;
                }
                if (maxRadiusVoxels > 0 && dist > static_cast<float>(maxRadiusVoxels)) {
                    continue;
                }

                candidates.push_back(Candidate{key, distSq});
            }
        }
    }

    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        return a.distanceSq < b.distanceSq;
    });

    for (const Candidate& candidate : candidates) {
        auto it = m_pages.find(candidate.key);
        if (it == m_pages.end()) {
            PageRecord record{};
            record.key = candidate.key;
            record.state = VoxelPageState::Missing;
            record.desiredRevision = 1;
            record.lastTouchedFrame = m_frameCounter;
            record.leafMinVoxels = static_cast<uint16_t>(std::max(1, m_config.minLeafVoxels));
            m_pages.emplace(candidate.key, std::move(record));
        } else {
            it->second.lastTouchedFrame = m_frameCounter;
        }

        PageRecord& record = m_pages[candidate.key];
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
    const int minLeaf = std::max(1, m_config.minLeafVoxels);
    const BlockRegistry* registry = m_registry;
    GeneratorSource::ChunkGenerateCallback generator = m_chunkGenerator;
    std::shared_ptr<const IVoxelSource> persistenceSource = m_persistenceSource;
    std::shared_ptr<std::atomic_bool> cancel = record->cancel;

    BrickSampleDesc desc;
    desc.worldMinVoxel = glm::ivec3(key.x * pageSize, key.y * pageSize, key.z * pageSize);
    desc.brickDimsVoxels = glm::ivec3(pageSize);
    desc.stepVoxels = 1;

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
    const int maxResident = std::max(0, m_config.maxResidentPages);
    if (maxResident <= 0 || m_pages.size() <= static_cast<size_t>(maxResident)) {
        return;
    }

    const glm::ivec3 cameraVoxel = floorToVoxel(cameraPos);
    const int pageSize = m_config.pageSizeVoxels;

    struct Entry {
        VoxelPageKey key{};
        float distSq = 0.0f;
    };
    std::vector<Entry> entries;
    entries.reserve(m_pages.size());
    for (const auto& [key, record] : m_pages) {
        (void)record;
        glm::vec3 pageCenter = glm::vec3(
            static_cast<float>(key.x * pageSize + pageSize / 2),
            static_cast<float>(key.y * pageSize + pageSize / 2),
            static_cast<float>(key.z * pageSize + pageSize / 2)
        );
        glm::vec3 delta = pageCenter - glm::vec3(cameraVoxel);
        float distSq = delta.x * delta.x + delta.y * delta.y + delta.z * delta.z;
        entries.push_back(Entry{key, distSq});
    }

    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        return a.distSq < b.distSq;
    });

    while (m_pages.size() > static_cast<size_t>(maxResident) && !entries.empty()) {
        VoxelPageKey key = entries.back().key;
        entries.pop_back();
        auto it = m_pages.find(key);
        if (it == m_pages.end()) {
            continue;
        }
        if (it->second.cancel) {
            it->second.cancel->store(true, std::memory_order_relaxed);
        }
        m_buildQueued.erase(key);
        m_pages.erase(it);
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
    processBuildCompletions();
    processMeshCompletions();
    seedDesiredPages(cameraPos);

    enforcePageLimit(cameraPos);

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
    m_telemetry.readyCpuPagesPerLevel = {};
    m_telemetry.readyCpuNodesPerLevel = {};
    m_telemetry.cpuBytesCurrent = 0;

    for (const auto& [key, record] : m_pages) {
        (void)key;
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
                m_telemetry.cpuBytesCurrent += record.cpu.cpuBytes();
                m_telemetry.cpuBytesCurrent += record.tree.cpuBytes();
                m_telemetry.cpuBytesCurrent += record.mesh.vertices.size() * sizeof(VoxelVertex);
                m_telemetry.cpuBytesCurrent += record.mesh.indices.size() * sizeof(uint32_t);
                if (record.key.level >= 0 && record.key.level < static_cast<int>(m_telemetry.readyCpuPagesPerLevel.size())) {
                    m_telemetry.readyCpuPagesPerLevel[static_cast<size_t>(record.key.level)] += 1;
                    m_telemetry.readyCpuNodesPerLevel[static_cast<size_t>(record.key.level)] += record.tree.nodes.size();
                }
                break;
            case VoxelPageState::ReadyMesh:
                ++m_telemetry.pagesUploaded;
                m_telemetry.cpuBytesCurrent += record.cpu.cpuBytes();
                m_telemetry.cpuBytesCurrent += record.tree.cpuBytes();
                m_telemetry.cpuBytesCurrent += record.mesh.vertices.size() * sizeof(VoxelVertex);
                m_telemetry.cpuBytesCurrent += record.mesh.indices.size() * sizeof(uint32_t);
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
