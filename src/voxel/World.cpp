#include "Rigel/Voxel/World.h"
#include "Rigel/Voxel/WorldResources.h"
#include "Rigel/Persistence/Providers.h"

#include <spdlog/spdlog.h>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace Rigel::Voxel {

namespace {

constexpr uint64_t kMaximumCollisionQueryCandidates = 65'536;

struct CollisionQueryRange {
    std::array<int, 3> min{};
    std::array<int, 3> max{};
};

bool collisionQueryRange(
    const BlockCollisionBox& bounds,
    CollisionQueryRange& range
) {
    uint64_t candidateCount = 1;
    for (size_t axis = 0; axis < 3; ++axis) {
        if (!std::isfinite(bounds.min[axis]) ||
            !std::isfinite(bounds.max[axis]) ||
            bounds.min[axis] > bounds.max[axis]) {
            return false;
        }

        const double candidateMin = std::ceil(
            static_cast<double>(bounds.min[axis]) -
            BlockCollisionShape::MaximumCoordinate -
            BlockCollisionContactTolerance);
        const double candidateMax = std::floor(
            static_cast<double>(bounds.max[axis]) -
            BlockCollisionShape::MinimumCoordinate +
            BlockCollisionContactTolerance);
        if (candidateMin < std::numeric_limits<int>::min() ||
            candidateMax > std::numeric_limits<int>::max() ||
            candidateMin > candidateMax) {
            return false;
        }

        range.min[axis] = static_cast<int>(candidateMin);
        range.max[axis] = static_cast<int>(candidateMax);
        const uint64_t axisCandidateCount = static_cast<uint64_t>(
            static_cast<int64_t>(range.max[axis]) -
            static_cast<int64_t>(range.min[axis]) + 1);
        if (axisCandidateCount >
            kMaximumCollisionQueryCandidates / candidateCount) {
            return false;
        }
        candidateCount *= axisCandidateCount;
    }
    return true;
}

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
    const float worldX = static_cast<float>(x);
    const float worldY = static_cast<float>(y);
    const float worldZ = static_cast<float>(z);
    return {
        .min = {
            box.min[0] + worldX,
            box.min[1] + worldY,
            box.min[2] + worldZ,
        },
        .max = {
            box.max[0] + worldX,
            box.max[1] + worldY,
            box.max[2] + worldZ,
        },
    };
}

BlockCollisionBox fullCubeAt(int x, int y, int z) {
    const float worldX = static_cast<float>(x);
    const float worldY = static_cast<float>(y);
    const float worldZ = static_cast<float>(z);
    return {
        .min = {worldX, worldY, worldZ},
        .max = {worldX + 1.0f, worldY + 1.0f, worldZ + 1.0f},
    };
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

bool World::forEachCollisionBox(
    const BlockCollisionBox& bounds,
    void* context,
    CollisionBoxCallback callback
) const {
    CollisionQueryRange range;
    if (!collisionQueryRange(bounds, range)) {
        return false;
    }

    const BlockRegistry& registry = blockRegistry();
    for (int64_t wideX = range.min[0]; wideX <= range.max[0]; ++wideX) {
        const int x = static_cast<int>(wideX);
        for (int64_t wideY = range.min[1]; wideY <= range.max[1]; ++wideY) {
            const int y = static_cast<int>(wideY);
            for (int64_t wideZ = range.min[2]; wideZ <= range.max[2]; ++wideZ) {
                const int z = static_cast<int>(wideZ);
                const BlockState state = getBlock(x, y, z);
                if (state.isAir()) continue;

                const BlockCollisionShape& shape =
                    registry.getType(state.id).collision;
                if (shape.isEmpty()) continue;

                if (shape.isFullCube()) {
                    const BlockCollisionBox worldBox = fullCubeAt(x, y, z);
                    if (overlaps(bounds, worldBox)) {
                        callback(context, worldBox);
                    }
                    continue;
                }

                for (const BlockCollisionBox& localBox : shape.boxes()) {
                    const BlockCollisionBox worldBox =
                        translated(localBox, x, y, z);
                    if (overlaps(bounds, worldBox)) {
                        callback(context, worldBox);
                    }
                }
            }
        }
    }
    return true;
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
