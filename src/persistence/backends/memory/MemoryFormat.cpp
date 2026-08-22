#include "Rigel/Persistence/Backends/Memory/MemoryFormat.h"

#include "Rigel/Persistence/Storage.h"
#include "Rigel/Voxel/ChunkCoord.h"
#include "Rigel/Voxel/Chunk.h"
#include "../../ChunkValidation.h"
#include "../../RegionFilename.h"
#include "../../ZoneIdentifier.h"

#include <bit>
#include <filesystem>
#include <stdexcept>
#include <utility>
#include <vector>

#include <glm/vec3.hpp>

namespace Rigel::Persistence::Backends::Memory {

namespace {

constexpr int32_t kMemoryRegionSpan = 16;
constexpr uint32_t kMaxMemoryStringBytes = 1'048'576;
constexpr uint32_t kMaxChunksPerRegion =
    kMemoryRegionSpan * kMemoryRegionSpan * kMemoryRegionSpan;
constexpr uint32_t kMaxEntitiesPerChunk = 1'048'576;
constexpr size_t kEncodedChunkHeaderBytes = 13 * sizeof(uint32_t);
constexpr size_t kEncodedBlockStateBytes =
    sizeof(uint16_t) + 2 * sizeof(uint8_t);
constexpr size_t kMinEncodedChunkBytes =
    kEncodedChunkHeaderBytes + kEncodedBlockStateBytes;
constexpr size_t kEncodedEntityChunkHeaderBytes = 4 * sizeof(uint32_t);
constexpr size_t kMinEncodedEntityBytes = 15 * sizeof(uint32_t);

size_t remainingInput(const ByteReader& reader) {
    const size_t position = reader.tell();
    const size_t size = reader.size();
    if (position > size) {
        throw std::runtime_error("MemoryFormat: invalid reader position");
    }
    return size - position;
}

void requireRemaining(const ByteReader& reader,
                      size_t required,
                      const char* diagnostic) {
    if (required > remainingInput(reader)) {
        throw std::runtime_error(diagnostic);
    }
}

void validateCollectionCount(const ByteReader& reader,
                             uint32_t count,
                             uint32_t limit,
                             size_t minimumElementBytes,
                             const char* limitDiagnostic,
                             const char* remainingDiagnostic) {
    if (count > limit) {
        throw std::runtime_error(limitDiagnostic);
    }
    if (count > remainingInput(reader) / minimumElementBytes) {
        throw std::runtime_error(remainingDiagnostic);
    }
}

void validateCollectionSize(size_t count,
                            uint32_t limit,
                            const char* diagnostic) {
    if (count > limit) {
        throw std::runtime_error(diagnostic);
    }
}

void validateStringSize(const std::string& value) {
    if (value.size() > kMaxMemoryStringBytes) {
        throw std::runtime_error(
            "MemoryFormat: string length exceeds format limit");
    }
}

std::string canonicalZoneRoot(const PersistenceContext& context,
                              const std::string& zoneId) {
    return context.rootPath + "/zones/" + detail::zoneIdentifierStoragePath(zoneId);
}

#ifndef _WIN32
std::string previousZoneRoot(const PersistenceContext& context,
                             const std::string& zoneId) {
    return context.rootPath + "/zones/" + zoneId;
}

bool hasStoredZone(StorageBackend& storage, const std::string& root) {
    if (storage.exists(root)) {
        return true;
    }
    const auto childPrefix = root + "/";
    for (const auto& entry : storage.list(root)) {
        if (entry == root || entry.rfind(childPrefix, 0) == 0) {
            return true;
        }
    }
    return false;
}
#endif

std::string zoneRoot(StorageBackend& storage,
                     const PersistenceContext& context,
                     const std::string& zoneId) {
    detail::validateZoneIdentifier(zoneId);
    const auto canonicalRoot = canonicalZoneRoot(context, zoneId);

#ifdef _WIN32
    return canonicalRoot;
#else
    if (zoneId.find(':') == std::string::npos) {
        return canonicalRoot;
    }

    const auto legacyRoot = previousZoneRoot(context, zoneId);
    const bool hasCanonicalData = hasStoredZone(storage, canonicalRoot);
    const bool hasLegacyData = hasStoredZone(storage, legacyRoot);
    if (hasCanonicalData && hasLegacyData) {
        throw std::runtime_error(
            "MemoryFormat configuration error: zone identifier '" + zoneId +
            "' has data in both its current and previous storage layouts; "
            "consolidate the zone under '" + canonicalRoot +
            "' before continuing");
    }
    return hasLegacyData ? legacyRoot : canonicalRoot;
#endif
}

std::string parentPath(const std::string& path) {
    auto pos = path.find_last_of('/');
    if (pos == std::string::npos) {
        return std::string();
    }
    return path.substr(0, pos);
}

std::string regionPath(StorageBackend& storage,
                       const PersistenceContext& context,
                       const RegionKey& key) {
    return zoneRoot(storage, context, key.zoneId) + "/regions/region_" + std::to_string(key.x) + "_" +
        std::to_string(key.y) + "_" + std::to_string(key.z) + ".mem";
}

std::string entityRegionPath(StorageBackend& storage,
                             const PersistenceContext& context,
                             const EntityRegionKey& key) {
    return zoneRoot(storage, context, key.zoneId) + "/entities/entityRegion_" + std::to_string(key.x) + "_" +
        std::to_string(key.y) + "_" + std::to_string(key.z) + ".mem";
}

void writeString(ByteWriter& writer, const std::string& value) {
    validateStringSize(value);
    writer.writeU32(static_cast<uint32_t>(value.size()));
    if (!value.empty()) {
        writer.writeBytes(reinterpret_cast<const uint8_t*>(value.data()), value.size());
    }
}

std::string readString(ByteReader& reader) {
    requireRemaining(
        reader, sizeof(uint32_t),
        "MemoryFormat: truncated string length");
    uint32_t len = reader.readU32();
    if (len > kMaxMemoryStringBytes) {
        throw std::runtime_error(
            "MemoryFormat: string length exceeds format limit");
    }
    if (len > remainingInput(reader)) {
        throw std::runtime_error(
            "MemoryFormat: string length exceeds remaining input");
    }
    std::string out;
    if (len == 0) {
        return out;
    }
    out.resize(len);
    reader.readBytes(reinterpret_cast<uint8_t*>(&out[0]), len);
    return out;
}

void writeF32(ByteWriter& writer, float value) {
    uint32_t bits = std::bit_cast<uint32_t>(value);
    writer.writeU32(bits);
}

float readF32(ByteReader& reader) {
    uint32_t bits = reader.readU32();
    return std::bit_cast<float>(bits);
}

void writeU64(ByteWriter& writer, uint64_t value) {
    writer.writeU32(static_cast<uint32_t>((value >> 32) & 0xFFFFFFFFu));
    writer.writeU32(static_cast<uint32_t>(value & 0xFFFFFFFFu));
}

uint64_t readU64(ByteReader& reader) {
    uint64_t high = static_cast<uint64_t>(reader.readU32());
    uint64_t low = static_cast<uint64_t>(reader.readU32());
    return (high << 32) | low;
}

void writeVec3(ByteWriter& writer, const glm::vec3& value) {
    writeF32(writer, value.x);
    writeF32(writer, value.y);
    writeF32(writer, value.z);
}

glm::vec3 readVec3(ByteReader& reader) {
    glm::vec3 out;
    out.x = readF32(reader);
    out.y = readF32(reader);
    out.z = readF32(reader);
    return out;
}

void writeBlockState(ByteWriter& writer, const Voxel::BlockState& state) {
    writer.writeU16(state.id.type);
    writer.writeU8(state.metadata);
    writer.writeU8(state.lightLevel);
}

Voxel::BlockState readBlockState(ByteReader& reader) {
    Voxel::BlockState state;
    state.id.type = reader.readU16();
    state.metadata = reader.readU8();
    state.lightLevel = reader.readU8();
    return state;
}

void writeChunkSpan(ByteWriter& writer, const ChunkSpan& span) {
    writer.writeI32(span.chunkX);
    writer.writeI32(span.chunkY);
    writer.writeI32(span.chunkZ);
    writer.writeI32(span.offsetX);
    writer.writeI32(span.offsetY);
    writer.writeI32(span.offsetZ);
    writer.writeI32(span.sizeX);
    writer.writeI32(span.sizeY);
    writer.writeI32(span.sizeZ);
}

ChunkSpan readChunkSpan(ByteReader& reader) {
    ChunkSpan span;
    span.chunkX = reader.readI32();
    span.chunkY = reader.readI32();
    span.chunkZ = reader.readI32();
    span.offsetX = reader.readI32();
    span.offsetY = reader.readI32();
    span.offsetZ = reader.readI32();
    span.sizeX = reader.readI32();
    span.sizeY = reader.readI32();
    span.sizeZ = reader.readI32();
    return span;
}

bool parseRegionFilename(const std::string& name, int32_t& rx, int32_t& ry, int32_t& rz) {
    return detail::parseCanonicalRegionFilename(name, "region_", ".mem", rx, ry, rz);
}

bool parseEntityRegionFilename(const std::string& name, int32_t& rx, int32_t& ry, int32_t& rz) {
    return detail::parseCanonicalRegionFilename(
        name, "entityRegion_", ".mem", rx, ry, rz);
}

int32_t floorDiv(int32_t value, int32_t divisor) {
    int32_t q = value / divisor;
    int32_t r = value % divisor;
    if (r < 0) {
        q -= 1;
    }
    return q;
}

class MemoryRegionLayout final : public RegionLayout {
public:
    RegionKey regionForChunk(const std::string& zoneId, Voxel::ChunkCoord coord) const override {
        detail::validateZoneIdentifier(zoneId);
        return RegionKey{
            zoneId,
            floorDiv(coord.x, kMemoryRegionSpan),
            floorDiv(coord.y, kMemoryRegionSpan),
            floorDiv(coord.z, kMemoryRegionSpan)
        };
    }

