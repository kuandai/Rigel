#include "Rigel/Simulation/ContentManifest.h"

#include "Rigel/Entity/Entity.h"
#include "Rigel/Voxel/BlockModel.h"
#include "Rigel/Voxel/BlockRegistry.h"
#include "Rigel/Voxel/GeneratorDefinition.h"
#include "Rigel/Voxel/WorldGenerator.h"
#include "../voxel/BlockModelGeometry.h"

#include <algorithm>
#include <bit>
#include <iomanip>
#include <limits>
#include <sstream>
#include <typeinfo>

namespace Rigel::Simulation {
namespace {

class CanonicalWriter final {
public:
    void byte(uint8_t value) { m_data.push_back(static_cast<char>(value)); }
    void boolean(bool value) { byte(value ? 1 : 0); }
    void u32(uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            byte(static_cast<uint8_t>(value >> shift));
        }
    }
    void string(std::string_view value) {
        if (value.size() > std::numeric_limits<uint32_t>::max()) {
            throw ContentManifestError("content field exceeds canonical limit");
        }
        u32(static_cast<uint32_t>(value.size()));
        m_data.append(value);
    }
    void floating(float value) { u32(std::bit_cast<uint32_t>(value)); }
    const std::string& data() const { return m_data; }

private:
    std::string m_data;
};

constexpr std::array<uint32_t, 64> ShaConstants = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

uint32_t rotateRight(uint32_t value, unsigned count) {
    return (value >> count) | (value << (32 - count));
}

ContentManifestId sha256(std::string_view input) {
    std::string padded(input);
    const uint64_t bitLength = static_cast<uint64_t>(padded.size()) * 8;
    padded.push_back(static_cast<char>(0x80));
    while (padded.size() % 64 != 56) padded.push_back('\0');
    for (int shift = 56; shift >= 0; shift -= 8) {
        padded.push_back(static_cast<char>(bitLength >> shift));
    }

    std::array<uint32_t, 8> hash = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
    };
    for (size_t offset = 0; offset < padded.size(); offset += 64) {
        std::array<uint32_t, 64> words{};
        for (size_t index = 0; index < 16; ++index) {
            const auto* bytes = reinterpret_cast<const uint8_t*>(
                padded.data() + offset + index * 4);
            words[index] = (static_cast<uint32_t>(bytes[0]) << 24) |
                (static_cast<uint32_t>(bytes[1]) << 16) |
                (static_cast<uint32_t>(bytes[2]) << 8) | bytes[3];
        }
        for (size_t index = 16; index < 64; ++index) {
            const uint32_t s0 = rotateRight(words[index - 15], 7) ^
                rotateRight(words[index - 15], 18) ^
                (words[index - 15] >> 3);
            const uint32_t s1 = rotateRight(words[index - 2], 17) ^
                rotateRight(words[index - 2], 19) ^
                (words[index - 2] >> 10);
            words[index] = words[index - 16] + s0 + words[index - 7] + s1;
        }
        auto [a, b, c, d, e, f, g, h] = hash;
        for (size_t index = 0; index < 64; ++index) {
            const uint32_t s1 = rotateRight(e, 6) ^ rotateRight(e, 11) ^
                rotateRight(e, 25);
            const uint32_t choose = (e & f) ^ (~e & g);
            const uint32_t temp1 = h + s1 + choose +
                ShaConstants[index] + words[index];
            const uint32_t s0 = rotateRight(a, 2) ^ rotateRight(a, 13) ^
                rotateRight(a, 22);
            const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t temp2 = s0 + majority;
            h = g; g = f; f = e; e = d + temp1;
            d = c; c = b; b = a; a = temp1 + temp2;
        }
        const std::array<uint32_t, 8> next = {a, b, c, d, e, f, g, h};
        for (size_t index = 0; index < hash.size(); ++index) {
            hash[index] += next[index];
        }
    }

    std::array<uint8_t, 32> result{};
    for (size_t index = 0; index < hash.size(); ++index) {
        for (size_t byte = 0; byte < 4; ++byte) {
            result[index * 4 + byte] = static_cast<uint8_t>(
                hash[index] >> (24 - byte * 8));
        }
    }
    return ContentManifestId(result);
}

