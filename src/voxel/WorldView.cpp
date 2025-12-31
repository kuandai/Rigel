#include "Rigel/Voxel/WorldView.h"

#include <spdlog/spdlog.h>
#include <algorithm>
#include <chrono>

namespace Rigel::Voxel {

WorldView::WorldView(World& world, WorldResources& resources)
    : m_world(&world)
    , m_resources(&resources)
{}

void WorldView::initialize(Asset::AssetManager& assets) {
    if (m_initialized) {
        spdlog::warn("WorldView::initialize called multiple times");
        return;
    }

    try {
        m_shader = assets.get<Asset::ShaderAsset>("shaders/voxel");
    } catch (const std::exception& e) {
        spdlog::error("Failed to load voxel shader: {}", e.what());
        throw;
    }
    try {
        m_shadowDepthShader = assets.get<Asset::ShaderAsset>("shaders/voxel_shadow_depth");
        m_shadowTransmitShader = assets.get<Asset::ShaderAsset>("shaders/voxel_shadow_transmit");
    } catch (const std::exception& e) {
        spdlog::warn("Shadow shaders unavailable: {}", e.what());
    }

    if (m_world && m_resources) {
        m_streamer.bind(&m_world->chunkManager(),
                        &m_meshStore,
                        &m_resources->registry(),
                        &m_resources->textureAtlas(),
                        m_world->generator());
    }

    m_initialized = true;
}

void WorldView::setGenerator(std::shared_ptr<WorldGenerator> generator) {
    if (!m_world || !m_resources) {
        return;
    }
    m_streamer.bind(&m_world->chunkManager(),
                    &m_meshStore,
                    &m_resources->registry(),
                    &m_resources->textureAtlas(),
                    std::move(generator));
}

void WorldView::setStreamConfig(const WorldGenConfig::StreamConfig& config) {
    m_streamer.setConfig(config);
    m_renderConfig.renderDistance =
        (static_cast<float>(std::max(0, config.viewDistanceChunks)) + 0.5f) *
        static_cast<float>(Chunk::SIZE);
}

void WorldView::setBenchmark(ChunkBenchmarkStats* stats) {
    m_benchmark = stats;
    m_streamer.setBenchmark(stats);
}

void WorldView::updateStreaming(const glm::vec3& cameraPos) {
    m_streamer.update(cameraPos);
}

void WorldView::updateMeshes() {
    m_streamer.processCompletions();
}

void WorldView::render(const glm::mat4& view,
                       const glm::mat4& projection,
                       const glm::vec3& cameraPos,
                       float nearPlane,
                       float farPlane) {
    if (!m_resources) {
        return;
    }

    WorldRenderContext ctx;
    ctx.meshes = &m_meshStore;
    ctx.atlas = &m_resources->textureAtlas();
    ctx.shader = m_shader;
    ctx.shadowDepthShader = m_shadowDepthShader;
    ctx.shadowTransmitShader = m_shadowTransmitShader;
    ctx.config = m_renderConfig;
    ctx.view = view;
    ctx.projection = projection;
    ctx.viewProjection = projection * view;
    ctx.cameraPos = cameraPos;
    ctx.nearPlane = nearPlane;
    ctx.farPlane = farPlane;
    ctx.worldTransform = glm::mat4(1.0f);
    m_renderer.render(ctx);
}

void WorldView::getChunkDebugStates(std::vector<ChunkStreamer::DebugChunkState>& out) const {
    m_streamer.getDebugStates(out);
}

int WorldView::viewDistanceChunks() const {
    return m_streamer.viewDistanceChunks();
}

void WorldView::rebuildChunkMesh(ChunkCoord coord) {
    if (!m_world || !m_resources) {
        return;
    }

    Chunk* chunk = m_world->chunkManager().getChunk(coord);
    if (!chunk) {
        return;
    }

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

    std::array<BlockState, MeshBuilder::PaddedVolume> paddedBlocks{};
    std::array<const Chunk*, 27> neighborChunks{};
    auto neighborIndex = [](int dx, int dy, int dz) {
        return (dx + 1) + (dy + 1) * 3 + (dz + 1) * 9;
    };
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                ChunkCoord neighborCoord = coord.offset(dx, dy, dz);
                neighborChunks[neighborIndex(dx, dy, dz)] =
                    m_world->chunkManager().getChunk(neighborCoord);
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
        .registry = m_resources->registry(),
        .atlas = &m_resources->textureAtlas(),
        .neighbors = {},
        .paddedBlocks = &paddedBlocks
    };

    ctx.neighbors[static_cast<size_t>(Direction::PosX)] =
        m_world->chunkManager().getChunk(coord.offset(1, 0, 0));
    ctx.neighbors[static_cast<size_t>(Direction::NegX)] =
        m_world->chunkManager().getChunk(coord.offset(-1, 0, 0));
    ctx.neighbors[static_cast<size_t>(Direction::PosY)] =
        m_world->chunkManager().getChunk(coord.offset(0, 1, 0));
    ctx.neighbors[static_cast<size_t>(Direction::NegY)] =
        m_world->chunkManager().getChunk(coord.offset(0, -1, 0));
    ctx.neighbors[static_cast<size_t>(Direction::PosZ)] =
        m_world->chunkManager().getChunk(coord.offset(0, 0, 1));
    ctx.neighbors[static_cast<size_t>(Direction::NegZ)] =
        m_world->chunkManager().getChunk(coord.offset(0, 0, -1));

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

void WorldView::applyChunkDelta(ChunkCoord coord, std::span<const uint8_t> payload) {
    (void)payload;
    m_replication.knownChunks.insert(coord);
}

void WorldView::clear() {
    m_meshStore.clear();
    m_renderer.clearCache();
    m_streamer.reset();
    m_replication.knownChunks.clear();
}

void WorldView::releaseRenderResources() {
    m_renderer.releaseResources();
    m_shader = {};
    m_shadowDepthShader = {};
    m_shadowTransmitShader = {};
}

} // namespace Rigel::Voxel