    std::vector<ChunkKey> storageKeysForChunk(const std::string& zoneId,
                                              Voxel::ChunkCoord coord) const override {
        detail::validateZoneIdentifier(zoneId);
        return {ChunkKey{zoneId, coord.x, coord.y, coord.z}};
    }

    ChunkSpan spanForStorageKey(const ChunkKey& key) const override {
        detail::validateZoneIdentifier(key.zoneId);
        ChunkSpan span;
        span.chunkX = key.x;
        span.chunkY = key.y;
        span.chunkZ = key.z;
        span.sizeX = Voxel::Chunk::SIZE;
        span.sizeY = Voxel::Chunk::SIZE;
        span.sizeZ = Voxel::Chunk::SIZE;
        return span;
    }

};

class MemoryWorldMetadataCodec final : public WorldMetadataCodec {
public:
    std::string metadataPath(const PersistenceContext& context) const override {
        return context.rootPath + "/world.meta";
    }

    void write(const WorldMetadata& metadata, ByteWriter& writer) override {
        validateStringSize(metadata.worldId);
        validateStringSize(metadata.displayName);
        writeString(writer, metadata.worldId);
        writeString(writer, metadata.displayName);
    }

    WorldMetadata read(ByteReader& reader) override {
        WorldMetadata out;
        out.worldId = readString(reader);
        out.displayName = readString(reader);
        return out;
    }
};

class MemoryZoneMetadataCodec final : public ZoneMetadataCodec {
public:
    explicit MemoryZoneMetadataCodec(std::shared_ptr<StorageBackend> storage)
        : m_storage(std::move(storage)) {
    }

