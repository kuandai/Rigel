#include "TestFramework.h"

#include "Rigel/Persistence/PersistenceService.h"
#include "Rigel/Persistence/Backends/Memory/MemoryFormat.h"
#include "Rigel/Persistence/Storage.h"
#include "Rigel/Voxel/Block.h"

#include <glm/vec3.hpp>

#include <algorithm>
#include <unordered_map>

using namespace Rigel::Persistence;

namespace {

class InMemoryByteReader final : public ByteReader {
public:
    explicit InMemoryByteReader(std::vector<uint8_t> data)
        : m_data(std::move(data)) {
    }

    uint8_t readU8() override {
        ensureAvailable(1);
        return m_data[m_pos++];
    }

    uint16_t readU16() override {
        uint16_t value = 0;
        value |= static_cast<uint16_t>(readU8()) << 8;
        value |= static_cast<uint16_t>(readU8());
        return value;
    }

    uint32_t readU32() override {
        uint32_t value = 0;
        value |= static_cast<uint32_t>(readU8()) << 24;
        value |= static_cast<uint32_t>(readU8()) << 16;
        value |= static_cast<uint32_t>(readU8()) << 8;
        value |= static_cast<uint32_t>(readU8());
        return value;
    }

    int32_t readI32() override {
        return static_cast<int32_t>(readU32());
    }

    void readBytes(uint8_t* dst, size_t len) override {
        ensureAvailable(len);
        if (len == 0) {
            return;
        }
        std::copy_n(m_data.data() + m_pos, len, dst);
        m_pos += len;
    }

    size_t size() const override {
        return m_data.size();
    }

    size_t tell() const override {
        return m_pos;
    }

    void seek(size_t offset) override {
        if (offset > m_data.size()) {
            throw std::runtime_error("InMemoryByteReader seek out of range");
        }
        m_pos = offset;
    }

    std::vector<uint8_t> readAt(size_t offset, size_t len) override {
        if (offset + len > m_data.size()) {
            throw std::runtime_error("InMemoryByteReader readAt out of range");
        }
        return std::vector<uint8_t>(m_data.begin() + offset, m_data.begin() + offset + len);
    }

private:
    void ensureAvailable(size_t len) {
        if (m_pos + len > m_data.size()) {
            throw std::runtime_error("InMemoryByteReader read out of range");
        }
    }

    std::vector<uint8_t> m_data;
    size_t m_pos = 0;
};

class InMemoryByteWriter final : public ByteWriter {
public:
    explicit InMemoryByteWriter(std::vector<uint8_t>& target)
        : m_target(target) {
    }

    void writeU8(uint8_t value) override {
        writeBytes(&value, 1);
    }

    void writeU16(uint16_t value) override {
        uint8_t bytes[2] = {
            static_cast<uint8_t>((value >> 8) & 0xFF),
            static_cast<uint8_t>(value & 0xFF)
        };
        writeBytes(bytes, 2);
    }

    void writeU32(uint32_t value) override {
        uint8_t bytes[4] = {
            static_cast<uint8_t>((value >> 24) & 0xFF),
            static_cast<uint8_t>((value >> 16) & 0xFF),
            static_cast<uint8_t>((value >> 8) & 0xFF),
            static_cast<uint8_t>(value & 0xFF)
        };
        writeBytes(bytes, 4);
    }

    void writeI32(int32_t value) override {
        writeU32(static_cast<uint32_t>(value));
    }

    void writeBytes(const uint8_t* src, size_t len) override {
        if (len == 0) {
            return;
        }
        if (m_pos + len > m_target.size()) {
            m_target.resize(m_pos + len, 0);
        }
        std::copy_n(src, len, m_target.data() + m_pos);
        m_pos += len;
    }

    size_t size() const override {
        return m_target.size();
    }

    size_t tell() const override {
        return m_pos;
    }

    void seek(size_t offset) override {
        if (offset > m_target.size()) {
            m_target.resize(offset, 0);
        }
        m_pos = offset;
    }

    void writeAt(size_t offset, const uint8_t* src, size_t len) override {
        if (offset + len > m_target.size()) {
            m_target.resize(offset + len, 0);
        }
        std::copy_n(src, len, m_target.data() + offset);
    }

