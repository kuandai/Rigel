#include "TestFramework.h"

#include "Rigel/Persistence/PersistenceService.h"
#include "Rigel/Persistence/Backends/Memory/MemoryFormat.h"
#include "Rigel/Persistence/Storage.h"
#include "Rigel/Voxel/Block.h"
#include "../src/entity/EntityPersistenceLimits.h"

#include <glm/vec3.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <unordered_map>

using namespace Rigel::Persistence;

namespace {

constexpr size_t kMaxMetadataDocumentBytes = 4 * 1024 * 1024;
constexpr size_t kMaxMemoryStringBytes = 1'048'576;

struct StorageMutationCounts {
    size_t mkdirs = 0;
    size_t openWrites = 0;
    size_t commits = 0;
    size_t removes = 0;

    bool operator==(const StorageMutationCounts&) const = default;
};

EntityRegionSnapshot maximumMemoryEntityRegion(
    const std::string& zoneId) {
    constexpr size_t kEntityCount = 32;
    constexpr size_t kRegionFixedBytes =
        sizeof(uint32_t) + Rigel::Entity::detail::MinEncodedChunkBytes +
        kEntityCount * Rigel::Entity::detail::MinEncodedEntityBytes;
    size_t remainingStringBytes =
        Rigel::Entity::detail::MaxEntityRegionBytes - kRegionFixedBytes;

    EntityRegionSnapshot region;
    region.key = EntityRegionKey{zoneId, 0, 0, 0};
    region.chunks.emplace_back();
    auto& entities = region.chunks.back().entities;
    entities.resize(kEntityCount);
    for (auto& entity : entities) {
        const size_t typeBytes = std::min(
            remainingStringBytes,
            static_cast<size_t>(
                Rigel::Entity::detail::MaxEntityStringBytes));
        entity.typeId.assign(typeBytes, 't');
        remainingStringBytes -= typeBytes;

        const size_t modelBytes = std::min(
            remainingStringBytes,
            static_cast<size_t>(
                Rigel::Entity::detail::MaxEntityStringBytes));
        entity.modelId.assign(modelBytes, 'm');
        remainingStringBytes -= modelBytes;
    }
    CHECK_EQ(remainingStringBytes, static_cast<size_t>(0));
    return region;
}

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
        if (offset > m_data.size() || len > m_data.size() - offset) {
            throw std::runtime_error("InMemoryByteReader readAt out of range");
        }
        return std::vector<uint8_t>(m_data.begin() + offset, m_data.begin() + offset + len);
    }

