#include "EntityRegionJournal.h"

#include "../entity/EntityPersistenceLimits.h"

#include "Rigel/Entity/EntityPersistence.h"
#include "Rigel/Persistence/Format.h"
#include "Rigel/Persistence/Storage.h"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Rigel::Persistence::detail {
namespace {

constexpr uint32_t kJournalMagic = 0x5247454A; // "RGEJ"
constexpr uint16_t kJournalVersion = 2;
constexpr const char* kJournalFilename = "entity-regions.journal";
constexpr uint32_t kMaxJournalRegions = 1'048'576;
constexpr size_t kMaxJournalPayloadBytes =
    Entity::detail::MaxEntityRegionBytes;
constexpr uint32_t kMaxJournalStringBytes = 1'048'576;
constexpr uint64_t kMinDesiredRegionBytes = 32;
constexpr uint64_t kMinObsoleteRegionBytes = 16;

struct EntityRegionKeyLess {
    bool operator()(const EntityRegionKey& lhs,
                    const EntityRegionKey& rhs) const {
        return std::tie(lhs.zoneId, lhs.x, lhs.y, lhs.z) <
            std::tie(rhs.zoneId, rhs.x, rhs.y, rhs.z);
    }
};

struct EntityRegionJournal {
    std::vector<EntityRegionSnapshot> desiredRegions;
    std::vector<EntityRegionKey> obsoleteRegions;
};

std::string journalPath(const PersistenceContext& context) {
    if (context.rootPath.empty()) {
        return kJournalFilename;
    }
    return context.rootPath + "/" + kJournalFilename;
}

std::string parentPath(const std::string& path) {
    const auto pos = path.find_last_of('/');
    if (pos == std::string::npos) {
        return {};
    }
    return path.substr(0, pos);
}

std::string describeId(const Entity::EntityId& id) {
    return std::to_string(id.time) + ":" +
        std::to_string(id.random) + ":" +
        std::to_string(id.counter);
}

std::string describeRegion(const EntityRegionKey& key) {
    return key.zoneId + "/(" + std::to_string(key.x) + ", " +
        std::to_string(key.y) + ", " + std::to_string(key.z) + ")";
}

size_t remaining(const ByteReader& reader) {
    if (reader.tell() > reader.size()) {
        throw std::runtime_error("Invalid entity region journal reader position");
    }
    return reader.size() - reader.tell();
}

void requireRemaining(const ByteReader& reader,
                      size_t required,
                      const char* diagnostic) {
    if (required > remaining(reader)) {
        throw std::runtime_error(diagnostic);
    }
}

void writeString(ByteWriter& writer, const std::string& value) {
    if (value.size() > kMaxJournalStringBytes ||
        value.size() > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error("Entity region journal string is too large");
    }
    writer.writeU32(static_cast<uint32_t>(value.size()));
    if (!value.empty()) {
        writer.writeBytes(
            reinterpret_cast<const uint8_t*>(value.data()), value.size());
    }
}

std::string readString(ByteReader& reader) {
    requireRemaining(
        reader, sizeof(uint32_t),
        "Truncated string length in entity region journal");
    const uint32_t size = reader.readU32();
    if (size > kMaxJournalStringBytes) {
        throw std::runtime_error(
            "Entity region journal string exceeds size limit");
    }
    if (size > remaining(reader)) {
        throw std::runtime_error("Truncated string in entity region journal");
    }
    std::string value(size, '\0');
    if (size > 0) {
        reader.readBytes(reinterpret_cast<uint8_t*>(value.data()), size);
    }
    return value;
}

void writeKey(ByteWriter& writer, const EntityRegionKey& key) {
    writeString(writer, key.zoneId);
    writer.writeI32(key.x);
    writer.writeI32(key.y);
    writer.writeI32(key.z);
}

EntityRegionKey readKey(ByteReader& reader) {
    EntityRegionKey key;
    key.zoneId = readString(reader);
    requireRemaining(
        reader, 3 * sizeof(uint32_t),
        "Truncated entity region key in recovery journal");
    key.x = reader.readI32();
    key.y = reader.readI32();
    key.z = reader.readI32();
    return key;
}

void sortAndValidateJournal(EntityRegionJournal& journal) {
    EntityRegionKeyLess less;
    std::sort(
        journal.desiredRegions.begin(), journal.desiredRegions.end(),
        [&](const EntityRegionSnapshot& lhs, const EntityRegionSnapshot& rhs) {
            return less(lhs.key, rhs.key);
        });
    std::sort(
        journal.obsoleteRegions.begin(), journal.obsoleteRegions.end(), less);

    for (size_t i = 0; i < journal.desiredRegions.size(); ++i) {
        const auto& region = journal.desiredRegions[i];
        bool populated = false;
        for (const auto& chunk : region.chunks) {
            populated = populated || !chunk.entities.empty();
        }
        if (!populated) {
            throw std::runtime_error(
                "Entity region journal contains an empty desired region " +
                describeRegion(region.key));
        }
        if (i > 0 && region.key == journal.desiredRegions[i - 1].key) {
            throw std::runtime_error(
                "Entity region journal repeats desired region " +
                describeRegion(region.key));
        }
    }

    for (size_t i = 0; i < journal.obsoleteRegions.size(); ++i) {
        const auto& key = journal.obsoleteRegions[i];
        if (i > 0 && key == journal.obsoleteRegions[i - 1]) {
            throw std::runtime_error(
                "Entity region journal repeats obsolete region " +
                describeRegion(key));
        }
        const auto desired = std::lower_bound(
            journal.desiredRegions.begin(), journal.desiredRegions.end(), key,
            [&](const EntityRegionSnapshot& region,
                const EntityRegionKey& candidate) {
                return less(region.key, candidate);
            });
        if (desired != journal.desiredRegions.end() && desired->key == key) {
            throw std::runtime_error(
                "Entity region journal marks desired region obsolete " +
                describeRegion(key));
        }
    }

    validateEntityRegionSnapshots(journal.desiredRegions);
}

void writeJournal(ByteWriter& writer,
                  const EntityRegionJournal& journal,
                  const FormatDescriptor& format) {
    if (journal.desiredRegions.size() > kMaxJournalRegions ||
        journal.obsoleteRegions.size() > kMaxJournalRegions ||
        journal.desiredRegions.size() > std::numeric_limits<uint32_t>::max() ||
        journal.obsoleteRegions.size() > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error("Entity region journal has too many regions");
    }
    if (format.id.empty()) {
        throw std::runtime_error(
            "Entity region journal requires a persistence format identity");
    }

    writer.writeU32(kJournalMagic);
    writer.writeU16(kJournalVersion);
    writer.writeU16(0);
    writeString(writer, format.id);
    writer.writeI32(format.version);
    writer.writeU32(static_cast<uint32_t>(journal.desiredRegions.size()));
    writer.writeU32(static_cast<uint32_t>(journal.obsoleteRegions.size()));

    for (const auto& region : journal.desiredRegions) {
        writeKey(writer, region.key);
        const auto payload = Entity::encodeEntityRegionPayload(region.chunks);
        if (payload.size() > kMaxJournalPayloadBytes ||
            payload.size() > std::numeric_limits<uint32_t>::max()) {
            throw std::runtime_error(
                "Entity region payload is too large for recovery journal");
        }
        writer.writeU32(static_cast<uint32_t>(payload.size()));
        if (!payload.empty()) {
            writer.writeBytes(payload.data(), payload.size());
        }
    }
    for (const auto& key : journal.obsoleteRegions) {
        writeKey(writer, key);
    }
}

EntityRegionJournal readJournal(ByteReader& reader,
                                const FormatDescriptor& format) {
    requireRemaining(
        reader, 2 * sizeof(uint32_t),
        "Truncated entity region journal header");
    if (reader.readU32() != kJournalMagic) {
        throw std::runtime_error("Invalid entity region journal magic");
    }
    if (reader.readU16() != kJournalVersion) {
        throw std::runtime_error("Unsupported entity region journal version");
    }
    if (reader.readU16() != 0) {
        throw std::runtime_error("Invalid entity region journal header flags");
    }

    const std::string formatId = readString(reader);
    requireRemaining(
        reader, 3 * sizeof(uint32_t),
        "Truncated entity region journal format and counts");
    const int32_t formatVersion = reader.readI32();
    if (formatId != format.id || formatVersion != format.version) {
        throw std::runtime_error(
            "Entity recovery journal persistence format mismatch: journal uses " +
            formatId + " version " + std::to_string(formatVersion) +
            ", opened " + format.id + " version " +
            std::to_string(format.version));
    }

    EntityRegionJournal journal;
    const uint32_t desiredCount = reader.readU32();
    const uint32_t obsoleteCount = reader.readU32();
    if (desiredCount > kMaxJournalRegions ||
        obsoleteCount > kMaxJournalRegions) {
        throw std::runtime_error(
            "Entity region journal region count exceeds limit");
    }
    const uint64_t minimumBytes =
        static_cast<uint64_t>(desiredCount) * kMinDesiredRegionBytes +
        static_cast<uint64_t>(obsoleteCount) * kMinObsoleteRegionBytes;
    if (minimumBytes > remaining(reader)) {
        throw std::runtime_error(
            "Entity region journal counts exceed remaining data");
    }
    journal.desiredRegions.reserve(desiredCount);
    journal.obsoleteRegions.reserve(obsoleteCount);

    for (uint32_t i = 0; i < desiredCount; ++i) {
        EntityRegionSnapshot region;
        region.key = readKey(reader);
        requireRemaining(
            reader, sizeof(uint32_t),
            "Truncated entity region payload size in recovery journal");
        const uint32_t payloadSize = reader.readU32();
        if (payloadSize > kMaxJournalPayloadBytes) {
            throw std::runtime_error(
                "Entity region journal payload exceeds size limit");
        }
        if (payloadSize > remaining(reader)) {
            throw std::runtime_error(
                "Truncated entity region payload in recovery journal");
        }
        std::vector<uint8_t> payload(payloadSize);
        if (payloadSize > 0) {
            reader.readBytes(payload.data(), payload.size());
        }
        if (!Entity::decodeEntityRegionPayload(payload, region.chunks)) {
            throw std::runtime_error(
                "Invalid entity payload for journal region " +
                describeRegion(region.key));
        }
        journal.desiredRegions.push_back(std::move(region));
    }
    for (uint32_t i = 0; i < obsoleteCount; ++i) {
        journal.obsoleteRegions.push_back(readKey(reader));
    }
    if (reader.tell() != reader.size()) {
        throw std::runtime_error("Trailing data in entity region journal");
    }

    sortAndValidateJournal(journal);
    return journal;
}

void publishJournal(const EntityRegionJournal& journal,
                    const FormatDescriptor& format,
                    const PersistenceContext& context) {
    const std::string path = journalPath(context);
    context.storage->mkdirs(parentPath(path));
    auto session = context.storage->openWrite(path, AtomicWriteOptions{});
    writeJournal(session->writer(), journal, format);
    session->writer().flush();
    session->commit();
}

void applyJournal(PersistenceFormat& format,
                  const PersistenceContext& context,
                  const EntityRegionJournal& journal) {
    for (const auto& region : journal.desiredRegions) {
        try {
            format.entityContainer().saveRegion(region);
        } catch (const std::exception& error) {
            throw std::runtime_error(
                "Failed to apply entity recovery journal to region " +
                describeRegion(region.key) + ": " + error.what());
        }
    }
    for (const auto& key : journal.obsoleteRegions) {
        try {
            format.entityContainer().removeRegion(key);
        } catch (const std::exception& error) {
            throw std::runtime_error(
                "Failed to remove obsolete entity region " +
                describeRegion(key) + ": " + error.what());
        }
    }

    try {
        context.storage->remove(journalPath(context));
    } catch (const std::exception& error) {
        throw std::runtime_error(
            std::string("Failed to remove completed entity recovery journal: ") +
            error.what());
    }
}

void validateJournalZone(const EntityRegionJournal& journal,
                         const std::string& zoneId) {
    for (const auto& region : journal.desiredRegions) {
        if (region.key.zoneId != zoneId) {
            throw std::runtime_error(
                "Entity recovery journal desired region belongs to unexpected zone " +
                describeRegion(region.key));
        }
    }
    for (const auto& key : journal.obsoleteRegions) {
        if (key.zoneId != zoneId) {
            throw std::runtime_error(
                "Entity recovery journal obsolete region belongs to unexpected zone " +
                describeRegion(key));
        }
    }
}

} // namespace

