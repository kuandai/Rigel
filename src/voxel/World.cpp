#include "Rigel/Voxel/World.h"
#include "Rigel/Voxel/WorldResources.h"
#include "Rigel/Persistence/Providers.h"

#include <spdlog/spdlog.h>
#include <cmath>
#include <stdexcept>

namespace Rigel::Voxel {

namespace {

bool overlaps(
    const BlockCollisionBox& first,
    const BlockCollisionBox& second
) {
    for (size_t axis = 0; axis < 3; ++axis) {
        if (first.min[axis] >
                second.max[axis] + BlockCollisionContactTolerance ||
            first.max[axis] <
                second.min[axis] - BlockCollisionContactTolerance) {
            return false;
        }
    }
    return true;
}

BlockCollisionBox translated(
    const BlockCollisionBox& box,
    int x,
    int y,
    int z
) {
    const std::array<float, 3> offset{
        static_cast<float>(x),
        static_cast<float>(y),
        static_cast<float>(z),
    };
    BlockCollisionBox result = box;
    for (size_t axis = 0; axis < 3; ++axis) {
        result.min[axis] += offset[axis];
        result.max[axis] += offset[axis];
    }
    return result;
}

} // namespace

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

void World::forEachCollisionBox(
    const BlockCollisionBox& bounds,
    void* context,
    CollisionBoxCallback callback
) const {
    for (size_t axis = 0; axis < 3; ++axis) {
        if (!std::isfinite(bounds.min[axis]) ||
            !std::isfinite(bounds.max[axis]) ||
            bounds.min[axis] > bounds.max[axis]) {
            return;
        }
    }

    const int minX = static_cast<int>(std::ceil(
        bounds.min[0] - BlockCollisionShape::MaximumCoordinate -
        BlockCollisionContactTolerance));
    const int minY = static_cast<int>(std::ceil(
        bounds.min[1] - BlockCollisionShape::MaximumCoordinate -
        BlockCollisionContactTolerance));
    const int minZ = static_cast<int>(std::ceil(
        bounds.min[2] - BlockCollisionShape::MaximumCoordinate -
        BlockCollisionContactTolerance));
    const int maxX = static_cast<int>(std::floor(
        bounds.max[0] - BlockCollisionShape::MinimumCoordinate +
        BlockCollisionContactTolerance));
    const int maxY = static_cast<int>(std::floor(
        bounds.max[1] - BlockCollisionShape::MinimumCoordinate +
        BlockCollisionContactTolerance));
    const int maxZ = static_cast<int>(std::floor(
        bounds.max[2] - BlockCollisionShape::MinimumCoordinate +
        BlockCollisionContactTolerance));

    const BlockRegistry& registry = blockRegistry();
    for (int x = minX; x <= maxX; ++x) {
        for (int y = minY; y <= maxY; ++y) {
            for (int z = minZ; z <= maxZ; ++z) {
                const BlockState state = getBlock(x, y, z);
                if (state.isAir()) continue;

                for (const BlockCollisionBox& localBox :
                     registry.getType(state.id).collision.boxes()) {
                    const BlockCollisionBox worldBox =
                        translated(localBox, x, y, z);
                    if (overlaps(bounds, worldBox)) {
                        callback(context, worldBox);
                    }
                }
            }
        }
    }
}

void World::setGenerator(std::shared_ptr<const WorldGenerator> generator) {
    if (m_generator) {
        if (!generator ||
            !m_generator->matchesRuntimeGenerator(*generator)) {
            throw std::invalid_argument(
                "World generator cannot change after it is set");
        }
        return;
    }
    m_generator = std::move(generator);
}

void World::tickEntities(float dt) {
    m_entities.tick(dt);
}

} // namespace Rigel::Voxel