    std::string metadataPath(const ZoneKey& key, const PersistenceContext& context) const override {
        return zoneRoot(*m_storage, context, key.zoneId) + "/zone.meta";
    }

    void write(const ZoneMetadata& metadata, ByteWriter& writer) override {
        detail::validateZoneIdentifier(metadata.zoneId);
        validateStringSize(metadata.zoneId);
        validateStringSize(metadata.displayName);
        writeString(writer, metadata.zoneId);
        writeString(writer, metadata.displayName);
    }

    ZoneMetadata read(ByteReader& reader) override {
        ZoneMetadata out;
        out.zoneId = readString(reader);
        out.displayName = readString(reader);
        detail::validateZoneIdentifier(out.zoneId);
        return out;
    }

private:
    std::shared_ptr<StorageBackend> m_storage;
};

class MemoryChunkCodec final : public ChunkCodec {
public:
    void write(const ChunkSnapshot& chunk, ByteWriter& writer) override {
        detail::validateZoneIdentifier(chunk.key.zoneId);
        detail::validateChunkBlockCount(
            chunk.data.span, chunk.data.blocks.size());
        writer.writeI32(chunk.key.x);
        writer.writeI32(chunk.key.y);
        writer.writeI32(chunk.key.z);
        writeChunkSpan(writer, chunk.data.span);
        writer.writeU32(static_cast<uint32_t>(chunk.data.blocks.size()));
        for (const auto& block : chunk.data.blocks) {
            writeBlockState(writer, block);
        }
    }

