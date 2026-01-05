#include "TestFramework.h"

#include "Rigel/Persistence/PersistenceService.h"
#include "Rigel/Persistence/Backends/CR/CRFormat.h"
#include "Rigel/Persistence/Backends/CR/CRPaths.h"
#include "Rigel/Persistence/Backends/CR/CRChunkMapping.h"
#include "Rigel/Persistence/Backends/CR/CRBin.h"
#include "Rigel/Persistence/Backends/CR/CRSettings.h"
#include "Rigel/Persistence/Backends/CR/CRLz4.h"
#include "Rigel/Voxel/Block.h"

#include <filesystem>
#include "Rigel/Persistence/Storage.h"

#include <unordered_map>

using namespace Rigel::Persistence;
using namespace Rigel::Persistence::Backends::CR;

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
        : m_writer(target) {
    }

    ByteWriter& writer() override {
        return m_writer;
    }

    void commit() override {
    }

    void abort() override {
    }

private:
    InMemoryByteWriter m_writer;
};

class InMemoryStorageBackend final : public StorageBackend {
public:
    std::unique_ptr<ByteReader> openRead(const std::string& path) override {
        auto it = m_files.find(path);
        if (it == m_files.end()) {
            throw std::runtime_error("InMemoryStorageBackend missing file: " + path);
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
        std::vector<std::string> out;
        for (const auto& [key, value] : m_files) {
            if (key.rfind(path, 0) == 0) {
                out.push_back(key);
            }
        }
        return out;
    }

    void mkdirs(const std::string&) override {
    }

    void remove(const std::string& path) override {
        m_files.erase(path);
    }

private:
    std::unordered_map<std::string, std::vector<uint8_t>> m_files;
};

ChunkData makeMinimalChunkData(const ChunkKey& key) {
    auto rigel = toRigelChunk(key);
    ChunkData data;
    data.span.chunkX = rigel.rigelChunkX;
    data.span.chunkY = rigel.rigelChunkY;
    data.span.chunkZ = rigel.rigelChunkZ;
    data.span.offsetX = (rigel.subchunkIndex & 1) * 16;
    data.span.offsetY = ((rigel.subchunkIndex >> 1) & 1) * 16;
    data.span.offsetZ = ((rigel.subchunkIndex >> 2) & 1) * 16;
    data.span.sizeX = 16;
    data.span.sizeY = 16;
    data.span.sizeZ = 16;
    data.blocks.assign(16 * 16 * 16, Rigel::Voxel::BlockState{});
    return data;
}

const CRBinValue& requireField(const CRBinObject& obj, const std::string& name) {
    auto it = obj.fields.find(name);
    if (it == obj.fields.end()) {
        throw std::runtime_error("Missing CRBin field: " + name);
    }
    return it->second;
}

int64_t asInt(const CRBinValue& value) {
    if (std::holds_alternative<int64_t>(value.value)) {
        return std::get<int64_t>(value.value);
    }
    throw std::runtime_error("CRBin field not int");
}

float asFloat(const CRBinValue& value) {
    if (std::holds_alternative<float>(value.value)) {
        return std::get<float>(value.value);
    }
    throw std::runtime_error("CRBin field not float");
}

bool asBool(const CRBinValue& value) {
    if (std::holds_alternative<bool>(value.value)) {
        return std::get<bool>(value.value);
    }
    throw std::runtime_error("CRBin field not bool");
}

std::string asString(const CRBinValue& value) {
    if (std::holds_alternative<std::string>(value.value)) {
        return std::get<std::string>(value.value);
    }
    throw std::runtime_error("CRBin field not string");
}

} // namespace

TEST_CASE(CRPaths_normalize_zone) {
    CHECK_EQ(CRPaths::normalizeZoneId("rigel:demo"), "rigel/demo");
    CHECK_EQ(CRPaths::normalizeZoneId("overworld"), "overworld");
}

