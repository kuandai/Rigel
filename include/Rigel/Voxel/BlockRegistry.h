#pragma once

/**
 * @file BlockRegistry.h
 * @brief Block type registry for the voxel system.
 *
 * The BlockRegistry manages block type definitions. Each block type is
 * assigned a unique BlockID on registration. ID 0 is reserved for air.
 */

#include "Block.h"
#include "BlockType.h"

#include <vector>
#include <unordered_map>
#include <optional>
#include <stdexcept>

namespace Rigel::Voxel {

/**
 * @brief Exception thrown when block registration fails.
 */
class BlockRegistrationError : public std::runtime_error {
public:
    explicit BlockRegistrationError(const std::string& message)
        : std::runtime_error(message) {}
};

/**
 * @brief Registry for block type definitions.
 *
 * Block types are registered at initialization and assigned sequential IDs.
 * The registry provides lookup by both ID (fast) and identifier string.
 *
 * @section lifecycle Lifecycle
 *
 * 1. Create registry (automatically registers air as ID 0)
 * 2. Register block types via registerBlock()
 * 3. Look up types via getType() or findByIdentifier()
 *
 * @section thread_safety Thread Safety
 *
 * Registration is not thread-safe. Complete all registration before
 * accessing the registry from multiple threads.
 */
class BlockRegistry {
public:
    /**
     * @brief Construct registry with air pre-registered as ID 0.
     */
    BlockRegistry();

    /**
     * @brief Register a block type.
     *
     * @param identifier Unique identifier (e.g., "rigel:stone")
     * @param type The block type definition
     * @return The assigned BlockID
     *
     * @throws BlockRegistrationError if identifier is already registered
     * @throws BlockRegistrationError if maximum block count exceeded (65535)
     */
    BlockID registerBlock(const std::string& identifier, BlockType type);

    /**
     * @brief Get block type by ID.
     *
     * @param id The block ID (0 returns air type)
     * @return Reference to the block type
     *
     * @note Accessing an invalid ID is undefined behavior.
     *       Use size() to check bounds if needed.
     */
    const BlockType& getType(BlockID id) const {
        return m_types[id.type];
    }

    /**
     * @brief Find block ID by string identifier.
     *
     * @param identifier The block identifier
     * @return The BlockID if found, std::nullopt otherwise
     */
    std::optional<BlockID> findByIdentifier(const std::string& identifier) const;

    /**
     * @brief Check if an identifier is registered.
     */
    bool hasIdentifier(const std::string& identifier) const {
        return m_identifierMap.find(identifier) != m_identifierMap.end();
    }

    /// @name Iteration
    /// @{
    auto begin() const { return m_types.begin(); }
    auto end() const { return m_types.end(); }
    size_t size() const { return m_types.size(); }
    /// @}

    /**
     * @brief Get the air block ID (always 0).
     */
    static constexpr BlockID airId() { return BlockID{0}; }

private:
    std::vector<BlockType> m_types;
    std::unordered_map<std::string, BlockID> m_identifierMap;
};

} // namespace Rigel::Voxel
