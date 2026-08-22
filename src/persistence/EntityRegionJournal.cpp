#include "EntityRegionJournal.h"

#include "Rigel/Entity/EntityPersistence.h"
#include "Rigel/Persistence/Format.h"
#include "Rigel/Persistence/Storage.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Rigel::Persistence::detail {
namespace {

constexpr uint32_t kJournalMagic = 0x5247454A; // "RGEJ"
constexpr uint16_t kJournalVersion = 1;
constexpr const char* kJournalFilename = "entity-regions.journal";

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

void writeString(ByteWriter& writer, const std::string& value) {
    if (value.size() > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error("Entity region journal string is too large");
    }
    writer.writeU32(static_cast<uint32_t>(value.size()));
    if (!value.empty()) {
        writer.writeBytes(
            reinterpret_cast<const uint8_t*>(value.data()), value.size());
    }
}

std::string readString(ByteReader& reader) {
    const uint32_t size = reader.readU32();
    if (reader.tell() > reader.size() || size > reader.size() - reader.tell()) {
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

void writeJournal(ByteWriter& writer, const EntityRegionJournal& journal) {
    if (journal.desiredRegions.size() > std::numeric_limits<uint32_t>::max() ||
        journal.obsoleteRegions.size() > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error("Entity region journal has too many regions");
    }

    writer.writeU32(kJournalMagic);
    writer.writeU16(kJournalVersion);
    writer.writeU16(0);
    writer.writeU32(static_cast<uint32_t>(journal.desiredRegions.size()));
    writer.writeU32(static_cast<uint32_t>(journal.obsoleteRegions.size()));

    for (const auto& region : journal.desiredRegions) {
        writeKey(writer, region.key);
        const auto payload = Entity::encodeEntityRegionPayload(region.chunks);
        if (payload.size() > std::numeric_limits<uint32_t>::max()) {
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

EntityRegionJournal readJournal(ByteReader& reader) {
    if (reader.readU32() != kJournalMagic) {
        throw std::runtime_error("Invalid entity region journal magic");
    }
    if (reader.readU16() != kJournalVersion) {
        throw std::runtime_error("Unsupported entity region journal version");
    }
    reader.readU16();

    EntityRegionJournal journal;
    const uint32_t desiredCount = reader.readU32();
    const uint32_t obsoleteCount = reader.readU32();
    journal.desiredRegions.reserve(desiredCount);
    journal.obsoleteRegions.reserve(obsoleteCount);

    for (uint32_t i = 0; i < desiredCount; ++i) {
        EntityRegionSnapshot region;
        region.key = readKey(reader);
        const uint32_t payloadSize = reader.readU32();
        if (reader.tell() > reader.size() ||
            payloadSize > reader.size() - reader.tell()) {
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
                    const PersistenceContext& context) {
    const std::string path = journalPath(context);
    context.storage->mkdirs(parentPath(path));
    auto session = context.storage->openWrite(path, AtomicWriteOptions{});
    writeJournal(session->writer(), journal);
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

} // namespace

void validateEntityRegionSnapshots(
    const std::vector<EntityRegionSnapshot>& regions) {
    std::unordered_map<Entity::EntityId,
                       EntityRegionKey,
                       Entity::EntityIdHash> sources;
    for (const auto& region : regions) {
        for (const auto& chunk : region.chunks) {
            for (const auto& entity : chunk.entities) {
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
    const PersistenceContext& context) {
    const std::string path = journalPath(context);
    if (!context.storage->exists(path)) {
        return;
    }

    try {
        auto reader = context.storage->openRead(path);
        const EntityRegionJournal journal = readJournal(*reader);
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
    replayEntityRegionJournal(format, context);

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
    publishJournal(journal, context);
    applyJournal(format, context, journal);
}

} // namespace Rigel::Persistence::detail
