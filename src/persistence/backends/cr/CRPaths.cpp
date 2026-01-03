#include "Rigel/Persistence/Backends/CR/CRPaths.h"

namespace Rigel::Persistence::Backends::CR {

namespace {

std::string joinPath(const std::string& base, const std::string& suffix) {
    if (base.empty()) {
        return suffix;
    }
    if (base.back() == '/') {
        return base + suffix;
    }
    return base + "/" + suffix;
}

} // namespace

std::string CRPaths::worldInfoPath(const PersistenceContext& context) {
    return joinPath(context.rootPath, "worldInfo.json");
}

std::string CRPaths::zoneRoot(const std::string& zoneId, const PersistenceContext& context) {
    return joinPath(joinPath(context.rootPath, "zones"), normalizeZoneId(zoneId));
}

std::string CRPaths::zoneInfoPath(const ZoneKey& key, const PersistenceContext& context) {
    return joinPath(zoneRoot(key.zoneId, context), "zoneInfo.json");
}

std::string CRPaths::regionPath(const RegionKey& key, const PersistenceContext& context) {
    return joinPath(joinPath(zoneRoot(key.zoneId, context), "regions"),
        "region_" + std::to_string(key.x) + "_" + std::to_string(key.y) + "_" + std::to_string(key.z) + ".cosmicreach");
}

std::string CRPaths::entityRegionPath(const EntityRegionKey& key, const PersistenceContext& context) {
    return joinPath(joinPath(zoneRoot(key.zoneId, context), "entities"),
        "entityRegion_" + std::to_string(key.x) + "_" + std::to_string(key.y) + "_" + std::to_string(key.z) + ".crbin");
}

std::string CRPaths::playersPath(const PersistenceContext& context) {
    return joinPath(joinPath(context.rootPath, "players"), "localPlayer.json");
}

std::string CRPaths::normalizeZoneId(const std::string& zoneId) {
    std::string out = zoneId;
    for (auto& ch : out) {
        if (ch == ':') {
            ch = '/';
        }
    }
    return out;
}

} // namespace Rigel::Persistence::Backends::CR
