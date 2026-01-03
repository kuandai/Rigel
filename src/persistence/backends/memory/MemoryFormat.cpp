#include "Rigel/Persistence/Backends/Memory/MemoryFormat.h"

#include "Rigel/Persistence/Storage.h"

#include <stdexcept>
#include <utility>

namespace Rigel::Persistence::Backends::Memory {

namespace {

std::string zoneRoot(const PersistenceContext& context, const std::string& zoneId) {
    return context.rootPath + "/zones/" + zoneId;
}

std::string parentPath(const std::string& path) {
    auto pos = path.find_last_of('/');
    if (pos == std::string::npos) {
        return std::string();
    }
    return path.substr(0, pos);
}

std::string regionPath(const PersistenceContext& context, const RegionKey& key) {
    return zoneRoot(context, key.zoneId) + "/regions/region_" + std::to_string(key.x) + "_" +
        std::to_string(key.y) + "_" + std::to_string(key.z) + ".mem";
}

std::string entityRegionPath(const PersistenceContext& context, const EntityRegionKey& key) {
    return zoneRoot(context, key.zoneId) + "/entities/entityRegion_" + std::to_string(key.x) + "_" +
        std::to_string(key.y) + "_" + std::to_string(key.z) + ".mem";
}

std::string chunkPath(const PersistenceContext& context, const ChunkKey& key) {
    return zoneRoot(context, key.zoneId) + "/chunks/chunk_" + std::to_string(key.x) + "_" +
        std::to_string(key.y) + "_" + std::to_string(key.z) + ".mem";
}

void writeString(ByteWriter& writer, const std::string& value) {
    writer.writeU32(static_cast<uint32_t>(value.size()));
    if (!value.empty()) {
        writer.writeBytes(reinterpret_cast<const uint8_t*>(value.data()), value.size());
    }
}

std::string readString(ByteReader& reader) {
    uint32_t len = reader.readU32();
    std::string out;
    if (len == 0) {
        return out;
    }
    out.resize(len);
    reader.readBytes(reinterpret_cast<uint8_t*>(&out[0]), len);
    return out;
}

class MemoryWorldMetadataCodec final : public WorldMetadataCodec {
public:
    std::string metadataPath(const PersistenceContext& context) const override {
        return context.rootPath + "/world.meta";
    }

    void write(const WorldMetadata& metadata, ByteWriter& writer) override {
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
    std::string metadataPath(const ZoneKey& key, const PersistenceContext& context) const override {
        return zoneRoot(context, key.zoneId) + "/zone.meta";
    }

    void write(const ZoneMetadata& metadata, ByteWriter& writer) override {
        writeString(writer, metadata.zoneId);
        writeString(writer, metadata.displayName);
    }

    ZoneMetadata read(ByteReader& reader) override {
        ZoneMetadata out;
        out.zoneId = readString(reader);
        out.displayName = readString(reader);
        return out;
    }
};

class MemoryChunkCodec final : public ChunkCodec {
public:
    void write(const ChunkSnapshot& chunk, ByteWriter& writer) override {
        writer.writeI32(chunk.key.x);
        writer.writeI32(chunk.key.y);
        writer.writeI32(chunk.key.z);
        writer.writeU32(static_cast<uint32_t>(chunk.payload.size()));
        if (!chunk.payload.empty()) {
            writer.writeBytes(chunk.payload.data(), chunk.payload.size());
        }
    }

    ChunkSnapshot read(ByteReader& reader, const ChunkKey& keyHint) override {
        ChunkSnapshot out;
        out.key = keyHint;
        out.key.x = reader.readI32();
        out.key.y = reader.readI32();
        out.key.z = reader.readI32();
        uint32_t size = reader.readU32();
        out.payload.resize(size);
        if (size > 0) {
            reader.readBytes(out.payload.data(), size);
        }
        return out;
    }
};

class MemoryEntityRegionCodec final : public EntityRegionCodec {
public:
    void write(const EntityRegionSnapshot& region, ByteWriter& writer) override {
        writer.writeU32(static_cast<uint32_t>(region.payload.size()));
        if (!region.payload.empty()) {
            writer.writeBytes(region.payload.data(), region.payload.size());
        }
    }

    EntityRegionSnapshot read(ByteReader& reader, const EntityRegionKey& keyHint) override {
        EntityRegionSnapshot out;
        out.key = keyHint;
        uint32_t size = reader.readU32();
        out.payload.resize(size);
        if (size > 0) {
            reader.readBytes(out.payload.data(), size);
        }
        return out;
    }
};

class MemoryChunkContainer final : public ChunkContainer {
public:
    MemoryChunkContainer(std::shared_ptr<StorageBackend> storage, PersistenceContext context, ChunkCodec& codec)
        : m_storage(std::move(storage)), m_context(std::move(context)), m_codec(codec) {
    }

    void saveRegion(const ChunkRegionSnapshot& region) override {
        auto path = regionPath(m_context, region.key);
        m_storage->mkdirs(parentPath(path));
        auto session = m_storage->openWrite(path, AtomicWriteOptions{});
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
        auto path = regionPath(m_context, key);
        auto reader = m_storage->openRead(path);
        uint32_t count = reader->readU32();
        region.chunks.reserve(count);
        ChunkKey hint{key.zoneId, 0, 0, 0};
        for (uint32_t i = 0; i < count; ++i) {
            region.chunks.push_back(m_codec.read(*reader, hint));
        }
        return region;
    }

    bool supportsChunkIO() const override {
        return true;
    }

    void saveChunk(const ChunkSnapshot& chunk) override {
        auto path = chunkPath(m_context, chunk.key);
        m_storage->mkdirs(parentPath(path));
        auto session = m_storage->openWrite(path, AtomicWriteOptions{});
        m_codec.write(chunk, session->writer());
        session->writer().flush();
        session->commit();
    }

    ChunkSnapshot loadChunk(const ChunkKey& key) override {
        auto path = chunkPath(m_context, key);
        auto reader = m_storage->openRead(path);
        return m_codec.read(*reader, key);
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
        auto path = entityRegionPath(m_context, region.key);
        m_storage->mkdirs(parentPath(path));
        auto session = m_storage->openWrite(path, AtomicWriteOptions{});
        m_codec.write(region, session->writer());
        session->writer().flush();
        session->commit();
    }

    EntityRegionSnapshot loadRegion(const EntityRegionKey& key) override {
        auto path = entityRegionPath(m_context, key);
        auto reader = m_storage->openRead(path);
        return m_codec.read(*reader, key);
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

private:
    std::shared_ptr<StorageBackend> m_storage;
    PersistenceContext m_context;
    MemoryWorldMetadataCodec m_worldCodec;
    MemoryZoneMetadataCodec m_zoneCodec;
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
