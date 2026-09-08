#pragma once

#include <Rigel/Voxel/Block.h>

#include <array>
#include <cstdint>
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
    std::string semanticRecord;
};

class ContentManifestError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/**
 * Frozen mapping between stable semantic keys and one process-local registry.
 * The retained canonical records are the authority; the digest is only their
 * compact identity.
 */
class ContentDictionary final {
public:
    ContentDictionary(
        const Voxel::BlockRegistry& registry,
        const Voxel::WorldGenerator& generator);

    ContentDictionary(const ContentDictionary&) = delete;
    ContentDictionary& operator=(const ContentDictionary&) = delete;
    ContentDictionary(ContentDictionary&&) = delete;
    ContentDictionary& operator=(ContentDictionary&&) = delete;

    const ContentManifestId& identity() const { return m_identity; }
    const std::vector<ContentDictionaryEntry>& entries() const {
        return m_entries;
    }
    const std::string& generatorRecord() const { return m_generatorRecord; }

    Voxel::BlockID localId(std::string_view stableKey) const;
    SemanticBlockState semanticState(Voxel::BlockState state) const;
    Voxel::BlockState localState(const SemanticBlockState& state) const;
    bool contains(std::string_view stableKey) const;
    /** True only for the built-in entity rule and hitbox bound by this manifest. */
    bool supportsEntity(const Entity::Entity& entity) const;
    void requireIdentity(const ContentManifestId& identity) const;

private:
    std::vector<ContentDictionaryEntry> m_entries;
    std::vector<const ContentDictionaryEntry*> m_byLocalId;
    std::string m_generatorRecord;
    ContentManifestId m_identity;
};

} // namespace Rigel::Simulation