    ChunkSnapshot read(ByteReader& reader, const ChunkKey& keyHint) override {
        detail::validateZoneIdentifier(keyHint.zoneId);
        requireRemaining(
            reader, kEncodedChunkHeaderBytes - sizeof(uint32_t),
            "MemoryFormat: truncated chunk header");
        ChunkSnapshot out;
        out.key = keyHint;
        out.key.x = reader.readI32();
        out.key.y = reader.readI32();
        out.key.z = reader.readI32();
        out.data.span = readChunkSpan(reader);
        detail::validateChunkSpan(out.data.span);
        requireRemaining(
            reader, sizeof(uint32_t),
            "MemoryFormat: truncated chunk block count");
        const uint32_t count = reader.readU32();
        detail::validateChunkBlockCount(out.data.span, count);
        if (count > remainingInput(reader) / kEncodedBlockStateBytes) {
            throw std::runtime_error(
                "MemoryFormat: chunk block count exceeds remaining input");
        }
        out.data.blocks.resize(count);
        for (uint32_t i = 0; i < count; ++i) {
            out.data.blocks[i] = readBlockState(reader);
        }
        return out;
    }
};

class MemoryEntityRegionCodec final : public EntityRegionCodec {
public:
    void write(const EntityRegionSnapshot& region, ByteWriter& writer) override {
        detail::validateZoneIdentifier(region.key.zoneId);
        validateCollectionSize(
            region.chunks.size(),
            kMaxChunksPerRegion,
            "MemoryFormat: entity-region chunk count exceeds format limit");
        for (const auto& chunk : region.chunks) {
            validateCollectionSize(
                chunk.entities.size(),
                kMaxEntitiesPerChunk,
                "MemoryFormat: entity count exceeds format limit");
            for (const auto& entity : chunk.entities) {
                validateStringSize(entity.typeId);
                validateStringSize(entity.modelId);
            }
        }

        writer.writeU32(static_cast<uint32_t>(region.chunks.size()));
        for (const auto& chunk : region.chunks) {
            writer.writeI32(chunk.coord.x);
            writer.writeI32(chunk.coord.y);
            writer.writeI32(chunk.coord.z);
            writer.writeU32(static_cast<uint32_t>(chunk.entities.size()));
            for (const auto& entity : chunk.entities) {
                writeString(writer, entity.typeId);
                writeU64(writer, entity.id.time);
                writer.writeU32(entity.id.random);
                writer.writeU32(entity.id.counter);
                writeVec3(writer, entity.position);
                writeVec3(writer, entity.velocity);
                writeVec3(writer, entity.viewDirection);
                writeString(writer, entity.modelId);
            }
        }
    }

