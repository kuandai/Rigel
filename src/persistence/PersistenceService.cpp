#include "Rigel/Persistence/PersistenceService.h"
#include "Rigel/Persistence/Storage.h"

#include <spdlog/spdlog.h>
#include <stdexcept>
#include <string>

namespace Rigel::Persistence {

namespace {

std::string parentPath(const std::string& path) {
    auto pos = path.find_last_of('/');
    if (pos == std::string::npos) {
        return std::string();
    }
    return path.substr(0, pos);
}

} // namespace

PersistenceService::PersistenceService(FormatRegistry& registry)
    : m_registry(registry) {
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

void PersistenceService::saveWorld(const WorldSnapshot& snapshot, SaveScope scope, const PersistenceContext& context) {
    auto format = resolve(context);

    if (includesMetadata(scope)) {
        auto& codec = format->worldMetadataCodec();
        auto path = codec.metadataPath(context);
        context.storage->mkdirs(parentPath(path));
        auto session = context.storage->openWrite(path, AtomicWriteOptions{});
        codec.write(snapshot.metadata, session->writer());
        session->writer().flush();
        session->commit();

        for (const auto& zoneMeta : snapshot.zones) {
            ZoneSnapshot zoneSnapshot;
            zoneSnapshot.metadata = zoneMeta;
            saveZone(zoneSnapshot, SaveScope::MetadataOnly, context);
        }
    }

    if (includesChunks(scope) || includesEntities(scope)) {
        handleUnsupportedFeature(context, "saveWorld: payload saves must be handled per-zone");
    }
}

WorldMetadata PersistenceService::loadWorldMetadata(const PersistenceContext& context) {
    auto format = resolve(context);
    auto& codec = format->worldMetadataCodec();
    auto path = codec.metadataPath(context);
    auto reader = context.storage->openRead(path);
    return codec.read(*reader);
}

void PersistenceService::saveZone(const ZoneSnapshot& snapshot, SaveScope scope, const PersistenceContext& context) {
    auto format = resolve(context);

    if (includesMetadata(scope)) {
        auto& codec = format->zoneMetadataCodec();
        ZoneKey key{snapshot.metadata.zoneId};
        auto path = codec.metadataPath(key, context);
        context.storage->mkdirs(parentPath(path));
        auto session = context.storage->openWrite(path, AtomicWriteOptions{});
        codec.write(snapshot.metadata, session->writer());
        session->writer().flush();
        session->commit();
    }

    if (includesChunks(scope)) {
        for (const auto& regionKey : snapshot.regions) {
            ChunkRegionSnapshot region;
            region.key = regionKey;
            format->chunkContainer().saveRegion(region);
        }
    }

    if (includesEntities(scope)) {
        if (!format->descriptor().capabilities.supportsEntityRegions) {
            handleUnsupportedFeature(context, "saveZone: entity regions not supported by format");
            return;
        }
        for (const auto& entityKey : snapshot.entityRegions) {
            EntityRegionSnapshot region;
            region.key = entityKey;
            format->entityContainer().saveRegion(region);
        }
    }
}

ZoneMetadata PersistenceService::loadZoneMetadata(const ZoneKey& key, const PersistenceContext& context) {
    auto format = resolve(context);
    auto& codec = format->zoneMetadataCodec();
    auto path = codec.metadataPath(key, context);
    auto reader = context.storage->openRead(path);
    return codec.read(*reader);
}

void PersistenceService::saveRegion(const ChunkRegionSnapshot& region, const PersistenceContext& context) {
    auto format = resolve(context);
    format->chunkContainer().saveRegion(region);
}

ChunkRegionSnapshot PersistenceService::loadRegion(const RegionKey& key, const PersistenceContext& context) {
    auto format = resolve(context);
    return format->chunkContainer().loadRegion(key);
}

void PersistenceService::saveEntities(const EntityRegionSnapshot& region, const PersistenceContext& context) {
    auto format = resolve(context);
    if (!format->descriptor().capabilities.supportsEntityRegions) {
        handleUnsupportedFeature(context, "saveEntities: entity regions not supported by format");
        return;
    }
    format->entityContainer().saveRegion(region);
}

EntityRegionSnapshot PersistenceService::loadEntities(const EntityRegionKey& key, const PersistenceContext& context) {
    auto format = resolve(context);
    if (!format->descriptor().capabilities.supportsEntityRegions) {
        handleUnsupportedFeature(context, "loadEntities: entity regions not supported by format");
        return EntityRegionSnapshot{key, {}};
    }
    return format->entityContainer().loadRegion(key);
}

} // namespace Rigel::Persistence
