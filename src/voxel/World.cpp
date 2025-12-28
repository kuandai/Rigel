#include "Rigel/Voxel/World.h"
#include "Rigel/Voxel/BlockLoader.h"

#include <spdlog/spdlog.h>
#include <algorithm>
#include <chrono>

namespace Rigel::Voxel {

World::World() = default;

void World::initialize(Asset::AssetManager& assets) {
    if (m_initialized) {
        spdlog::warn("World::initialize called multiple times");
        return;
    }

    // Load voxel shader
    try {
        m_shader = assets.get<Asset::ShaderAsset>("shaders/voxel");
    } catch (const std::exception& e) {
        spdlog::error("Failed to load voxel shader: {}", e.what());
        throw;
    }

    m_streamer.bind(&m_chunkManager, &m_meshStore, &m_blockRegistry, &m_textureAtlas, m_generator);

    // Load block definitions and populate texture atlas
    try {
        BlockLoader loader;
        size_t loaded = loader.loadFromManifest(assets, m_blockRegistry, m_textureAtlas);
        m_chunkManager.setRegistry(&m_blockRegistry);
        if (m_textureAtlas.textureCount() > 0) {
            m_textureAtlas.upload();
        }
        spdlog::info("Loaded {} block types (registry size {})", loaded, m_blockRegistry.size());
        spdlog::info("Texture atlas entries: {}", m_textureAtlas.textureCount());
    } catch (const std::exception& e) {
        spdlog::error("Failed to load blocks: {}", e.what());
        throw;
    }

    m_initialized = true;
    spdlog::debug("Voxel world initialized");
}

void World::setBlock(int wx, int wy, int wz, BlockState state) {
    m_chunkManager.setBlock(wx, wy, wz, state);
}

BlockState World::getBlock(int wx, int wy, int wz) const {
    return m_chunkManager.getBlock(wx, wy, wz);
}

void World::updateMeshes() {
    m_streamer.processCompletions();
}

void World::updateStreaming(const glm::vec3& cameraPos) {
    m_streamer.update(cameraPos);
}

void World::setBenchmark(ChunkBenchmarkStats* stats) {
    m_benchmark = stats;
    m_streamer.setBenchmark(stats);
}

void World::render(const glm::mat4& viewProjection, const glm::vec3& cameraPos) {
    WorldRenderContext ctx;
    ctx.meshes = &m_meshStore;
    ctx.atlas = &m_textureAtlas;
    ctx.shader = m_shader;
    ctx.config = m_renderConfig;
    ctx.viewProjection = viewProjection;
    ctx.cameraPos = cameraPos;
    ctx.worldTransform = glm::mat4(1.0f);
    m_renderer.render(ctx);
}

void World::getChunkDebugStates(std::vector<ChunkStreamer::DebugChunkState>& out) const {
    m_streamer.getDebugStates(out);
}

int World::viewDistanceChunks() const {
    return m_streamer.viewDistanceChunks();
}

void World::clear() {
    m_chunkManager.clear();
    m_meshStore.clear();
    m_renderer.clearCache();
    m_streamer.reset();
}

void World::releaseRenderResources() {
    m_renderer.clearCache();
    m_textureAtlas.releaseGPU();
    m_shader = {};
}

void World::setGenerator(std::shared_ptr<WorldGenerator> generator) {
    m_generator = std::move(generator);
    m_streamer.bind(&m_chunkManager, &m_meshStore, &m_blockRegistry, &m_textureAtlas, m_generator);
}

void World::setStreamConfig(const WorldGenConfig::StreamConfig& config) {
    m_streamer.setConfig(config);
    m_renderConfig.renderDistance =
        (static_cast<float>(std::max(0, config.viewDistanceChunks)) + 0.5f) *
        static_cast<float>(Chunk::SIZE);
}

void World::rebuildChunkMesh(ChunkCoord coord) {
    Chunk* chunk = m_chunkManager.getChunk(coord);
    if (!chunk) {
        return;
    }

    // Skip empty chunks
    auto start = std::chrono::steady_clock::now();
    if (chunk->isEmpty()) {
        m_meshStore.remove(coord);
        chunk->clearDirty();
        if (m_benchmark) {
            auto end = std::chrono::steady_clock::now();
            m_benchmark->addMesh(
                std::chrono::duration<double>(end - start).count(),
                true
            );
        }
        return;
    }

    // Build context with neighbors and texture atlas
    std::array<BlockState, MeshBuilder::PaddedVolume> paddedBlocks{};
    std::array<const Chunk*, 27> neighborChunks{};
    auto neighborIndex = [](int dx, int dy, int dz) {
        return (dx + 1) + (dy + 1) * 3 + (dz + 1) * 9;
    };
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                ChunkCoord neighborCoord = coord.offset(dx, dy, dz);
                neighborChunks[neighborIndex(dx, dy, dz)] = m_chunkManager.getChunk(neighborCoord);
            }
        }
    }
    neighborChunks[neighborIndex(0, 0, 0)] = chunk;

    BlockState air;
    for (int pz = 0; pz < MeshBuilder::PaddedSize; ++pz) {
        int lz = pz - 1;
        for (int py = 0; py < MeshBuilder::PaddedSize; ++py) {
            int ly = py - 1;
            for (int px = 0; px < MeshBuilder::PaddedSize; ++px) {
                int lx = px - 1;

                int ox = 0;
                int oy = 0;
                int oz = 0;
                int sx = lx;
                int sy = ly;
                int sz = lz;

                if (sx < 0) {
                    ox = -1;
                    sx += Chunk::SIZE;
                } else if (sx >= Chunk::SIZE) {
                    ox = 1;
                    sx -= Chunk::SIZE;
                }
                if (sy < 0) {
                    oy = -1;
                    sy += Chunk::SIZE;
                } else if (sy >= Chunk::SIZE) {
                    oy = 1;
                    sy -= Chunk::SIZE;
                }
                if (sz < 0) {
                    oz = -1;
                    sz += Chunk::SIZE;
                } else if (sz >= Chunk::SIZE) {
                    oz = 1;
                    sz -= Chunk::SIZE;
                }

                const Chunk* source = neighborChunks[neighborIndex(ox, oy, oz)];
                size_t index = static_cast<size_t>(px)
                    + static_cast<size_t>(py) * MeshBuilder::PaddedSize
                    + static_cast<size_t>(pz) * MeshBuilder::PaddedSize * MeshBuilder::PaddedSize;
                if (source) {
                    paddedBlocks[index] = source->getBlock(sx, sy, sz);
                } else {
                    paddedBlocks[index] = air;
                }
            }
        }
    }

    MeshBuilder::BuildContext ctx{
        .chunk = *chunk,
        .registry = m_blockRegistry,
        .atlas = &m_textureAtlas,
        .neighbors = {},
        .paddedBlocks = &paddedBlocks
    };

    // Get neighbor chunks
    ctx.neighbors[static_cast<size_t>(Direction::PosX)] = m_chunkManager.getChunk(coord.offset(1, 0, 0));
    ctx.neighbors[static_cast<size_t>(Direction::NegX)] = m_chunkManager.getChunk(coord.offset(-1, 0, 0));
    ctx.neighbors[static_cast<size_t>(Direction::PosY)] = m_chunkManager.getChunk(coord.offset(0, 1, 0));
    ctx.neighbors[static_cast<size_t>(Direction::NegY)] = m_chunkManager.getChunk(coord.offset(0, -1, 0));
    ctx.neighbors[static_cast<size_t>(Direction::PosZ)] = m_chunkManager.getChunk(coord.offset(0, 0, 1));
    ctx.neighbors[static_cast<size_t>(Direction::NegZ)] = m_chunkManager.getChunk(coord.offset(0, 0, -1));

    // Build mesh
    ChunkMesh mesh = m_meshBuilder.build(ctx);
    auto end = std::chrono::steady_clock::now();

    if (mesh.isEmpty()) {
        m_meshStore.remove(coord);
    } else {
        m_meshStore.set(coord, std::move(mesh));
    }
    chunk->clearDirty();

    if (m_benchmark) {
        m_benchmark->addMesh(
            std::chrono::duration<double>(end - start).count(),
            false
        );
    }
}

} // namespace Rigel::Voxel