    EntityRegionSnapshot read(ByteReader& reader, const EntityRegionKey& keyHint) override {
        detail::validateZoneIdentifier(keyHint.zoneId);
        EntityRegionSnapshot out;
        out.key = keyHint;
        requireRemaining(
            reader, sizeof(uint32_t),
            "MemoryFormat: truncated entity-region chunk count");
        const uint32_t chunkCount = reader.readU32();
        validateCollectionCount(
            reader,
            chunkCount,
            kMaxChunksPerRegion,
            kEncodedEntityChunkHeaderBytes,
            "MemoryFormat: entity-region chunk count exceeds format limit",
            "MemoryFormat: entity-region chunk count exceeds remaining input");
        out.chunks.reserve(chunkCount);
        for (uint32_t c = 0; c < chunkCount; ++c) {
            requireRemaining(
                reader, kEncodedEntityChunkHeaderBytes,
                "MemoryFormat: truncated entity chunk header");
            EntityPersistedChunk chunk;
            chunk.coord.x = reader.readI32();
            chunk.coord.y = reader.readI32();
            chunk.coord.z = reader.readI32();
            const uint32_t entityCount = reader.readU32();
            validateCollectionCount(
                reader,
                entityCount,
                kMaxEntitiesPerChunk,
                kMinEncodedEntityBytes,
                "MemoryFormat: entity count exceeds format limit",
                "MemoryFormat: entity count exceeds remaining input");
            chunk.entities.reserve(entityCount);
            for (uint32_t e = 0; e < entityCount; ++e) {
                EntityPersistedEntity entity;
                entity.typeId = readString(reader);
                requireRemaining(
                    reader, 14 * sizeof(uint32_t),
                    "MemoryFormat: truncated entity record");
                entity.id.time = readU64(reader);
                entity.id.random = reader.readU32();
                entity.id.counter = reader.readU32();
                entity.position = readVec3(reader);
                entity.velocity = readVec3(reader);
                entity.viewDirection = readVec3(reader);
                entity.modelId = readString(reader);
                chunk.entities.push_back(std::move(entity));
            }
            out.chunks.push_back(std::move(chunk));
        }
        return out;
    }
};

class MemoryChunkContainer final : public ChunkContainer {
public:
    MemoryChunkContainer(std::shared_ptr<StorageBackend> storage, PersistenceContext context, ChunkCodec& codec)
        : m_storage(std::move(storage)), m_context(std::move(context)), m_codec(codec) {
    }

    bool regionExists(const RegionKey& key) override {
        return m_storage->exists(regionPath(*m_storage, m_context, key));
    }

    void saveRegion(const ChunkRegionSnapshot& region) override {
        validateCollectionSize(
            region.chunks.size(),
            kMaxChunksPerRegion,
            "MemoryFormat: chunk count exceeds format limit");
        for (const auto& chunk : region.chunks) {
            detail::validateZoneIdentifier(chunk.key.zoneId);
            detail::validateChunkBlockCount(
                chunk.data.span, chunk.data.blocks.size());
        }

        auto path = regionPath(*m_storage, m_context, region.key);
        m_storage->mkdirs(parentPath(path));
        auto session = m_storage->openWrite(path);
        auto& writer = session->writer();
        writer.writeU32(static_cast<uint32_t>(region.chunks.size()));
        for (const auto& chunk : region.chunks) {
            m_codec.write(chunk, writer);
        }
        writer.flush();
        session->commit();
    }

    ChunkRegionSnapshot loadRegion(const RegionKey& key) override {
        ChunkRegionSnapshot region;
        region.key = key;
        auto path = regionPath(*m_storage, m_context, key);
        if (!m_storage->exists(path)) {
            return region;
        }
        auto reader = m_storage->openRead(path);
        requireRemaining(
            *reader, sizeof(uint32_t),
            "MemoryFormat: truncated chunk count");
        const uint32_t count = reader->readU32();
        validateCollectionCount(
            *reader,
            count,
            kMaxChunksPerRegion,
            kMinEncodedChunkBytes,
            "MemoryFormat: chunk count exceeds format limit",
            "MemoryFormat: chunk count exceeds remaining input");
        region.chunks.reserve(count);
        ChunkKey hint{key.zoneId, 0, 0, 0};
        for (uint32_t i = 0; i < count; ++i) {
            region.chunks.push_back(m_codec.read(*reader, hint));
        }
        return region;
    }

    std::vector<RegionKey> listRegions(const std::string& zoneId) override {
        std::vector<RegionKey> regions;
        std::string dir = zoneRoot(*m_storage, m_context, zoneId) + "/regions";
        if (!m_storage->exists(dir)) {
            return regions;
        }
        for (const auto& entry : m_storage->list(dir)) {
            std::string name = std::filesystem::path(entry).filename().string();
            int32_t rx = 0;
            int32_t ry = 0;
            int32_t rz = 0;
            if (!parseRegionFilename(name, rx, ry, rz)) {
                continue;
            }
            regions.push_back(RegionKey{zoneId, rx, ry, rz});
        }
        return regions;
    }

private:
    std::shared_ptr<StorageBackend> m_storage;
    PersistenceContext m_context;
    ChunkCodec& m_codec;
};

class MemoryEntityContainer final : public EntityContainer {
public:
    MemoryEntityContainer(std::shared_ptr<StorageBackend> storage, PersistenceContext context, EntityRegionCodec& codec)
        : m_storage(std::move(storage)), m_context(std::move(context)), m_codec(codec) {
    }