void validateEntityRegionSnapshots(
    const std::vector<EntityRegionSnapshot>& regions) {
    std::unordered_map<Entity::EntityId,
                       EntityRegionKey,
                       Entity::EntityIdHash> sources;
    for (const auto& region : regions) {
        std::unordered_set<Voxel::ChunkCoord, Voxel::ChunkCoordHash> chunks;
        for (const auto& chunk : region.chunks) {
            if (!chunks.insert(chunk.coord).second) {
                throw std::runtime_error(
                    "Duplicate entity chunk coordinates in region " +
                    describeRegion(region.key));
            }
            const Entity::PersistenceRegionCoord expected =
                Entity::persistenceRegionForChunk(chunk.coord);
            if (expected.x != region.key.x ||
                expected.y != region.key.y ||
                expected.z != region.key.z) {
                throw std::runtime_error(
                    "Entity chunk lies outside region " +
                    describeRegion(region.key));
            }
            for (const auto& entity : chunk.entities) {
                if (!Entity::detail::isPersistablePosition(entity.position)) {
                    throw std::runtime_error(
                        "Invalid persistent entity position in region " +
                        describeRegion(region.key));
                }
                if (!Entity::detail::isFiniteVector(entity.velocity) ||
                    !Entity::detail::isFiniteVector(entity.viewDirection)) {
                    throw std::runtime_error(
                        "Invalid persistent entity vector in region " +
                        describeRegion(region.key));
                }
                if (entity.id.isNull()) {
                    throw std::runtime_error(
                        "Null persistent entity ID " + describeId(entity.id) +
                        " in entity region " + describeRegion(region.key));
                }
                const auto [existing, inserted] =
                    sources.emplace(entity.id, region.key);
                if (!inserted) {
                    throw std::runtime_error(
                        "Duplicate persistent entity ID " +
                        describeId(entity.id) + " in entity regions " +
                        describeRegion(existing->second) + " and " +
                        describeRegion(region.key));
                }
            }
        }
    }
}