std::string blockRecord(const Voxel::BlockType& type) {
    if (type.customData.has_value()) {
        throw ContentManifestError(
            "block '" + type.identifier +
            "' has unsupported untyped simulation metadata");
    }
    CanonicalWriter out;
    out.string(type.identifier);
    const auto selectableCuboids = static_cast<uint32_t>(std::count_if(
        type.model->cuboids().begin(), type.model->cuboids().end(),
        [](const Voxel::BlockModelCuboid& cuboid) {
            return std::any_of(
                cuboid.faces.begin(), cuboid.faces.end(),
                [](const auto& face) { return face.has_value(); });
        }));
    out.u32(selectableCuboids);
    for (const auto& cuboid : type.model->cuboids()) {
        if (std::none_of(
                cuboid.faces.begin(), cuboid.faces.end(),
                [](const auto& face) { return face.has_value(); })) {
            continue;
        }
        const auto bounds = Voxel::detail::orientedBounds(
            cuboid.bounds, type.model.orientation);
        for (float value : bounds.min) out.floating(value);
        for (float value : bounds.max) out.floating(value);
        std::array<bool, Voxel::DirectionCount> faces{};
        for (size_t source = 0; source < cuboid.faces.size(); ++source) {
            if (!cuboid.faces[source]) continue;
            const auto oriented = Voxel::detail::orientedDirection(
                static_cast<Voxel::Direction>(source),
                type.model.orientation);
            faces[static_cast<size_t>(oriented)] = true;
        }
        for (const bool face : faces) out.boolean(face);
    }

    out.byte(static_cast<uint8_t>(type.collision.kind()));
    out.u32(static_cast<uint32_t>(type.collision.boxes().size()));
    for (const auto& box : type.collision.boxes()) {
        for (float value : box.min) out.floating(value);
        for (float value : box.max) out.floating(value);
    }
    return out.data();
}

std::string entityRuleRecord() {
    CanonicalWriter out;
    out.string("rigel.entity-rule");
    out.u32(2);
    out.string("rigel:entity");
    out.string("axis-sweep-block-collision-v1");
    out.string("gravity-friction-tags-v1");
    for (float value : {-0.5f, -0.5f, -0.5f, 0.5f, 0.5f, 0.5f}) {
        out.floating(value);
    }
    return out.data();
}

} // namespace

std::string ContentManifestId::hex() const {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (uint8_t byte : m_bytes) out << std::setw(2) << unsigned(byte);
    return out.str();
}

ContentDictionary::ContentDictionary(
    const Voxel::BlockRegistry& registry,
    const Voxel::WorldGenerator& generator,
    size_t maxRetainedBytes
) {
    if (!registry.frozen()) {
        throw ContentManifestError(
            "content dictionary requires a frozen block registry");
    }
    if (generator.usesBlockGallery()) {
        throw ContentManifestError(
            "block gallery generation is unsupported by simulation authority");
    }

    // Reject aggregate key storage before copying any registry entry. Include
    // both lookup tables and inline-string capacity in this conservative estimate.
    size_t projectedBytes = 0;
    const auto charge = [&](size_t bytes) {
        if (bytes > maxRetainedBytes - projectedBytes) {
            throw ContentManifestError("content dictionary exceeds retained byte limit");
        }
        projectedBytes += bytes;
    };
    charge(sizeof(ContentDictionary));
    const size_t inlineStringCapacity = std::string{}.capacity();
    for (size_t index = 0; index < registry.size(); ++index) {
        charge(sizeof(ContentDictionaryEntry) + sizeof(const ContentDictionaryEntry*));
        const auto& key = registry.getType(
            Voxel::BlockID{static_cast<uint16_t>(index)}).identifier;
        charge(std::max(key.size(), inlineStringCapacity));
        charge(1);
    }

    m_entries.reserve(registry.size());
    for (size_t index = 0; index < registry.size(); ++index) {
        const auto localId = Voxel::BlockID{static_cast<uint16_t>(index)};
        const auto& type = registry.getType(localId);
        m_entries.push_back({type.identifier, localId});
    }
    std::sort(m_entries.begin(), m_entries.end(), [](const auto& a, const auto& b) {
        return a.blockKey < b.blockKey;
    });
    for (size_t index = 1; index < m_entries.size(); ++index) {
        if (m_entries[index - 1].blockKey == m_entries[index].blockKey) {
            throw ContentManifestError(
                "duplicate stable block key: " + m_entries[index].blockKey);
        }
    }

    m_byLocalId.resize(registry.size());
    const auto retainedBytes = retainedStorageBytes();
    if (!retainedBytes || *retainedBytes > maxRetainedBytes) {
        throw ContentManifestError("content dictionary exceeds retained byte limit");
    }
    CanonicalWriter manifest;
    manifest.string("rigel.content-manifest");
    manifest.u32(4);
    manifest.u32(static_cast<uint32_t>(m_entries.size()));
    for (const auto& entry : m_entries) {
        m_byLocalId[entry.localId.type] = &entry;
        manifest.string(entry.blockKey);
        manifest.string(blockRecord(registry.getType(entry.localId)));
    }
    manifest.string(entityRuleRecord());

    CanonicalWriter generatorRecord;
    generatorRecord.string("rigel.world-generator");
    generatorRecord.u32(generator.semanticsVersion());
    generatorRecord.u32(generator.seed());
    generatorRecord.string(
        Voxel::serializeGeneratorDefinitionSnapshot(generator.definition()));
    manifest.string(generatorRecord.data());
    m_identity = sha256(manifest.data());
}

