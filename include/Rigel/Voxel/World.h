#pragma once

/**
 * @file World.h
 * @brief Authoritative voxel space (data + generator).
 *
 * World owns chunk data and the world generator. Rendering and mesh ownership
 * live in WorldView.
 */

#include "Block.h"
#include "BlockRegistry.h"
#include "ChunkManager.h"
#include "WorldGenerator.h"
#include "WorldId.h"

#include <Rigel/Entity/WorldEntities.h>

#include <memory>
#include <vector>

namespace Rigel::Voxel {

class WorldResources;

/**
 * @brief Authoritative voxel space.
 *
 * World provides access to chunk data and the world generator. Rendering,
 * streaming, and mesh ownership are handled by WorldView.
 *
 * @section usage Usage
 *
 * @code
 * Voxel::WorldResources resources;
 * resources.initialize(assets);
 *
 * Voxel::World world(resources);
 * world.setBlock(0, 0, 0, BlockState{stoneId});
 * @endcode
 */
class World {
public:
    World();
    explicit World(WorldResources& resources);
    ~World() = default;

    /// Non-copyable
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    /// Movable
    World(World&&) = default;
    World& operator=(World&&) = default;

    /**
     * @brief Initialize with shared resources.
     */
    void initialize(WorldResources& resources);

    /// World identifier (assigned by WorldSet).
    WorldId id() const { return m_id; }
    void setId(WorldId id) { m_id = id; }

    /// @name Component Access
    /// @{

    /// Get the block registry for type registration
    BlockRegistry& blockRegistry();
    const BlockRegistry& blockRegistry() const;

    /// Get the chunk manager for direct chunk access
    ChunkManager& chunkManager() { return m_chunkManager; }
    const ChunkManager& chunkManager() const { return m_chunkManager; }

    /// Access the entity manager
    Entity::WorldEntities& entities() { return m_entities; }
    const Entity::WorldEntities& entities() const { return m_entities; }

    /// @}

    /// @name Block Access (World Coordinates)
    /// @{

    /**
     * @brief Set block at world coordinates.
     *
     * Creates the containing chunk if needed.
     *
     * @param wx World X
     * @param wy World Y
     * @param wz World Z
     * @param state Block state to set
     */
    void setBlock(int wx, int wy, int wz, BlockState state);

    /**
     * @brief Get block at world coordinates.
     *
     * Returns air if chunk is not loaded.
     */
    BlockState getBlock(int wx, int wy, int wz) const;

    /// @}

    /// @name Lifecycle
    /// @{

    /**
     * @brief Unload all chunks.
     */
    void clear();

    /// @}

    /// @name Generation
    /// @{

    void setGenerator(std::shared_ptr<WorldGenerator> generator);
    const std::shared_ptr<WorldGenerator>& generator() const { return m_generator; }

    /**
     * @brief Tick entities for this world.
     */
    void tickEntities(float dt);

    /**
     * @brief Serialize a delta for replication.
     *
     * Stub for network integration.
     */
    std::vector<uint8_t> serializeChunkDelta(ChunkCoord coord) const;

    /// @}

private:
    WorldId m_id = kDefaultWorldId;
    WorldResources* m_resources = nullptr;
    ChunkManager m_chunkManager;
    Entity::WorldEntities m_entities;
    std::shared_ptr<WorldGenerator> m_generator;
    bool m_initialized = false;

};

} // namespace Rigel::Voxel