void replayEntityRegionJournal(
    PersistenceFormat& format,
    const PersistenceContext& context,
    const std::string& zoneId) {
    const std::string path = journalPath(context);
    if (!context.storage->exists(path)) {
        return;
    }

    try {
        auto reader = context.storage->openRead(path);
        const EntityRegionJournal journal =
            readJournal(*reader, format.descriptor());
        validateJournalZone(journal, zoneId);
        applyJournal(format, context, journal);
    } catch (const std::exception& error) {
        throw std::runtime_error(
            std::string("Failed to replay entity recovery journal: ") +
            error.what());
    }
}

void saveEntityRegionsRecoverably(
    PersistenceFormat& format,
    const PersistenceContext& context,
    const std::string& zoneId,
    std::vector<EntityRegionSnapshot> desiredRegions) {
    EntityRegionJournal journal;
    journal.desiredRegions = std::move(desiredRegions);

    std::set<EntityRegionKey, EntityRegionKeyLess> desiredKeys;
    for (const auto& region : journal.desiredRegions) {
        desiredKeys.insert(region.key);
    }
    for (const auto& key : format.entityContainer().listRegions(zoneId)) {
        if (!desiredKeys.contains(key)) {
            journal.obsoleteRegions.push_back(key);
        }
    }

    sortAndValidateJournal(journal);
    validateJournalZone(journal, zoneId);
    if (journal.desiredRegions.empty() && journal.obsoleteRegions.empty()) {
        return;
    }
    publishJournal(journal, format.descriptor(), context);
    applyJournal(format, context, journal);
}

} // namespace Rigel::Persistence::detail
