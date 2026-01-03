#pragma once

#include "Rigel/Persistence/Types.h"

#include <string>

namespace Rigel::Persistence::Backends::CR {

struct CRPaths {
    static std::string worldInfoPath(const PersistenceContext& context);
    static std::string zoneInfoPath(const ZoneKey& key, const PersistenceContext& context);
    static std::string regionPath(const RegionKey& key, const PersistenceContext& context);
    static std::string entityRegionPath(const EntityRegionKey& key, const PersistenceContext& context);
    static std::string playersPath(const PersistenceContext& context);
    static std::string zoneRoot(const std::string& zoneId, const PersistenceContext& context);
    static std::string normalizeZoneId(const std::string& zoneId);
};

} // namespace Rigel::Persistence::Backends::CR