    void flush() override {
    }

private:
    std::vector<uint8_t>& m_target;
    size_t m_pos = 0;
};

class InMemoryWriteSession final : public AtomicWriteSession {
public:
    explicit InMemoryWriteSession(std::vector<uint8_t>& target)
        : m_target(target), m_writer(m_buffer) {
    }

    ByteWriter& writer() override {
        return m_writer;
    }

    void commit() override {
        m_target = m_buffer;
    }

    void abort() override {
    }

private:
    std::vector<uint8_t>& m_target;
    std::vector<uint8_t> m_buffer;
    InMemoryByteWriter m_writer;
};

class InMemoryStorageBackend final : public StorageBackend {
public:
    std::unique_ptr<ByteReader> openRead(const std::string& path) override {
        auto it = m_files.find(path);
        if (it == m_files.end()) {
            throw std::runtime_error("Missing in-memory file: " + path);
        }
        return std::make_unique<InMemoryByteReader>(it->second);
    }

    std::unique_ptr<AtomicWriteSession> openWrite(const std::string& path, AtomicWriteOptions) override {
        return std::make_unique<InMemoryWriteSession>(m_files[path]);
    }

    bool exists(const std::string& path) override {
        return m_files.find(path) != m_files.end();
    }

    std::vector<std::string> list(const std::string& path) override {
        std::vector<std::string> results;
        for (const auto& [key, value] : m_files) {
            if (key.rfind(path, 0) == 0) {
                results.push_back(key);
            }
        }
        return results;
    }

    void mkdirs(const std::string&) override {
    }

    void remove(const std::string& path) override {
        m_files.erase(path);
    }

private:
    std::unordered_map<std::string, std::vector<uint8_t>> m_files;
};

class NullWorldMetadataCodec final : public WorldMetadataCodec {
public:
    std::string metadataPath(const PersistenceContext& context) const override {
        return context.rootPath + "/null.world";
    }

    void write(const WorldMetadata&, ByteWriter&) override {
    }

    WorldMetadata read(ByteReader&) override {
        return {};
    }
};

class NullZoneMetadataCodec final : public ZoneMetadataCodec {
public:
    std::string metadataPath(const ZoneKey& key, const PersistenceContext& context) const override {
        return context.rootPath + "/zones/" + key.zoneId + "/null.zone";
    }

    void write(const ZoneMetadata&, ByteWriter&) override {
    }

    ZoneMetadata read(ByteReader&) override {
        return {};
    }
};

class NullChunkContainer final : public ChunkContainer {
public:
    void saveRegion(const ChunkRegionSnapshot&) override {
    }

    ChunkRegionSnapshot loadRegion(const RegionKey& key) override {
        return ChunkRegionSnapshot{key, {}};
    }

    std::vector<RegionKey> listRegions(const std::string&) override {
        return {};
    }
};

class NullEntityContainer final : public EntityContainer {
public:
    void saveRegion(const EntityRegionSnapshot&) override {
    }

    EntityRegionSnapshot loadRegion(const EntityRegionKey& key) override {
        return EntityRegionSnapshot{key, {}};
    }

    std::vector<EntityRegionKey> listRegions(const std::string&) override {
        return {};
    }
};

class NullRegionLayout final : public RegionLayout {
public:
    RegionKey regionForChunk(const std::string& zoneId, Rigel::Voxel::ChunkCoord coord) const override {
        (void)coord;
        return RegionKey{zoneId, 0, 0, 0};
    }

    std::vector<ChunkKey> storageKeysForChunk(const std::string& zoneId,
                                              Rigel::Voxel::ChunkCoord coord) const override {
        return {ChunkKey{zoneId, coord.x, coord.y, coord.z}};
    }

    ChunkSpan spanForStorageKey(const ChunkKey& key) const override {
        ChunkSpan span;
        span.chunkX = key.x;
        span.chunkY = key.y;
        span.chunkZ = key.z;
        span.sizeX = 1;
        span.sizeY = 1;
        span.sizeZ = 1;
        return span;
    }