    void saveRegion(const EntityRegionSnapshot& region) override {
        auto path = entityRegionPath(*m_storage, m_context, region.key);
        m_storage->mkdirs(parentPath(path));
        auto session = m_storage->openWrite(path);
        m_codec.write(region, session->writer());
        session->writer().flush();
        session->commit();
    }

    void removeRegion(const EntityRegionKey& key) override {
        auto path = entityRegionPath(*m_storage, m_context, key);
        m_storage->remove(path);
    }

    EntityRegionSnapshot loadRegion(const EntityRegionKey& key) override {
        EntityRegionSnapshot empty;
        empty.key = key;
        auto path = entityRegionPath(*m_storage, m_context, key);
        if (!m_storage->exists(path)) {
            return empty;
        }
        auto reader = m_storage->openRead(path);
        return m_codec.read(*reader, key);
    }

    std::vector<EntityRegionKey> listRegions(const std::string& zoneId) override {
        std::vector<EntityRegionKey> regions;
        std::string dir = zoneRoot(*m_storage, m_context, zoneId) + "/entities";
        if (!m_storage->exists(dir)) {
            return regions;
        }
        for (const auto& entry : m_storage->list(dir)) {
            std::string name = std::filesystem::path(entry).filename().string();
            int32_t rx = 0;
            int32_t ry = 0;
            int32_t rz = 0;
            if (!parseEntityRegionFilename(name, rx, ry, rz)) {
                continue;
            }
            regions.push_back(EntityRegionKey{zoneId, rx, ry, rz});
        }
        return regions;
    }

private:
    std::shared_ptr<StorageBackend> m_storage;
    PersistenceContext m_context;
    EntityRegionCodec& m_codec;
};

class MemoryFormat final : public PersistenceFormat {
public:
    MemoryFormat(std::shared_ptr<StorageBackend> storage, const PersistenceContext& context)
        : m_storage(std::move(storage)),
          m_context(context),
          m_zoneCodec(m_storage),
          m_chunkContainer(m_storage, m_context, m_chunkCodec),
          m_entityContainer(m_storage, m_context, m_entityCodec) {
    }

    const FormatDescriptor& descriptor() const override {
        return Backends::Memory::descriptor();
    }

    WorldMetadataCodec& worldMetadataCodec() override {
        return m_worldCodec;
    }

    ZoneMetadataCodec& zoneMetadataCodec() override {
        return m_zoneCodec;
    }

    ChunkContainer& chunkContainer() override {
        return m_chunkContainer;
    }

    EntityContainer& entityContainer() override {
        return m_entityContainer;
    }

    RegionLayout& regionLayout() override {
        return m_layout;
    }

private:
    std::shared_ptr<StorageBackend> m_storage;
    PersistenceContext m_context;
    MemoryWorldMetadataCodec m_worldCodec;
    MemoryZoneMetadataCodec m_zoneCodec;
    MemoryRegionLayout m_layout;
    MemoryChunkCodec m_chunkCodec;
    MemoryEntityRegionCodec m_entityCodec;
    MemoryChunkContainer m_chunkContainer;
    MemoryEntityContainer m_entityContainer;
};

} // namespace

const FormatDescriptor& descriptor() {
    static FormatDescriptor desc = []() {
        FormatDescriptor init;
        init.id = "memory";
        init.version = 1;
        init.extensions = {"mem"};
        init.capabilities.supportsPartialChunkSave = true;
        init.capabilities.supportsRandomAccess = false;
        init.capabilities.supportsEntityRegions = true;
        init.capabilities.supportsVersions = true;
        init.capabilities.fillMissingChunkSpans = true;
        init.capabilities.metadataFormat = "binary";
        init.capabilities.regionIndexType = "none";
        return init;
    }();
    return desc;
}

FormatFactory factory() {
    return [](const PersistenceContext& context) -> std::unique_ptr<PersistenceFormat> {
        if (!context.storage) {
            throw std::runtime_error("MemoryFormat: storage backend is required");
        }
        return std::make_unique<MemoryFormat>(context.storage, context);
    };
}

FormatProbe probe() {
    return [](StorageBackend&, const PersistenceContext&) -> std::optional<ProbeResult> {
        return std::nullopt;
    };
}

} // namespace Rigel::Persistence::Backends::Memory
