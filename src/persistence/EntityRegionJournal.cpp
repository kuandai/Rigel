#include "EntityRegionJournal.h"

#include "../entity/EntityPersistenceDetail.h"
#include "../entity/EntityPersistenceLimits.h"
#include "EntityRegionJournalLimits.h"

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
constexpr uint32_t kMaxJournalRegionsPerList = 1'048'576;
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

void consumeLimit(size_t& used,
                  size_t amount,
                  size_t limit,
                  const char* diagnostic) {
    if (used > limit || amount > limit - used) {
        throw std::runtime_error(diagnostic);
    }
    used += amount;
}

size_t encodedStringBytes(const std::string& value) {
    if (value.size() > kMaxJournalStringBytes ||
        value.size() > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error("Entity region journal string is too large");
    }
    return sizeof(uint32_t) + value.size();
}

void consumeEncodedBytes(EntityRegionJournalUsage& usage, size_t amount) {
    consumeLimit(
        usage.encodedBytes,
        amount,
        MaxEntityJournalEncodedBytes,
        "Entity region journal encoded size exceeds limit");
}

void consumeKey(EntityRegionJournalUsage& usage, const EntityRegionKey& key) {
    consumeEncodedBytes(usage, encodedStringBytes(key.zoneId));
    consumeEncodedBytes(usage, 3 * sizeof(uint32_t));
}

void consumeRegion(EntityRegionJournalUsage& usage) {
    consumeLimit(
        usage.regions,
        1,
        MaxEntityJournalRegions,
        "Entity region journal aggregate region count exceeds limit");
}

void consumeRegionPayload(EntityRegionJournalUsage& usage,
                          size_t& regionPayloadBytes,
                          size_t amount) {
    consumeLimit(
        regionPayloadBytes,
        amount,
        Entity::detail::MaxEntityRegionBytes,
        "Entity region payload is too large");
    consumeLimit(
        usage.payloadBytes,
        amount,
        MaxEntityJournalPayloadBytes,
        "Entity region journal aggregate payload exceeds size limit");
    consumeEncodedBytes(usage, amount);
}

void validateJournalRegionCounts(size_t desiredCount,
                                 size_t obsoleteCount) {
    if (desiredCount > std::numeric_limits<uint32_t>::max() ||
        obsoleteCount > std::numeric_limits<uint32_t>::max() ||
        desiredCount > kMaxJournalRegionsPerList ||
        obsoleteCount > kMaxJournalRegionsPerList) {
        throw std::runtime_error(
            "Entity region journal region count exceeds limit");
    }
    if (desiredCount > MaxEntityJournalRegions ||
        obsoleteCount > MaxEntityJournalRegions - desiredCount) {
        throw std::runtime_error(
            "Entity region journal aggregate region count exceeds limit");
    }
}

EntityRegionJournalUsage beginJournalUsage(const FormatDescriptor& format) {
    if (format.id.empty()) {
        throw std::runtime_error(
            "Entity region journal requires a persistence format identity");
    }

    EntityRegionJournalUsage usage;
    consumeEncodedBytes(usage, 2 * sizeof(uint32_t));
    consumeEncodedBytes(usage, encodedStringBytes(format.id));
    consumeEncodedBytes(usage, 3 * sizeof(uint32_t));
    return usage;
}

EntityRegionJournalUsage measureJournal(
    const EntityRegionJournal& journal,
    const FormatDescriptor& format) {
    validateJournalRegionCounts(
        journal.desiredRegions.size(), journal.obsoleteRegions.size());
    EntityRegionJournalUsage usage = beginJournalUsage(format);
    for (const auto& region : journal.desiredRegions) {
        if (region.chunks.size() > Entity::detail::MaxChunksPerEntityRegion) {
            throw std::runtime_error("Entity region has too many chunks");
        }
        size_t regionPayloadBytes = accountDesiredEntityRegion(
            usage, region.key);
        size_t existingRegionChunks = 0;
        for (const auto& chunk : region.chunks) {
            if (chunk.entities.size() >
                Entity::detail::MaxEntitiesPerChunk) {
                throw std::runtime_error(
                    "Entity chunk has too many entities");
            }
            accountEntityRegionChunk(
                usage, regionPayloadBytes, existingRegionChunks);
            ++existingRegionChunks;
            size_t existingChunkEntities = 0;
            for (const auto& entity : chunk.entities) {
                accountEntityRegionEntity(
                    usage,
                    regionPayloadBytes,
                    existingChunkEntities,
                    Entity::detail::measurePersistedEntityBytes(
                        entity.typeId, entity.modelId));
                ++existingChunkEntities;
            }
        }
    }
    for (const auto& key : journal.obsoleteRegions) {
        consumeRegion(usage);
        consumeKey(usage, key);
    }
    return usage;
}