    std::vector<Rigel::Voxel::ChunkCoord> chunksForRegion(const RegionKey& key) const override {
        return {Rigel::Voxel::ChunkCoord{key.x, key.y, key.z}};
    }
};

class NoEntityFormat final : public PersistenceFormat {
public:
    explicit NoEntityFormat(FormatDescriptor descriptor)
        : m_descriptor(std::move(descriptor)) {
    }

    const FormatDescriptor& descriptor() const override {
        return m_descriptor;
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
    FormatDescriptor m_descriptor;
    NullWorldMetadataCodec m_worldCodec;
    NullZoneMetadataCodec m_zoneCodec;
    NullRegionLayout m_layout;
    NullChunkContainer m_chunkContainer;
    NullEntityContainer m_entityContainer;
};

} // namespace

TEST_CASE(Persistence_MetadataRoundTrip) {
    auto storage = std::make_shared<InMemoryStorageBackend>();

    FormatRegistry registry;
    registry.registerFormat(Backends::Memory::descriptor(), Backends::Memory::factory(), Backends::Memory::probe());

    PersistenceService service(registry);
    PersistenceContext context;
    context.rootPath = "root";
    context.preferredFormat = "memory";
    context.storage = storage;

    WorldSnapshot world;
    world.metadata.worldId = "world-alpha";
    world.metadata.displayName = "World Alpha";
    world.zones.push_back(ZoneMetadata{"zone-main", "Main"});

    service.saveWorld(world, SaveScope::MetadataOnly, context);

    auto loaded = service.loadWorldMetadata(context);
    CHECK_EQ(loaded.worldId, world.metadata.worldId);
    CHECK_EQ(loaded.displayName, world.metadata.displayName);
}

TEST_CASE(Persistence_ZoneMetadataRoundTrip) {
    auto storage = std::make_shared<InMemoryStorageBackend>();

    FormatRegistry registry;
    registry.registerFormat(Backends::Memory::descriptor(), Backends::Memory::factory(), Backends::Memory::probe());

    PersistenceService service(registry);
    PersistenceContext context;
    context.rootPath = "root";
    context.preferredFormat = "memory";
    context.storage = storage;

    ZoneSnapshot zone;
    zone.metadata.zoneId = "zone-main";
    zone.metadata.displayName = "Main Zone";

    service.saveZone(zone, SaveScope::MetadataOnly, context);

    auto loaded = service.loadZoneMetadata(ZoneKey{"zone-main"}, context);
    CHECK_EQ(loaded.zoneId, zone.metadata.zoneId);
    CHECK_EQ(loaded.displayName, zone.metadata.displayName);
}

TEST_CASE(Persistence_RegionRoundTrip) {
    auto storage = std::make_shared<InMemoryStorageBackend>();

    FormatRegistry registry;
    registry.registerFormat(Backends::Memory::descriptor(), Backends::Memory::factory(), Backends::Memory::probe());

    PersistenceService service(registry);
    PersistenceContext context;
    context.rootPath = "root";
    context.preferredFormat = "memory";
    context.storage = storage;

    ChunkSnapshot chunk;
    chunk.key = ChunkKey{"zone-main", 1, 2, 3};
    chunk.data.span.chunkX = 1;
    chunk.data.span.chunkY = 2;
    chunk.data.span.chunkZ = 3;
    chunk.data.span.sizeX = 1;
    chunk.data.span.sizeY = 1;
    chunk.data.span.sizeZ = 1;
    chunk.data.blocks.push_back(Rigel::Voxel::BlockState{Rigel::Voxel::BlockID{1}, 2, 3});

    ChunkRegionSnapshot region;
    region.key = RegionKey{"zone-main", 0, 0, 0};
    region.chunks.push_back(chunk);

    service.saveRegion(region, context);

    auto loaded = service.loadRegion(region.key, context);
    CHECK_EQ(loaded.chunks.size(), 1u);
    CHECK_EQ(loaded.chunks[0], chunk);
}

TEST_CASE(Persistence_EntityRegionRoundTrip) {
    auto storage = std::make_shared<InMemoryStorageBackend>();

    FormatRegistry registry;
    registry.registerFormat(Backends::Memory::descriptor(), Backends::Memory::factory(), Backends::Memory::probe());

    PersistenceService service(registry);
    PersistenceContext context;
    context.rootPath = "root";
    context.preferredFormat = "memory";
    context.storage = storage;

    EntityRegionSnapshot entityRegion;
    entityRegion.key = EntityRegionKey{"zone-main", 0, 0, 0};
    EntityPersistedChunk chunk;
    chunk.coord = Rigel::Voxel::ChunkCoord{0, 0, 0};
    EntityPersistedEntity entity;
    entity.typeId = "rigel:test_entity";
    entity.id.time = 1;
    entity.id.random = 2;
    entity.id.counter = 3;
    entity.position = glm::vec3(1.0f, 2.0f, 3.0f);
    entity.velocity = glm::vec3(0.0f);
    entity.viewDirection = glm::vec3(0.0f, 0.0f, 1.0f);
    entity.modelId = "models/entities/test";
    chunk.entities.push_back(entity);
    entityRegion.chunks.push_back(chunk);

    service.saveEntities(entityRegion, context);

    auto loaded = service.loadEntities(entityRegion.key, context);
    CHECK_EQ(loaded, entityRegion);
}

TEST_CASE(Persistence_PartialChunkSupport) {
    auto storage = std::make_shared<InMemoryStorageBackend>();

    FormatRegistry registry;
    registry.registerFormat(Backends::Memory::descriptor(), Backends::Memory::factory(), Backends::Memory::probe());

    PersistenceContext context;
    context.rootPath = "root";
    context.preferredFormat = "memory";
    context.storage = storage;

    auto format = registry.resolveFormat(context);
    auto& container = format->chunkContainer();
    CHECK(container.supportsChunkIO());

    ChunkSnapshot chunk;
    chunk.key = ChunkKey{"zone-main", 5, 6, 7};
    chunk.data.span.chunkX = 5;
    chunk.data.span.chunkY = 6;
    chunk.data.span.chunkZ = 7;
    chunk.data.span.sizeX = 1;
    chunk.data.span.sizeY = 1;
    chunk.data.span.sizeZ = 1;
    chunk.data.blocks.push_back(Rigel::Voxel::BlockState{Rigel::Voxel::BlockID{2}, 0, 0});

    container.saveChunk(chunk);
    auto loaded = container.loadChunk(chunk.key);
    CHECK_EQ(loaded, chunk);
}

TEST_CASE(Persistence_UnsupportedEntityPolicy) {
    auto storage = std::make_shared<InMemoryStorageBackend>();

    FormatRegistry registry;
    FormatDescriptor desc;
    desc.id = "no-entities";
    desc.version = 1;
    desc.capabilities.supportsEntityRegions = false;

    registry.registerFormat(desc,
        [desc](const PersistenceContext&) {
            return std::make_unique<NoEntityFormat>(desc);
        },
        [](StorageBackend&, const PersistenceContext&) {
            return std::optional<ProbeResult>();
        });

    PersistenceService service(registry);
    PersistenceContext context;
    context.rootPath = "root";
    context.preferredFormat = "no-entities";
    context.storage = storage;

    EntityRegionSnapshot entityRegion;
    entityRegion.key = EntityRegionKey{"zone-main", 1, 1, 1};
    EntityPersistedChunk chunk;
    chunk.coord = Rigel::Voxel::ChunkCoord{0, 0, 0};
    EntityPersistedEntity entity;
    entity.typeId = "rigel:test_entity";
    entity.id.time = 1;
    entity.id.random = 2;
    entity.id.counter = 3;
    entity.position = glm::vec3(0.0f);
    entity.velocity = glm::vec3(0.0f);
    entity.viewDirection = glm::vec3(0.0f, 0.0f, 1.0f);
    entity.modelId = "models/entities/test";
    chunk.entities.push_back(entity);
    entityRegion.chunks.push_back(chunk);

    context.policies.unsupportedFeaturePolicy = UnsupportedFeaturePolicy::Fail;
    CHECK_THROWS(service.saveEntities(entityRegion, context));

    context.policies.unsupportedFeaturePolicy = UnsupportedFeaturePolicy::NoOp;
    CHECK_NO_THROW(service.saveEntities(entityRegion, context));
}
