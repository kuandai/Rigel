#pragma once

#include <Rigel/Voxel/Block.h>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace Rigel::Voxel {
class BlockRegistry;
class WorldGenerator;
}

namespace Rigel::Entity {
class Entity;
struct EntitySimulationState;
}

namespace Rigel::Simulation {

/** Stable identity for every simulation-relevant content record. */
class ContentManifestId final {
public:
    ContentManifestId() = default;
    explicit ContentManifestId(std::array<uint8_t, 32> bytes)
        : m_bytes(bytes) {}

    const std::array<uint8_t, 32>& bytes() const { return m_bytes; }
    std::string hex() const;
    bool operator==(const ContentManifestId&) const = default;

private:
    std::array<uint8_t, 32> m_bytes{};
};

/** Stable block-state meaning, independent of a registry's compact IDs. */
struct SemanticBlockState {
    std::string blockKey;
    uint8_t metadata = 0;
    uint8_t lightLevel = 0;

    bool operator==(const SemanticBlockState&) const = default;
};

struct ContentDictionaryEntry {
    std::string blockKey;
    Voxel::BlockID localId;
};

class ContentManifestError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/**
 * Frozen mapping between stable semantic keys and one process-local registry.
 * Canonical model, collision, entity-rule, and generator records contribute to
 * the identity but are not retained after construction.
 */
class ContentDictionary final {
public:
    static constexpr size_t kDefaultMaxRetainedBytes = 16 * 1024 * 1024;
    ContentDictionary(
        const Voxel::BlockRegistry& registry,
        const Voxel::WorldGenerator& generator,
        size_t maxRetainedBytes = kDefaultMaxRetainedBytes);

    ContentDictionary(const ContentDictionary&) = delete;
    ContentDictionary& operator=(const ContentDictionary&) = delete;
    ContentDictionary(ContentDictionary&&) = delete;
    ContentDictionary& operator=(ContentDictionary&&) = delete;

    const ContentManifestId& identity() const { return m_identity; }
    const std::vector<ContentDictionaryEntry>& entries() const {
        return m_entries;
    }
    /** Complete retained object, container, and string storage for this mapping. */
    std::optional<size_t> retainedStorageBytes() const;

    Voxel::BlockID localId(std::string_view stableKey) const;
    SemanticBlockState semanticState(Voxel::BlockState state) const;
    Voxel::BlockState localState(const SemanticBlockState& state) const;
    bool contains(std::string_view stableKey) const;
    /** True when current authoritative chunk storage preserves this state exactly. */
    bool supportsState(const SemanticBlockState& state) const;
    /** True only for the built-in entity rule and hitbox bound by this manifest. */
    bool supportsEntity(const Entity::Entity& entity) const;
    bool supportsEntityState(
        const Entity::EntitySimulationState& state) const;
    void requireIdentity(const ContentManifestId& identity) const;

private:
    std::vector<ContentDictionaryEntry> m_entries;
    std::vector<const ContentDictionaryEntry*> m_byLocalId;
    ContentManifestId m_identity;
};

} // namespace Rigel::Simulation