void writeString(ByteWriter& writer, const std::string& value) {
    encodedStringBytes(value);
    writer.writeU32(static_cast<uint32_t>(value.size()));
    if (!value.empty()) {
        writer.writeBytes(
            reinterpret_cast<const uint8_t*>(value.data()), value.size());
    }
}

void skipString(ByteReader& reader) {
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
    reader.seek(reader.tell() + size);
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

void skipKey(ByteReader& reader) {
    skipString(reader);
    requireRemaining(
        reader, 3 * sizeof(uint32_t),
        "Truncated entity region key in recovery journal");
    reader.seek(reader.tell() + 3 * sizeof(uint32_t));
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
                  const FormatDescriptor& format,
                  const EntityRegionJournalUsage& expectedUsage) {
    if (writer.tell() != 0) {
        throw std::runtime_error(
            "Entity region journal writer did not start at the beginning");
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
        const Entity::detail::EntityRegionPayloadInfo info =
            Entity::detail::measureEntityRegionPayload(region.chunks);
        const auto payload = Entity::encodeEntityRegionPayload(region.chunks);
        if (payload.size() != info.encodedBytes ||
            payload.size() > kMaxJournalPayloadBytes ||
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
    if (writer.tell() != expectedUsage.encodedBytes) {
        throw std::runtime_error(
            "Entity region journal encoded size did not match its preflight");
    }
}

std::pair<uint32_t, uint32_t> readJournalPrefix(
    ByteReader& reader,
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

    const uint32_t desiredCount = reader.readU32();
    const uint32_t obsoleteCount = reader.readU32();
    if (desiredCount > kMaxJournalRegionsPerList ||
        obsoleteCount > kMaxJournalRegionsPerList) {
        throw std::runtime_error(
            "Entity region journal region count exceeds limit");
    }
    if (desiredCount > MaxEntityJournalRegions ||
        obsoleteCount > MaxEntityJournalRegions - desiredCount) {
        throw std::runtime_error(
            "Entity region journal aggregate region count exceeds limit");
    }
    const size_t available = remaining(reader);
    if (desiredCount > available / kMinDesiredRegionBytes) {
        throw std::runtime_error(
            "Entity region journal counts exceed remaining data");
    }
    const size_t desiredBytes = desiredCount * kMinDesiredRegionBytes;
    if (obsoleteCount >
        (available - desiredBytes) / kMinObsoleteRegionBytes) {
        throw std::runtime_error(
            "Entity region journal counts exceed remaining data");
    }
    return {desiredCount, obsoleteCount};
}

void preflightJournal(ByteReader& reader,
                      const FormatDescriptor& format) {
    if (reader.size() > MaxEntityJournalEncodedBytes) {
        throw std::runtime_error(
            "Entity region journal encoded size exceeds limit");
    }

    const auto [desiredCount, obsoleteCount] =
        readJournalPrefix(reader, format);
    EntityRegionJournalUsage usage;
    usage.encodedBytes = reader.size();
    usage.regions = static_cast<size_t>(desiredCount) + obsoleteCount;

    for (uint32_t i = 0; i < desiredCount; ++i) {
        skipKey(reader);
        requireRemaining(
            reader, sizeof(uint32_t),
            "Truncated entity region payload size in recovery journal");
        const uint32_t payloadSize = reader.readU32();
        if (payloadSize > kMaxJournalPayloadBytes) {
            throw std::runtime_error(
                "Entity region journal payload exceeds size limit");
        }
        consumeLimit(
            usage.payloadBytes,
            payloadSize,
            MaxEntityJournalPayloadBytes,
            "Entity region journal aggregate payload exceeds size limit");
        if (payloadSize > remaining(reader)) {
            throw std::runtime_error(
                "Truncated entity region payload in recovery journal");
        }

        std::vector<uint8_t> payload(payloadSize);
        if (payloadSize > 0) {
            reader.readBytes(payload.data(), payload.size());
        }
        Entity::detail::EntityRegionPayloadInfo info;
        const auto inspection = Entity::detail::inspectEntityRegionPayload(
            payload,
            MaxEntityJournalChunks - usage.chunks,
            MaxEntityJournalEntities - usage.entities,
            info);
        if (inspection ==
            Entity::detail::EntityRegionPayloadInspection::ChunkLimitExceeded) {
            throw std::runtime_error(
                "Entity region journal aggregate chunk count exceeds limit");
        }
        if (inspection ==
            Entity::detail::EntityRegionPayloadInspection::EntityLimitExceeded) {
            throw std::runtime_error(
                "Entity region journal aggregate entity count exceeds limit");
        }
        if (inspection !=
            Entity::detail::EntityRegionPayloadInspection::Valid) {
            throw std::runtime_error(
                "Invalid entity payload for journal region during preflight");
        }
        usage.chunks += info.chunks;
        usage.entities += info.entities;
    }
    for (uint32_t i = 0; i < obsoleteCount; ++i) {
        skipKey(reader);
    }
    if (reader.tell() != reader.size()) {
        throw std::runtime_error("Trailing data in entity region journal");
    }
}

EntityRegionJournal decodeJournal(ByteReader& reader,
                                  const FormatDescriptor& format) {
    const auto [desiredCount, obsoleteCount] =
        readJournalPrefix(reader, format);
    EntityRegionJournal journal;
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

EntityRegionJournal readJournal(ByteReader& reader,
                                const FormatDescriptor& format) {
    preflightJournal(reader, format);
    reader.seek(0);
    return decodeJournal(reader, format);
}

void publishJournal(const EntityRegionJournal& journal,
                    const FormatDescriptor& format,
                    const PersistenceContext& context) {
    const EntityRegionJournalUsage usage = measureJournal(journal, format);
    const std::string path = journalPath(context);
    context.storage->mkdirs(parentPath(path));
    auto session = context.storage->openWrite(path);
    writeJournal(session->writer(), journal, format, usage);
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

EntityRegionJournalUsage beginEntityRegionJournalUsage(
    const FormatDescriptor& format) {
    return beginJournalUsage(format);
}

size_t accountDesiredEntityRegion(
    EntityRegionJournalUsage& usage,
    const EntityRegionKey& key) {
    consumeRegion(usage);
    consumeKey(usage, key);
    consumeEncodedBytes(usage, sizeof(uint32_t));
    size_t regionPayloadBytes = 0;
    consumeRegionPayload(
        usage, regionPayloadBytes, 3 * sizeof(uint32_t));
    return regionPayloadBytes;
}

void accountEntityRegionChunk(
    EntityRegionJournalUsage& usage,
    size_t& regionPayloadBytes,
    size_t existingRegionChunks) {
    if (existingRegionChunks >= Entity::detail::MaxChunksPerEntityRegion) {
        throw std::runtime_error("Entity region has too many chunks");
    }
    consumeLimit(
        usage.chunks,
        1,
        MaxEntityJournalChunks,
        "Entity region journal aggregate chunk count exceeds limit");
    consumeRegionPayload(
        usage,
        regionPayloadBytes,
        Entity::detail::MinEncodedChunkBytes);
}

void accountEntityRegionEntity(
    EntityRegionJournalUsage& usage,
    size_t& regionPayloadBytes,
    size_t existingChunkEntities,
    size_t encodedEntityBytes) {
    if (existingChunkEntities >= Entity::detail::MaxEntitiesPerChunk) {
        throw std::runtime_error("Entity chunk has too many entities");
    }
    consumeLimit(
        usage.entities,
        1,
        MaxEntityJournalEntities,
        "Entity region journal aggregate entity count exceeds limit");
    consumeRegionPayload(
        usage, regionPayloadBytes, encodedEntityBytes);
}

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

    EntityRegionJournalUsage usage =
        measureJournal(journal, format.descriptor());

    std::set<EntityRegionKey, EntityRegionKeyLess> desiredKeys;
    for (const auto& region : journal.desiredRegions) {
        desiredKeys.insert(region.key);
    }
    format.entityContainer().forEachRegion(zoneId, [&](const auto& key) {
        if (!desiredKeys.contains(key)) {
            consumeRegion(usage);
            consumeKey(usage, key);
            journal.obsoleteRegions.push_back(key);
        }
        return true;
    });

    sortAndValidateJournal(journal);
    validateJournalZone(journal, zoneId);
    if (journal.desiredRegions.empty() && journal.obsoleteRegions.empty()) {
        return;
    }
    publishJournal(journal, format.descriptor(), context);
    applyJournal(format, context, journal);
}

} // namespace Rigel::Persistence::detail
