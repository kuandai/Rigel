#pragma once

#include "Rigel/Persistence/Types.h"

#include <string>
#include <vector>

namespace Rigel::Persistence {

class PersistenceFormat;

namespace detail {

void validateEntityRegionSnapshots(
    const std::vector<EntityRegionSnapshot>& regions);

void replayEntityRegionJournal(
    PersistenceFormat& format,
    const PersistenceContext& context,
    const std::string& zoneId);

void saveEntityRegionsRecoverably(
    PersistenceFormat& format,
    const PersistenceContext& context,
    const std::string& zoneId,
    std::vector<EntityRegionSnapshot> desiredRegions);

} // namespace detail
} // namespace Rigel::Persistence