TEST_CASE(CRChunkMapping_basic) {
    ChunkKey crKey{"zone", -1, 2, 3};
    auto mapped = toRigelChunk(crKey);
    CHECK_EQ(mapped.rigelChunkX, -1);
    CHECK_EQ(mapped.rigelChunkY, 1);
    CHECK_EQ(mapped.rigelChunkZ, 1);
    CHECK_EQ(mapped.subchunkIndex, 5);

    auto crBack = toCRChunk(mapped);
    CHECK_EQ(crBack.x, crKey.x);
    CHECK_EQ(crBack.y, crKey.y);
    CHECK_EQ(crBack.z, crKey.z);

    auto local = toRigelLocal(5, 6, 7, mapped.subchunkIndex);
    CHECK_EQ(local.x, 21);
    CHECK_EQ(local.y, 6);
    CHECK_EQ(local.z, 23);
}

TEST_CASE(CRBackend_region_roundtrip_minimal) {
    auto storage = std::make_shared<InMemoryStorageBackend>();
    FormatRegistry registry;
    registry.registerFormat(Backends::CR::descriptor(), Backends::CR::factory(), Backends::CR::probe());
    PersistenceService service(registry);

    PersistenceContext context;
    context.rootPath = "worlds/test";
    context.preferredFormat = "cr";
    context.storage = storage;

    ChunkRegionSnapshot region;
    region.key = RegionKey{"zone:default", 0, 0, 0};

    ChunkSnapshot chunk;
    chunk.key = ChunkKey{"zone:default", 0, 0, 0};
    chunk.data = makeMinimalChunkData(chunk.key);
    region.chunks.push_back(chunk);

    service.saveRegion(region, context);
    auto loaded = service.loadRegion(region.key, context);

    CHECK_EQ(loaded.chunks.size(), 1u);
    CHECK_EQ(loaded.chunks[0].key, chunk.key);
    CHECK_EQ(loaded.chunks[0].data, chunk.data);
}

TEST_CASE(CRBackend_world_metadata_roundtrip) {
    auto storage = std::make_shared<InMemoryStorageBackend>();
    FormatRegistry registry;
    registry.registerFormat(Backends::CR::descriptor(), Backends::CR::factory(), Backends::CR::probe());
    PersistenceService service(registry);

    PersistenceContext context;
    context.rootPath = "worlds/demo";
    context.preferredFormat = "cr";
    context.storage = storage;

    WorldSnapshot world;
    world.metadata.worldId = "demo";
    world.metadata.displayName = "Demo World";

    service.saveWorld(world, SaveScope::MetadataOnly, context);
    auto loaded = service.loadWorldMetadata(context);

    CHECK_EQ(loaded.worldId, "demo");
    CHECK_EQ(loaded.displayName, "Demo World");
}

TEST_CASE(CRBackend_region_roundtrip_lz4) {
    if (!CRLz4::available()) {
        SKIP_TEST("LZ4 not available");
    }
    auto storage = std::make_shared<InMemoryStorageBackend>();
    FormatRegistry registry;
    registry.registerFormat(Backends::CR::descriptor(), Backends::CR::factory(), Backends::CR::probe());
    PersistenceService service(registry);

    auto providers = std::make_shared<ProviderRegistry>();
    auto settings = std::make_shared<CRPersistenceSettings>();
    settings->enableLz4 = true;
    providers->add(kCRSettingsProviderId, settings);

    PersistenceContext context;
    context.rootPath = "worlds/test";
    context.preferredFormat = "cr";
    context.storage = storage;
    context.providers = providers;

    ChunkRegionSnapshot region;
    region.key = RegionKey{"zone:default", 0, 0, 0};

    ChunkSnapshot chunk;
    chunk.key = ChunkKey{"zone:default", 1, 0, 0};
    chunk.data = makeMinimalChunkData(chunk.key);
    region.chunks.push_back(chunk);

    service.saveRegion(region, context);

    auto path = CRPaths::regionPath(region.key, context);
    auto reader = storage->openRead(path);
    CHECK_EQ(reader->readI32(), static_cast<int32_t>(0xFFECCEAC));
    CHECK_EQ(reader->readI32(), 4);
    CHECK_EQ(reader->readI32(), 1);

    auto loaded = service.loadRegion(region.key, context);

    CHECK_EQ(loaded.chunks.size(), 1u);
    CHECK_EQ(loaded.chunks[0].key, chunk.key);
    CHECK_EQ(loaded.chunks[0].data, chunk.data);
}

