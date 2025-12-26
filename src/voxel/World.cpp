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
        auto shader = assets.get<Asset::ShaderAsset>("shaders/voxel");
        m_renderer.setShader(shader);
    } catch (const std::exception& e) {
        spdlog::error("Failed to load voxel shader: {}", e.what());
        throw;
    }

    // Connect renderer to texture atlas
    m_renderer.setTextureAtlas(&m_textureAtlas);
    m_streamer.bind(&m_chunkManager, &m_renderer, &m_blockRegistry, &m_textureAtlas, m_generator);

    // Load block definitions and populate texture atlas
    try {
        BlockLoader loader;
        size_t loaded = loader.loadFromManifest(assets, m_blockRegistry, m_textureAtlas);
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
    m_renderer.render(viewProjection, cameraPos);
}

void World::clear() {
    m_chunkManager.clear();
    m_renderer.clear();
    m_streamer.reset();
}

void World::setGenerator(std::shared_ptr<WorldGenerator> generator) {
    m_generator = std::move(generator);
    m_streamer.bind(&m_chunkManager, &m_renderer, &m_blockRegistry, &m_textureAtlas, m_generator);
}

void World::setStreamConfig(const WorldGenConfig::StreamConfig& config) {
    m_streamer.setConfig(config);
    m_renderer.config().renderDistance =
        (static_cast<float>(std::max(0, config.viewDistanceChunks)) + 0.5f) *
        static_cast<float>(Chunk::SIZE);
}

void World::rebuildChunkMesh(ChunkCoord coord) {
    const Chunk* chunk = m_chunkManager.getChunk(coord);
    if (!chunk) {
        return;
    }

    // Skip empty chunks
    auto start = std::chrono::steady_clock::now();
    if (chunk->isEmpty()) {
        m_renderer.removeChunkMesh(coord);
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
    MeshBuilder::BuildContext ctx{
        .chunk = *chunk,
        .registry = m_blockRegistry,
        .atlas = &m_textureAtlas,
        .neighbors = {}
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
        m_renderer.removeChunkMesh(coord);
    } else {
        m_renderer.setChunkMesh(coord, std::move(mesh));
    }

    if (m_benchmark) {
        m_benchmark->addMesh(
            std::chrono::duration<double>(end - start).count(),
            false
        );
    }
}

} // namespace Rigel::Voxel
