#pragma once

#include "Rigel/Persistence/Types.h"

#include <cstddef>
#include <string>
#include <vector>

namespace Rigel::Persistence {

struct FormatDescriptor;
class PersistenceFormat;

namespace detail {

struct EntityRegionJournalUsage {
    size_t encodedBytes = 0;
    size_t payloadBytes = 0;
    size_t regions = 0;
    size_t chunks = 0;
    size_t entities = 0;
};

struct EntityRegionJournalPlan {
    std::vector<EntityRegionSnapshot> desiredRegions;
    std::vector<EntityRegionKey> obsoleteRegions;
    EntityRegionJournalUsage usage;
};

EntityRegionJournalUsage beginEntityRegionJournalUsage(
    const FormatDescriptor& format);

size_t accountDesiredEntityRegion(
    EntityRegionJournalUsage& usage,
    const EntityRegionKey& key);

void accountEntityRegionChunk(
    EntityRegionJournalUsage& usage,
    size_t& regionPayloadBytes,
    size_t existingRegionChunks);

void accountEntityRegionEntity(
    EntityRegionJournalUsage& usage,
    size_t& regionPayloadBytes,
    size_t existingChunkEntities,
    size_t encodedEntityBytes);

void validateEntityRegionSnapshots(
    const std::vector<EntityRegionSnapshot>& regions);

void replayEntityRegionJournal(
    PersistenceFormat& format,
    const PersistenceContext& context,
    const std::string& zoneId);

EntityRegionJournalPlan prepareEntityRegionJournal(
    PersistenceFormat& format,
    const std::string& zoneId,
    std::vector<EntityRegionSnapshot> desiredRegions);

void publishAndApplyEntityRegionJournal(
    PersistenceFormat& format,
    const PersistenceContext& context,
    const EntityRegionJournalPlan& plan);

} // namespace detail
} // namespace Rigel::Persistence
