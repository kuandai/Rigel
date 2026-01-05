#include "Rigel/Voxel/World.h"
#include "Rigel/Voxel/WorldResources.h"
#include "Rigel/Persistence/Providers.h"

#include <spdlog/spdlog.h>
#include <stdexcept>

namespace Rigel::Voxel {

World::World()
    : m_persistenceProviders(std::make_shared<Persistence::ProviderRegistry>()) {
}

World::World(WorldResources& resources) {
    initialize(resources);
}

World::~World() = default;

void World::initialize(WorldResources& resources) {
    if (m_initialized) {
        spdlog::warn("World::initialize called multiple times");
        return;
    }

    m_resources = &resources;
    m_chunkManager.setRegistry(&m_resources->registry());
    m_entities.bind(this);
    persistenceProviders().add(
        Persistence::kBlockRegistryProviderId,
        std::make_shared<Persistence::BlockRegistryProvider>(&m_resources->registry())
    );

    m_initialized = true;
    spdlog::debug("Voxel world initialized");
}

Persistence::ProviderRegistry& World::persistenceProviders() {
    if (!m_persistenceProviders) {
        m_persistenceProviders = std::make_shared<Persistence::ProviderRegistry>();
    }
    return *m_persistenceProviders;
}

const Persistence::ProviderRegistry& World::persistenceProviders() const {
    if (!m_persistenceProviders) {
        throw std::runtime_error("World persistence providers not initialized");
    }
    return *m_persistenceProviders;
}

std::shared_ptr<Persistence::ProviderRegistry> World::persistenceProvidersHandle() const {
    return m_persistenceProviders;
}

void World::setPersistenceProviders(std::shared_ptr<Persistence::ProviderRegistry> providers) {
    m_persistenceProviders = std::move(providers);
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
    m_entities.clear();
}

void World::setGenerator(std::shared_ptr<WorldGenerator> generator) {
    m_generator = std::move(generator);
}

void World::tickEntities(float dt) {
    m_entities.tick(dt);
}

std::vector<uint8_t> World::serializeChunkDelta(ChunkCoord coord) const {
    (void)coord;
    return {};
}

} // namespace Rigel::Voxel
