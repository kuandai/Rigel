#include "Rigel/Voxel/World.h"
#include "Rigel/Voxel/WorldResources.h"

#include <spdlog/spdlog.h>
#include <stdexcept>

namespace Rigel::Voxel {

World::World() = default;

World::World(WorldResources& resources) {
    initialize(resources);
}

void World::initialize(WorldResources& resources) {
    if (m_initialized) {
        spdlog::warn("World::initialize called multiple times");
        return;
    }

    m_resources = &resources;
    m_chunkManager.setRegistry(&m_resources->registry());

    m_initialized = true;
    spdlog::debug("Voxel world initialized");
}

BlockRegistry& World::blockRegistry() {
    if (!m_resources) {
        throw std::runtime_error("World resources not initialized");
    }
    return m_resources->registry();
}

const BlockRegistry& World::blockRegistry() const {
    if (!m_resources) {
        throw std::runtime_error("World resources not initialized");
    }
    return m_resources->registry();
}

void World::setBlock(int wx, int wy, int wz, BlockState state) {
    m_chunkManager.setBlock(wx, wy, wz, state);
}

BlockState World::getBlock(int wx, int wy, int wz) const {
    return m_chunkManager.getBlock(wx, wy, wz);
}

void World::clear() {
    m_chunkManager.clear();
}

void World::setGenerator(std::shared_ptr<WorldGenerator> generator) {
    m_generator = std::move(generator);
}

std::vector<uint8_t> World::serializeChunkDelta(ChunkCoord coord) const {
    (void)coord;
    return {};
}

} // namespace Rigel::Voxel