private:
    void ensureAvailable(size_t len) {
        if (m_pos > m_data.size() || len > m_data.size() - m_pos) {
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

void checkSequentialReadFailure(ByteReader& reader,
                                size_t length,
                                const std::string& diagnostic) {
    const size_t position = reader.tell();
    const std::array<uint8_t, 4> expected{91, 92, 93, 94};
    auto destination = expected;
    try {
        reader.readBytes(destination.data(), length);
    } catch (const std::runtime_error& error) {
        CHECK_EQ(std::string(error.what()), diagnostic);
        CHECK_EQ(reader.tell(), position);
        CHECK_EQ(destination, expected);
        return;
    } catch (const std::exception& error) {
        throw Rigel::Test::TestFailure(
            std::string("Unexpected sequential read exception: ") + error.what());
    }
    throw Rigel::Test::TestFailure("Expected sequential read to fail");
}

void checkSequentialReadBounds(ByteReader& reader) {
    const std::array<uint8_t, 4> initial{91, 92, 93, 94};
    std::array<uint8_t, 4> destination = initial;

    reader.readBytes(destination.data(), 0);
    CHECK_EQ(reader.tell(), static_cast<size_t>(0));
    CHECK_EQ(destination, initial);

    reader.seek(1);
    checkSequentialReadFailure(
        reader, destination.size(), "InMemoryByteReader read out of range");
    checkSequentialReadFailure(
        reader,
        std::numeric_limits<size_t>::max(),
        "InMemoryByteReader read out of range");

    reader.seek(0);
    reader.readBytes(destination.data(), destination.size());
    CHECK_EQ(destination, (std::array<uint8_t, 4>{10, 20, 30, 40}));
    CHECK_EQ(reader.tell(), destination.size());
}

class InMemoryWriteSession final : public AtomicWriteSession {
public:
    InMemoryWriteSession(
        std::unordered_map<std::string, std::vector<uint8_t>>& files,
        std::string path,
        StorageMutationCounts& mutations)
        : m_files(files),
          m_path(std::move(path)),
          m_mutations(mutations),
          m_writer(m_buffer) {
    }

    ByteWriter& writer() override {
        return m_writer;
    }

    void commit() override {
        ++m_mutations.commits;
        m_files[m_path] = m_buffer;
    }

    void abort() override {
    }

private:
    std::unordered_map<std::string, std::vector<uint8_t>>& m_files;
    std::string m_path;
    StorageMutationCounts& m_mutations;
    std::vector<uint8_t> m_buffer;
    InMemoryByteWriter m_writer;
};

class InMemoryStorageBackend final : public StorageBackend {
public:
    std::unique_ptr<ByteReader> openRead(const std::string& path) override {
        m_calls.push_back("openRead " + path);
        auto it = m_files.find(path);
        if (it == m_files.end()) {
            throw std::runtime_error("Missing in-memory file: " + path);
        }
        return std::make_unique<InMemoryByteReader>(it->second);
    }

    std::unique_ptr<AtomicWriteSession> openWrite(const std::string& path) override {
        m_calls.push_back("openWrite " + path);
        ++m_mutations.openWrites;
        return std::make_unique<InMemoryWriteSession>(
            m_files, path, m_mutations);
    }

    bool exists(const std::string& path) override {
        m_calls.push_back("exists " + path);
        return m_files.find(path) != m_files.end();
    }

    void forEachEntry(
        const std::string& path,
        const StorageEntryVisitor& visitor) override {
        m_calls.push_back("list " + path);
        for (const auto& [key, value] : m_files) {
            if (key.rfind(path, 0) == 0 && !visitor(key)) {
                return;
            }
        }
    }

    std::vector<std::string> list(const std::string& path) override {
        m_calls.push_back("list " + path);
        std::vector<std::string> results;
        for (const auto& [key, value] : m_files) {
            if (key.rfind(path, 0) == 0) {
                results.push_back(key);
            }
        }
        return results;
    }

    void mkdirs(const std::string& path) override {
        m_calls.push_back("mkdirs " + path);
        ++m_mutations.mkdirs;
    }

    void remove(const std::string& path) override {
        m_calls.push_back("remove " + path);
        ++m_mutations.removes;
        m_files.erase(path);
    }

    const std::vector<std::string>& calls() const {
        return m_calls;
    }

    void clearCalls() {
        m_calls.clear();
    }

    StorageMutationCounts mutations() const {
        return m_mutations;
    }

    void clearMutations() {
        m_mutations = {};
    }

    const std::unordered_map<std::string, std::vector<uint8_t>>& files() const {
        return m_files;
    }

private:
    std::unordered_map<std::string, std::vector<uint8_t>> m_files;
    std::vector<std::string> m_calls;
    StorageMutationCounts m_mutations;
};

void createEmptyFile(StorageBackend& storage, const std::string& path) {
    auto session = storage.openWrite(path);
    session->commit();
}

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

    void removeRegion(const EntityRegionKey&) override {
    }

    EntityRegionSnapshot loadRegion(const EntityRegionKey& key) override {
        return EntityRegionSnapshot{key, {}};
    }

    void forEachRegion(
        const std::string&,
        const EntityRegionVisitor&) override {
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

TEST_CASE(PersistenceInMemoryByteReader_bounds_sequential_reads) {
    InMemoryByteReader reader({10, 20, 30, 40});
    checkSequentialReadBounds(reader);
}

TEST_CASE(PersistenceInMemoryStorage_abandoned_write_does_not_create_destination) {
    InMemoryStorageBackend storage;
    auto session = storage.openWrite("root/uncommitted.bin");
    session->writer().writeU8(42);

    CHECK(!storage.exists("root/uncommitted.bin"));
    session.reset();
    CHECK(!storage.exists("root/uncommitted.bin"));
}

TEST_CASE(Persistence_MetadataRoundTrip) {
    auto storage = std::make_shared<InMemoryStorageBackend>();

    FormatRegistry registry;
    registry.registerFormat(Backends::Memory::descriptor(), Backends::Memory::factory(), Backends::Memory::probe());

    PersistenceService service(registry);
    PersistenceContext context;
    context.rootPath = "root";
    context.preferredFormat = "memory";
    context.storage = storage;

    WorldMetadata world{"world-alpha", "World Alpha"};
    const ZoneMetadata zone{"zone-main", "Main"};

    service.saveWorldMetadata(world, context);
    service.saveZoneMetadata(zone, context);

    auto loaded = service.loadWorldMetadata(context);
    CHECK_EQ(loaded, world);
    CHECK_EQ(service.loadZoneMetadata(ZoneKey{zone.zoneId}, context), zone);
}

TEST_CASE(Persistence_MaximumMemoryMetadataDocumentRoundTrip) {
    auto storage = std::make_shared<InMemoryStorageBackend>();

    FormatRegistry registry;
    registry.registerFormat(Backends::Memory::descriptor(), Backends::Memory::factory(), Backends::Memory::probe());

    PersistenceService service(registry);
    PersistenceContext context;
    context.rootPath = "root";
    context.preferredFormat = "memory";
    context.storage = storage;

    WorldMetadata world;
    world.worldId = std::string(kMaxMemoryStringBytes, 'w');
    world.displayName = std::string(kMaxMemoryStringBytes, 'd');

    service.saveWorldMetadata(world, context);

    CHECK_EQ(service.loadWorldMetadata(context), world);
    CHECK_EQ(
        storage->files().at("root/world.meta").size(),
        2 * kMaxMemoryStringBytes + 2 * sizeof(uint32_t));
    CHECK(storage->files().at("root/world.meta").size() <
          kMaxMetadataDocumentBytes);
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

    ZoneMetadata zone;
    zone.zoneId = "zone-main";
    zone.displayName = "Main Zone";

    service.saveZoneMetadata(zone, context);

    auto loaded = service.loadZoneMetadata(ZoneKey{"zone-main"}, context);
    CHECK_EQ(loaded.zoneId, zone.zoneId);
    CHECK_EQ(loaded.displayName, zone.displayName);
}

TEST_CASE(Persistence_ZoneMetadataValidationPrecedesReplacement) {
    auto storage = std::make_shared<InMemoryStorageBackend>();

    FormatRegistry registry;
    registry.registerFormat(
        Backends::Memory::descriptor(),
        Backends::Memory::factory(),
        Backends::Memory::probe());

    PersistenceService service(registry);
    PersistenceContext context;
    context.rootPath = "root";
    context.preferredFormat = "memory";
    context.storage = storage;

    const ZoneMetadata original{"zone-main", "Main Zone"};
    service.saveZoneMetadata(original, context);
    const auto originalFiles = storage->files();
    storage->clearMutations();

    std::string diagnostic;
    try {
        service.saveZoneMetadata(
            ZoneMetadata{
                original.zoneId,
                std::string(kMaxMemoryStringBytes + 1, 'x')},
            context);
    } catch (const std::runtime_error& error) {
        diagnostic = error.what();
    }

    CHECK_EQ(
        diagnostic,
        "MemoryFormat: string length exceeds format limit");
    CHECK_EQ(storage->mutations(), StorageMutationCounts{});
    CHECK_EQ(storage->files(), originalFiles);
    CHECK_EQ(
        service.loadZoneMetadata(ZoneKey{original.zoneId}, context),
        original);
}

TEST_CASE(Persistence_MetadataSavesPreserveExistingPayloads) {
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

    ChunkRegionSnapshot chunkRegion;
    chunkRegion.key = RegionKey{"zone-main", 0, 0, 0};
    chunkRegion.chunks.push_back(chunk);
    service.saveRegion(chunkRegion, context);

    EntityPersistedChunk entityChunk;
    entityChunk.coord = Rigel::Voxel::ChunkCoord{1, 2, 3};
    EntityPersistedEntity entity;
    entity.typeId = "rigel:test_entity";
    entity.id.time = 1;
    entity.id.random = 2;
    entity.id.counter = 3;
    entity.position = glm::vec3(1.0f, 2.0f, 3.0f);
    entity.modelId = "models/entities/test";
    entityChunk.entities.push_back(entity);

    EntityRegionSnapshot entityRegion;
    entityRegion.key = EntityRegionKey{"zone-main", 0, 0, 0};
    entityRegion.chunks.push_back(entityChunk);
    service.saveEntities(entityRegion, context);

    ZoneMetadata zone{"zone-main", "Main Zone"};
    service.saveZoneMetadata(zone, context);
    auto format = service.openFormat(context);
    const auto zonePath = format->zoneMetadataCodec().metadataPath(
        ZoneKey{zone.zoneId}, context);
    const auto zonePayload = storage->files().at(zonePath);

    CHECK_EQ(service.loadRegion(chunkRegion.key, context), chunkRegion);
    CHECK_EQ(service.loadEntities(entityRegion.key, context), entityRegion);

    service.saveWorldMetadata(
        WorldMetadata{"world-main", "Main World"}, context);

    CHECK_EQ(service.loadRegion(chunkRegion.key, context), chunkRegion);
    CHECK_EQ(service.loadEntities(entityRegion.key, context), entityRegion);
    CHECK_EQ(storage->files().at(zonePath), zonePayload);
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

TEST_CASE(MemoryFormat_EntityRegionPayloadBoundaryIsReadableAndAtomic) {
    Rigel::Test::TemporaryDirectory directory("memory_entity_boundary");
    auto storage = std::make_shared<FilesystemBackend>();
    PersistenceContext context;
    context.rootPath = directory.path().generic_string();
    context.preferredFormat = "memory";
    context.storage = storage;
    auto format = Backends::Memory::factory()(context);

    auto region = maximumMemoryEntityRegion("zone");
    format->entityContainer().saveRegion(region);

    const std::string path = context.rootPath +
        "/zones/zone/entities/entityRegion_0_0_0.mem";
    auto reader = storage->openRead(path);
    CHECK_EQ(
        reader->size(),
        Rigel::Entity::detail::MaxEntityRegionBytes);
    const auto originalBytes = reader->readAt(0, reader->size());
    reader.reset();
    CHECK(format->entityContainer().loadRegion(region.key) == region);

    region.chunks.front().entities.back().modelId.push_back('m');
    std::string diagnostic;
    try {
        format->entityContainer().saveRegion(region);
    } catch (const std::runtime_error& error) {
        diagnostic = error.what();
    }

    CHECK_EQ(
        diagnostic,
        "MemoryFormat: entity region payload exceeds format limit");
    reader = storage->openRead(path);
    CHECK(reader->readAt(0, reader->size()) == originalBytes);
}

TEST_CASE(Persistence_MemoryEntityRegionLimitPrecedesStorageMutation) {
    auto storage = std::make_shared<InMemoryStorageBackend>();

    FormatRegistry registry;
    registry.registerFormat(
        Backends::Memory::descriptor(),
        Backends::Memory::factory(),
        Backends::Memory::probe());

    PersistenceService service(registry);
    PersistenceContext context;
    context.rootPath = "root";
    context.preferredFormat = "memory";
    context.storage = storage;

    EntityRegionSnapshot original;
    original.key = EntityRegionKey{"zone-main", 0, 0, 0};
    original.chunks.emplace_back();
    service.saveEntities(original, context);

    auto oversized = maximumMemoryEntityRegion(original.key.zoneId);
    oversized.chunks.front().entities.back().modelId.push_back('m');
    const auto originalFiles = storage->files();
    storage->clearCalls();
    storage->clearMutations();

    std::string diagnostic;
    try {
        service.saveEntities(oversized, context);
    } catch (const std::runtime_error& error) {
        diagnostic = error.what();
    }

    CHECK_EQ(
        diagnostic,
        "MemoryFormat: entity region payload exceeds format limit");
    CHECK(storage->calls().empty());
    CHECK_EQ(storage->mutations(), StorageMutationCounts{});
    CHECK_EQ(storage->files(), originalFiles);
}

TEST_CASE(MemoryFormat_region_discovery_requires_canonical_filenames) {
    auto storage = std::make_shared<InMemoryStorageBackend>();
    PersistenceContext context;
    context.rootPath = "root";
    context.storage = storage;
    const std::string zoneId = "zone-main";
    const auto zoneRoot = context.rootPath + "/zones/" + zoneId;

    const auto regionsRoot = zoneRoot + "/regions";
    createEmptyFile(*storage, regionsRoot);
    createEmptyFile(*storage, regionsRoot + "/region_-12_3_0.mem");
    createEmptyFile(*storage, regionsRoot + "/region_4_5_6.mem.tmp.123");
    createEmptyFile(*storage, regionsRoot + "/region_7_8_9.mem.bak");
    createEmptyFile(*storage, regionsRoot + "/region_01_2_3.mem");
    createEmptyFile(*storage, regionsRoot + "/region_10_11_12");

    const auto entitiesRoot = zoneRoot + "/entities";
    createEmptyFile(*storage, entitiesRoot);
    createEmptyFile(*storage, entitiesRoot + "/entityRegion_6_-7_8.mem");
    createEmptyFile(*storage, entitiesRoot + "/entityRegion_1_2_3.mem.tmp.456");
    createEmptyFile(*storage, entitiesRoot + "/entityRegion_4_5_6.mem.bak");
    createEmptyFile(*storage, entitiesRoot + "/entityRegion_-0_7_8.mem");
    createEmptyFile(*storage, entitiesRoot + "/entityRegion_9_10_11");

    auto format = Backends::Memory::factory()(context);
    const auto regions = format->chunkContainer().listRegions(zoneId);
    const auto entityRegions = format->entityContainer().listRegions(zoneId);

    CHECK_EQ(regions.size(), 1u);
    CHECK_EQ(regions.front(), (RegionKey{zoneId, -12, 3, 0}));
    CHECK_EQ(entityRegions.size(), 1u);
    CHECK_EQ(entityRegions.front(), (EntityRegionKey{zoneId, 6, -7, 8}));
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