std::optional<size_t> ContentDictionary::retainedStorageBytes() const {
    size_t total = sizeof(ContentDictionary);
    if (m_entries.capacity() >
            (std::numeric_limits<size_t>::max() - total) /
                sizeof(ContentDictionaryEntry)) {
        return std::nullopt;
    }
    total += m_entries.capacity() * sizeof(ContentDictionaryEntry);
    for (const auto& entry : m_entries) {
        if (entry.blockKey.capacity() >
            std::numeric_limits<size_t>::max() - total) {
            return std::nullopt;
        }
        total += entry.blockKey.capacity();
    }
    if (m_byLocalId.capacity() >
            (std::numeric_limits<size_t>::max() - total) /
                sizeof(const ContentDictionaryEntry*)) {
        return std::nullopt;
    }
    total += m_byLocalId.capacity() * sizeof(const ContentDictionaryEntry*);
    return total;
}

Voxel::BlockID ContentDictionary::localId(std::string_view stableKey) const {
    const auto found = std::lower_bound(
        m_entries.begin(), m_entries.end(), stableKey,
        [](const ContentDictionaryEntry& entry, std::string_view key) {
            return entry.blockKey < key;
        });
    if (found == m_entries.end() || found->blockKey != stableKey) {
        throw ContentManifestError(
            "unknown stable block key: " + std::string(stableKey));
    }
    return found->localId;
}

bool ContentDictionary::contains(std::string_view stableKey) const {
    const auto found = std::lower_bound(
        m_entries.begin(), m_entries.end(), stableKey,
        [](const ContentDictionaryEntry& entry, std::string_view key) {
            return entry.blockKey < key;
        });
    return found != m_entries.end() && found->blockKey == stableKey;
}

bool ContentDictionary::supportsState(const SemanticBlockState& state) const {
    return contains(state.blockKey) &&
        (state.blockKey != "base:air" || state.metadata == 0);
}

bool ContentDictionary::supportsPublishedState(
    const SemanticBlockState& state, uint8_t lightLevel
) const {
    return supportsState(state) &&
        (state.blockKey != "base:air" || lightLevel == 0);
}

bool ContentDictionary::supportsEntity(const Entity::Entity& entity) const {
    return typeid(entity) == typeid(Entity::Entity) &&
        !entity.model() && supportsEntityState(entity.simulationState());
}

bool ContentDictionary::supportsEntityState(
    const Entity::EntitySimulationState& state
) const {
    return state.typeId == "rigel:entity" &&
        (state.modelIdentifier.empty() ||
         state.modelIdentifier == "entity_models/model_drone_interceptor" ||
         state.modelIdentifier == "entity_models/demo_cube") &&
        state.localBounds.min == glm::vec3(-0.5f) &&
        state.localBounds.max == glm::vec3(0.5f);
}

SemanticBlockState ContentDictionary::semanticState(Voxel::BlockState state) const {
    if (state.id.type >= m_byLocalId.size() || !m_byLocalId[state.id.type]) {
        throw ContentManifestError("block state contains an invalid local ID");
    }
    return {
        m_byLocalId[state.id.type]->blockKey,
        state.metadata,
    };
}

Voxel::BlockState ContentDictionary::localState(
    const SemanticBlockState& state, uint8_t lightLevel
) const {
    return {localId(state.blockKey), state.metadata, lightLevel};
}

void ContentDictionary::requireIdentity(
    const ContentManifestId& identity
) const {
    if (identity != m_identity) {
        throw ContentManifestError(
            "content manifest mismatch: expected " + m_identity.hex() +
            ", received " + identity.hex());
    }
}

} // namespace Rigel::Simulation
