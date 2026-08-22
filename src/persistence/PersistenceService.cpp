#include "Rigel/Persistence/PersistenceService.h"
#include "Rigel/Persistence/Storage.h"
#include "ZoneIdentifier.h"

#include <algorithm>
#include <spdlog/spdlog.h>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace Rigel::Persistence {

namespace {

class MetadataBufferWriter final : public ByteWriter {
public:
    explicit MetadataBufferWriter(std::vector<uint8_t>& payload)
        : m_payload(payload) {
    }

    void writeU8(uint8_t value) override {
        writeBytes(&value, sizeof(value));
    }

    void writeU16(uint16_t value) override {
        const uint8_t bytes[] = {
            static_cast<uint8_t>((value >> 8) & 0xFF),
            static_cast<uint8_t>(value & 0xFF)
        };
        writeBytes(bytes, sizeof(bytes));
    }

    void writeU32(uint32_t value) override {
        const uint8_t bytes[] = {
            static_cast<uint8_t>((value >> 24) & 0xFF),
            static_cast<uint8_t>((value >> 16) & 0xFF),
            static_cast<uint8_t>((value >> 8) & 0xFF),
            static_cast<uint8_t>(value & 0xFF)
        };
        writeBytes(bytes, sizeof(bytes));
    }

    void writeI32(int32_t value) override {
        writeU32(static_cast<uint32_t>(value));
    }

    void writeBytes(const uint8_t* src, size_t len) override {
        requireCapacity(m_position, len);
        if (len == 0) {
            return;
        }
        if (m_position + len > m_payload.size()) {
            m_payload.resize(m_position + len, 0);
        }
        std::copy_n(src, len, m_payload.data() + m_position);
        m_position += len;
    }

    size_t size() const override {
        return m_payload.size();
    }

    size_t tell() const override {
        return m_position;
    }

    void seek(size_t offset) override {
        requireCapacity(offset, 0);
        if (offset > m_payload.size()) {
            m_payload.resize(offset, 0);
        }
        m_position = offset;
    }

    void writeAt(size_t offset, const uint8_t* src, size_t len) override {
        requireCapacity(offset, len);
        if (len == 0) {
            return;
        }
        if (offset + len > m_payload.size()) {
            m_payload.resize(offset + len, 0);
        }
        std::copy_n(src, len, m_payload.data() + offset);
    }

    void flush() override {
    }

private:
    void requireCapacity(size_t offset, size_t len) const {
        if (offset > m_payload.max_size() ||
            len > m_payload.max_size() - offset) {
            throw std::length_error("Metadata payload exceeds staging capacity");
        }
    }

    std::vector<uint8_t>& m_payload;
    size_t m_position = 0;
};

std::string parentPath(const std::string& path) {
    auto pos = path.find_last_of('/');
    if (pos == std::string::npos) {
        return std::string();
    }
    return path.substr(0, pos);
}

template <typename Codec, typename Metadata>
std::vector<uint8_t> encodeMetadata(Codec& codec, const Metadata& metadata) {
    std::vector<uint8_t> payload;
    MetadataBufferWriter writer(payload);
    codec.write(metadata, writer);
    writer.flush();
    return payload;
}

void publishMetadata(StorageBackend& storage,
                     const std::vector<uint8_t>& payload,
                     const std::string& path) {
    storage.mkdirs(parentPath(path));
    auto session = storage.openWrite(path, AtomicWriteOptions{});
    if (!payload.empty()) {
        session->writer().writeBytes(payload.data(), payload.size());
    }
    session->writer().flush();
    session->commit();
}

template <typename Codec, typename Metadata>
void writeMetadata(StorageBackend& storage,
                   Codec& codec,
                   const Metadata& metadata,
                   const std::string& path) {
    publishMetadata(storage, encodeMetadata(codec, metadata), path);
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

    auto worldPayload = encodeMetadata(worldCodec, snapshot.metadata);
    std::vector<std::vector<uint8_t>> zonePayloads;
    zonePayloads.reserve(snapshot.zones.size());
    for (const auto& zone : snapshot.zones) {
        zonePayloads.push_back(encodeMetadata(zoneCodec, zone));
    }

    publishMetadata(*context.storage, worldPayload, worldPath);

    for (size_t index = 0; index < snapshot.zones.size(); ++index) {
        publishMetadata(
            *context.storage,
            zonePayloads[index],
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