TEST_CASE(CRBin_roundtrip_basic) {
    CRBinDocument doc;
    doc.schema.entries = {
        {"id", CRSchemaType::Int},
        {"name", CRSchemaType::String},
        {"flag", CRSchemaType::Boolean},
        {"items", CRSchemaType::IntArray},
        {"child", CRSchemaType::Object}
    };
    CRSchema childSchema;
    childSchema.entries = {{"value", CRSchemaType::Float}};
    doc.altSchemas.push_back(childSchema);

    CRBinObject root;
    root.fields["id"] = CRBinValue::fromInt(42);
    root.fields["name"] = CRBinValue::fromString("demo");
    root.fields["flag"] = CRBinValue::fromBool(true);
    CRBinValue::Array items;
    items.push_back(CRBinValue::fromInt(1));
    items.push_back(CRBinValue::fromInt(2));
    items.push_back(CRBinValue::fromInt(3));
    root.fields["items"] = CRBinValue::fromArray(std::move(items));

    CRBinObject child;
    child.schemaIndex = 0;
    child.fields["value"] = CRBinValue::fromFloat(1.25f);
    root.fields["child"] = CRBinValue::fromObject(std::move(child));

    doc.root = std::move(root);

    std::vector<uint8_t> bytes;
    InMemoryByteWriter writer(bytes);
    CRBinWriter::write(writer, doc);

    InMemoryByteReader reader(bytes);
    auto loaded = CRBinReader::read(reader);

    CHECK_EQ(asInt(requireField(loaded.root, "id")), 42);
    CHECK_EQ(asString(requireField(loaded.root, "name")), "demo");
    CHECK_EQ(asBool(requireField(loaded.root, "flag")), true);

    const auto& itemsValue = requireField(loaded.root, "items");
    CHECK(std::holds_alternative<CRBinValue::Array>(itemsValue.value));
    const auto& itemsArray = std::get<CRBinValue::Array>(itemsValue.value);
    CHECK_EQ(itemsArray.size(), 3u);
    CHECK_EQ(asInt(itemsArray[0]), 1);
    CHECK_EQ(asInt(itemsArray[1]), 2);
    CHECK_EQ(asInt(itemsArray[2]), 3);

    const auto& childValue = requireField(loaded.root, "child");
    CHECK(std::holds_alternative<CRBinObject>(childValue.value));
    const auto& childObj = std::get<CRBinObject>(childValue.value);
    CHECK_EQ(asFloat(requireField(childObj, "value")), 1.25f);
}

TEST_CASE(CRBackend_filesystem_region_roundtrip) {
    std::filesystem::path root = ".cache/cr_backend_fs_test";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);

    auto storage = std::make_shared<FilesystemBackend>();
    FormatRegistry registry;
    registry.registerFormat(Backends::CR::descriptor(), Backends::CR::factory(), Backends::CR::probe());
    PersistenceService service(registry);

    PersistenceContext context;
    context.rootPath = root.string();
    context.preferredFormat = "cr";
    context.storage = storage;

    ChunkRegionSnapshot region;
    region.key = RegionKey{"zone:default", 0, 0, 0};

    ChunkSnapshot chunk;
    chunk.key = ChunkKey{"zone:default", 2, 0, 0};
    chunk.data = makeMinimalChunkData(chunk.key);
    region.chunks.push_back(chunk);

    service.saveRegion(region, context);

    auto path = CRPaths::regionPath(region.key, context);
    CHECK(std::filesystem::exists(path));

    auto loaded = service.loadRegion(region.key, context);
    CHECK_EQ(loaded.chunks.size(), 1u);
    CHECK_EQ(loaded.chunks[0].key, chunk.key);
    CHECK_EQ(loaded.chunks[0].data, chunk.data);
}
