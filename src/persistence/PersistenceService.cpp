#include "Rigel/Persistence/PersistenceService.h"
#include "Rigel/Persistence/Storage.h"
#include "ZoneIdentifier.h"

#include <spdlog/spdlog.h>
#include <stdexcept>
#include <string>
#include <vector>

namespace Rigel::Persistence {

namespace {

std::string parentPath(const std::string& path) {
    auto pos = path.find_last_of('/');
    if (pos == std::string::npos) {
        return std::string();
    }
    return path.substr(0, pos);
}

template <typename Codec, typename Metadata>
void writeMetadata(StorageBackend& storage,
                   Codec& codec,
                   const Metadata& metadata,
                   const std::string& path) {
    storage.mkdirs(parentPath(path));
    auto session = storage.openWrite(path, AtomicWriteOptions{});
    codec.write(metadata, session->writer());
    session->writer().flush();
    session->commit();
}

} // namespace

PersistenceService::PersistenceService(FormatRegistry& registry)
    : m_registry(registry) {
}

std::unique_ptr<PersistenceFormat> PersistenceService::openFormat(const PersistenceContext& context) const {
    return resolve(context);
}

std::unique_ptr<PersistenceFormat> PersistenceService::resolve(const PersistenceContext& context) const {
    return m_registry.resolveFormat(context);
}

void PersistenceService::handleUnsupportedFeature(const PersistenceContext& context, const std::string& message) const {
    switch (context.policies.unsupportedFeaturePolicy) {
    case UnsupportedFeaturePolicy::Fail:
        throw std::runtime_error(message);
    case UnsupportedFeaturePolicy::Warn:
        spdlog::warn("{}", message);
        break;
    case UnsupportedFeaturePolicy::NoOp:
        break;
    }
}

void PersistenceService::saveWorld(const WorldSnapshot& snapshot, const PersistenceContext& context) {
    for (const auto& zone : snapshot.zones) {
        detail::validateZoneIdentifier(zone.zoneId);
    }
    auto format = resolve(context);

    auto& worldCodec = format->worldMetadataCodec();
    auto& zoneCodec = format->zoneMetadataCodec();
    const auto worldPath = worldCodec.metadataPath(context);
    std::vector<std::string> zonePaths;
    zonePaths.reserve(snapshot.zones.size());
    for (const auto& zone : snapshot.zones) {
        zonePaths.push_back(
            zoneCodec.metadataPath(ZoneKey{zone.zoneId}, context));
    }

    writeMetadata(*context.storage, worldCodec, snapshot.metadata, worldPath);

    for (size_t index = 0; index < snapshot.zones.size(); ++index) {
        writeMetadata(
            *context.storage,
            zoneCodec,
            snapshot.zones[index],
            zonePaths[index]);
    }
}

WorldMetadata PersistenceService::loadWorldMetadata(const PersistenceContext& context) {
    auto format = resolve(context);
    auto& codec = format->worldMetadataCodec();
    auto path = codec.metadataPath(context);
    auto reader = context.storage->openRead(path);
    return codec.read(*reader);
}

void PersistenceService::saveZoneMetadata(const ZoneMetadata& metadata, const PersistenceContext& context) {
    detail::validateZoneIdentifier(metadata.zoneId);
    auto format = resolve(context);
    auto& codec = format->zoneMetadataCodec();
    ZoneKey key{metadata.zoneId};
    auto path = codec.metadataPath(key, context);
    writeMetadata(*context.storage, codec, metadata, path);
}

ZoneMetadata PersistenceService::loadZoneMetadata(const ZoneKey& key, const PersistenceContext& context) {
    detail::validateZoneIdentifier(key.zoneId);
    auto format = resolve(context);
    auto& codec = format->zoneMetadataCodec();
    auto path = codec.metadataPath(key, context);
    auto reader = context.storage->openRead(path);
    return codec.read(*reader);
}

void PersistenceService::saveRegion(const ChunkRegionSnapshot& region, const PersistenceContext& context) {
    detail::validateZoneIdentifier(region.key.zoneId);
    for (const auto& chunk : region.chunks) {
        detail::validateZoneIdentifier(chunk.key.zoneId);
    }
    auto format = resolve(context);
    format->chunkContainer().saveRegion(region);
}

ChunkRegionSnapshot PersistenceService::loadRegion(const RegionKey& key, const PersistenceContext& context) {
    detail::validateZoneIdentifier(key.zoneId);
    auto format = resolve(context);
    return format->chunkContainer().loadRegion(key);
}

void PersistenceService::saveEntities(const EntityRegionSnapshot& region, const PersistenceContext& context) {
    detail::validateZoneIdentifier(region.key.zoneId);
    auto format = resolve(context);
    if (!format->descriptor().capabilities.supportsEntityRegions) {
        handleUnsupportedFeature(context, "saveEntities: entity regions not supported by format");
        return;
    }
    format->entityContainer().saveRegion(region);
}

EntityRegionSnapshot PersistenceService::loadEntities(const EntityRegionKey& key, const PersistenceContext& context) {
    detail::validateZoneIdentifier(key.zoneId);
    auto format = resolve(context);
    if (!format->descriptor().capabilities.supportsEntityRegions) {
        handleUnsupportedFeature(context, "loadEntities: entity regions not supported by format");
        return EntityRegionSnapshot{key, {}};
    }
    return format->entityContainer().loadRegion(key);
}

} // namespace Rigel::Persistence
