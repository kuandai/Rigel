#include "TestFramework.h"

#include "Rigel/Persistence/PersistenceService.h"
#include "Rigel/Persistence/AsyncChunkLoader.h"
#include "Rigel/Persistence/Backends/CR/CRFormat.h"
#include "Rigel/Persistence/Backends/CR/CRPaths.h"
#include "Rigel/Persistence/Backends/CR/CRChunkMapping.h"
#include "Rigel/Persistence/Backends/CR/CRSettings.h"
#include "Rigel/Persistence/Backends/CR/CRLz4.h"
#include "Rigel/Persistence/Providers.h"
#include "Rigel/Persistence/WorldPersistence.h"
#include "Rigel/Asset/AssetManager.h"
#include "Rigel/Entity/Entity.h"
#include "Rigel/Entity/EntityPersistence.h"
#include "Rigel/Voxel/Block.h"
#include "Rigel/Voxel/BlockType.h"
#include "Rigel/Voxel/World.h"
#include "Rigel/Voxel/WorldGenerator.h"
#include "Rigel/Voxel/WorldResources.h"

#include "Rigel/Persistence/Storage.h"
#include "../src/persistence/backends/cr/MemoryByteReader.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <limits>
#include <unordered_map>

using namespace Rigel::Persistence;
using namespace Rigel::Persistence::Backends::CR;

namespace {

constexpr size_t kMaxMetadataDocumentBytes = 4 * 1024 * 1024;
constexpr size_t kMaxMetadataStringBytes = 1024 * 1024;

void checkMemoryRandomReadFailure(MemoryByteReader& reader,
                                  size_t offset,
                                  size_t length) {
    const size_t position = reader.tell();
    try {
        static_cast<void>(reader.readAt(offset, length));
    } catch (const std::runtime_error& error) {
        CHECK_EQ(
            std::string(error.what()),
            std::string("CRMemoryReader readAt out of range"));
        CHECK_EQ(reader.tell(), position);
        return;
    } catch (const std::exception& error) {
        throw Rigel::Test::TestFailure(
            std::string("Unexpected random read exception: ") + error.what());
    }
    throw Rigel::Test::TestFailure("Expected random read to fail");
}

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

void checkSequentialReadBounds(ByteReader& reader,
                               const std::string& diagnostic) {
    const std::array<uint8_t, 4> initial{91, 92, 93, 94};
    std::array<uint8_t, 4> destination = initial;

    reader.readBytes(destination.data(), 0);
    CHECK_EQ(reader.tell(), static_cast<size_t>(0));
    CHECK_EQ(destination, initial);

    reader.seek(1);
    checkSequentialReadFailure(reader, destination.size(), diagnostic);
    checkSequentialReadFailure(
        reader, std::numeric_limits<size_t>::max(), diagnostic);

    reader.seek(0);
    reader.readBytes(destination.data(), destination.size());
    CHECK_EQ(destination, (std::array<uint8_t, 4>{10, 20, 30, 40}));
    CHECK_EQ(reader.tell(), destination.size());
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

class InMemoryWriteSession final : public AtomicWriteSession {
public:
    InMemoryWriteSession(
        std::unordered_map<std::string, std::vector<uint8_t>>& files,
        std::string path)
        : m_files(files), m_path(std::move(path)), m_writer(m_buffer) {
    }

    ByteWriter& writer() override {
        return m_writer;
    }

    void commit() override {
        m_files[m_path] = m_buffer;
    }

    void abort() override {
    }

private:
    std::unordered_map<std::string, std::vector<uint8_t>>& m_files;
    std::string m_path;
    std::vector<uint8_t> m_buffer;
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

    std::unique_ptr<AtomicWriteSession> openWrite(const std::string& path) override {
        ++m_writeSessionCount;
        return std::make_unique<InMemoryWriteSession>(m_files, path);
    }

    bool exists(const std::string& path) override {
        return m_files.find(path) != m_files.end();
    }

    void forEachEntry(
        const std::string& path,
        const StorageEntryVisitor& visitor) override {
        for (const auto& [key, value] : m_files) {
            if (key.rfind(path, 0) == 0 && !visitor(key)) {
                return;
            }
        }
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
        ++m_mkdirCount;
    }

    void remove(const std::string& path) override {
        m_files.erase(path);
    }

    size_t writeSessionCount() const {
        return m_writeSessionCount;
    }

    size_t mkdirCount() const {
        return m_mkdirCount;
    }

private:
    std::unordered_map<std::string, std::vector<uint8_t>> m_files;
    size_t m_writeSessionCount = 0;
    size_t m_mkdirCount = 0;
};

void createEmptyFile(StorageBackend& storage, const std::string& path) {
    auto session = storage.openWrite(path);
    session->commit();
}

void writeText(StorageBackend& storage,
               const std::string& path,
               const std::string& text) {
    auto session = storage.openWrite(path);
    session->writer().writeBytes(
        reinterpret_cast<const uint8_t*>(text.data()), text.size());
    session->writer().flush();
    session->commit();
}

void saveCRWorld(const std::shared_ptr<StorageBackend>& storage,
                 const std::string& rootPath,
                 bool dirtyChunk = false) {
    Rigel::Voxel::WorldResources resources;
    Rigel::Voxel::World world(resources);
    world.setId(17);
    if (dirtyChunk) {
        const std::string identifier = "base:save_marker";
        Rigel::Voxel::BlockType block;
        block.identifier = identifier;
        block.isOpaque = true;
        block.isSolid = true;
        auto blockId = resources.registry().registerBlock(
            identifier, std::move(block));
        world.setBlock(0, 0, 0, Rigel::Voxel::BlockState{blockId});
    }

    FormatRegistry registry;
    registry.registerFormat(
        Backends::CR::descriptor(), Backends::CR::factory(), Backends::CR::probe());
    PersistenceService service(registry);

    PersistenceContext context;
    context.rootPath = rootPath;
    context.preferredFormat = "cr";
    context.storage = storage;
    context.providers = world.persistenceProvidersHandle();
    saveWorldToDisk(world, service, context);
}

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

Rigel::Voxel::BlockID registerTestBlock(Rigel::Voxel::BlockRegistry& registry,
                                        const std::string& identifier) {
    Rigel::Voxel::BlockType block;
    block.identifier = identifier;
    block.isOpaque = true;
    block.isSolid = true;
    return registry.registerBlock(identifier, std::move(block));
}

void writeFixtureString(ByteWriter& writer, const std::string& value) {
    writer.writeI32(static_cast<int32_t>(value.size()));
    writer.writeBytes(reinterpret_cast<const uint8_t*>(value.data()), value.size());
}

std::vector<uint8_t> makeFixtureRecord(const ChunkKey& key,
                                       const std::string& blockIdentifier,
                                       bool includeOptionalPayloads) {
    std::vector<uint8_t> bytes;
    InMemoryByteWriter writer(bytes);
    writer.writeI32(key.x);
    writer.writeI32(key.y);
    writer.writeI32(key.z);

    if (!includeOptionalPayloads) {
        writer.writeU8(1);
        writeFixtureString(writer, blockIdentifier);
        writer.writeU8(1);
        writer.writeU8(1);
        writer.writeU8(0);
        return bytes;
    }

    writer.writeU8(2);
    writer.writeI32(2);
    writeFixtureString(writer, blockIdentifier);
    writeFixtureString(writer, "base:air");
    const std::array<uint8_t, 32> paletteIndices{};
    for (int layer = 0; layer < 16; ++layer) {
        writer.writeU8(7);
        writer.writeBytes(paletteIndices.data(), paletteIndices.size());
    }
    writer.writeU8(3);
    writer.writeU8(0x0D);
    writer.writeU8(2);
    for (uint8_t layer = 0; layer < 16; ++layer) {
        writer.writeU8(1);
        writer.writeU8(static_cast<uint8_t>(0x20 + layer));
        writer.writeU8(static_cast<uint8_t>(0x40 + layer));
        writer.writeU8(static_cast<uint8_t>(0x60 + layer));
    }
    const std::array<uint8_t, 5> blockEntity{0xDE, 0xAD, 0xBE, 0xEF, 0x42};
    writer.writeU8(1);
    writer.writeI32(static_cast<int32_t>(blockEntity.size()));
    writer.writeBytes(blockEntity.data(), blockEntity.size());
    return bytes;
}

void writeFixtureRegion(StorageBackend& storage,
                        const std::string& path,
                        const std::vector<std::vector<uint8_t>>& records) {
    auto separator = path.find_last_of('/');
    if (separator != std::string::npos) {
        createEmptyFile(storage, path.substr(0, separator));
    }
    std::array<std::vector<std::vector<uint8_t>>, 16 * 16> columns;
    for (const auto& record : records) {
        InMemoryByteReader reader(record);
        int32_t x = reader.readI32();
        reader.readI32();
        int32_t z = reader.readI32();
        columns[static_cast<size_t>(x + z * 16)].push_back(record);
    }

    std::array<int32_t, 16 * 16> offsets;
    offsets.fill(-1);
    std::vector<uint8_t> columnBytes;
    InMemoryByteWriter columnWriter(columnBytes);
    int32_t columnCount = 0;
    for (size_t index = 0; index < columns.size(); ++index) {
        const auto& column = columns[index];
        if (column.empty()) {
            continue;
        }
        offsets[index] = static_cast<int32_t>(columnBytes.size());
        ++columnCount;
        size_t columnStart = columnWriter.tell();
        columnWriter.writeI32(0);
        columnWriter.writeI32(4);
        columnWriter.writeU8(static_cast<uint8_t>(column.size()));
        for (const auto& record : column) {
            columnWriter.writeBytes(record.data(), record.size());
        }
        std::array<uint8_t, 4> columnSize{
            static_cast<uint8_t>(((columnBytes.size() - columnStart) >> 24) & 0xFF),
            static_cast<uint8_t>(((columnBytes.size() - columnStart) >> 16) & 0xFF),
            static_cast<uint8_t>(((columnBytes.size() - columnStart) >> 8) & 0xFF),
            static_cast<uint8_t>((columnBytes.size() - columnStart) & 0xFF)
        };
        columnWriter.writeAt(columnStart, columnSize.data(), columnSize.size());
    }

    std::vector<uint8_t> regionBytes;
    InMemoryByteWriter writer(regionBytes);
    writer.writeI32(static_cast<int32_t>(0xFFECCEAC));
    writer.writeI32(4);
    writer.writeI32(0);
    writer.writeI32(columnCount);
    writer.writeU8(2);
    for (int32_t offset : offsets) {
        writer.writeU16(static_cast<uint16_t>(offset));
    }
    writer.writeBytes(columnBytes.data(), columnBytes.size());

    auto session = storage.openWrite(path);
    session->writer().writeBytes(regionBytes.data(), regionBytes.size());
    session->writer().flush();
    session->commit();
}

std::vector<uint8_t> readAll(StorageBackend& storage, const std::string& path) {
    auto reader = storage.openRead(path);
    std::vector<uint8_t> bytes(reader->size());
    reader->readBytes(bytes.data(), bytes.size());
    return bytes;
}

std::vector<uint8_t> readOnlyRecordInColumn(StorageBackend& storage,
                                            const std::string& path,
                                            size_t columnIndex) {
    auto bytes = readAll(storage, path);
    InMemoryByteReader reader(bytes);
    reader.seek(16);
    CHECK_EQ(reader.readU8(), 2);
    size_t tableStart = reader.tell();
    reader.seek(tableStart + columnIndex * 2);
    int16_t relativeOffset = static_cast<int16_t>(reader.readU16());
    CHECK(relativeOffset >= 0);
    size_t columnStart = tableStart + 16 * 16 * 2 + static_cast<size_t>(relativeOffset);
    reader.seek(columnStart);
    int32_t columnSize = reader.readI32();
    reader.readI32();
    CHECK_EQ(reader.readU8(), 1);
    return reader.readAt(columnStart + 9, static_cast<size_t>(columnSize) - 9);
}

template <typename Fn>
void checkCRRegionError(Fn&& fn, const std::string& expected) {
    try {
        fn();
    } catch (const std::runtime_error& error) {
        CHECK_EQ(std::string(error.what()), expected);
        return;
    }
    throw Rigel::Test::TestFailure("Expected CR region format error");
}

template <typename Fn>
void checkCRMetadataError(Fn&& fn, const std::string& expected) {
    try {
        fn();
    } catch (const std::runtime_error& error) {
        CHECK_EQ(std::string(error.what()), expected);
        return;
    }
    throw Rigel::Test::TestFailure("Expected CR metadata format error");
}

std::vector<uint8_t> makeEmptyFixtureRecord(int32_t x, int32_t y, int32_t z) {
    std::vector<uint8_t> bytes;
    InMemoryByteWriter writer(bytes);
    writer.writeI32(x);
    writer.writeI32(y);
    writer.writeI32(z);
    writer.writeU8(0);
    writer.writeU8(1);
    writer.writeU8(1);
    writer.writeU8(0);
    return bytes;
}

std::vector<uint8_t> makeLayeredFixtureRecord(int32_t paletteSize) {
    std::vector<uint8_t> bytes;
    InMemoryByteWriter writer(bytes);
    writer.writeI32(0);
    writer.writeI32(0);
    writer.writeI32(0);
    writer.writeU8(2);
    writer.writeI32(paletteSize);
    return bytes;
}

std::vector<uint8_t> makeSingleBlockFixtureRecord(int32_t stringSize,
                                                   const std::vector<uint8_t>& stringBytes = {}) {
    std::vector<uint8_t> bytes;
    InMemoryByteWriter writer(bytes);
    writer.writeI32(0);
    writer.writeI32(0);
    writer.writeI32(0);
    writer.writeU8(1);
    writer.writeI32(stringSize);
    writer.writeBytes(stringBytes.data(), stringBytes.size());
    return bytes;
}

std::vector<uint8_t> makeBlockEntityFixtureRecord(int32_t entitySize,
                                                   const std::vector<uint8_t>& entityBytes = {}) {
    auto bytes = makeEmptyFixtureRecord(0, 0, 0);
    bytes.back() = 1;
    InMemoryByteWriter writer(bytes);
    writer.seek(bytes.size());
    writer.writeI32(entitySize);
    writer.writeBytes(entityBytes.data(), entityBytes.size());
    return bytes;
}

std::vector<uint8_t> makeInvalidPaletteReferenceRecord(int32_t paletteIndex) {
    std::vector<uint8_t> bytes;
    InMemoryByteWriter writer(bytes);
    writer.writeI32(0);
    writer.writeI32(0);
    writer.writeI32(0);
    writer.writeU8(2);
    writer.writeI32(1);
    writeFixtureString(writer, "base:air");
    writer.writeU8(2);
    writer.writeI32(paletteIndex);
    return bytes;
}

std::vector<uint8_t> makeFixtureColumn(
    int32_t version,
    uint8_t chunkCount,
    const std::vector<std::vector<uint8_t>>& records = {},
    const std::vector<uint8_t>& trailing = {}) {
    std::vector<uint8_t> bytes;
    InMemoryByteWriter writer(bytes);
    writer.writeI32(0);
    writer.writeI32(version);
    writer.writeU8(chunkCount);
    for (const auto& record : records) {
        writer.writeBytes(record.data(), record.size());
    }
    writer.writeBytes(trailing.data(), trailing.size());

    std::array<uint8_t, 4> sizeBytes{
        static_cast<uint8_t>((bytes.size() >> 24) & 0xFF),
        static_cast<uint8_t>((bytes.size() >> 16) & 0xFF),
        static_cast<uint8_t>((bytes.size() >> 8) & 0xFF),
        static_cast<uint8_t>(bytes.size() & 0xFF)
    };
    writer.writeAt(0, sizeBytes.data(), sizeBytes.size());
    return bytes;
}

void overwriteFixtureI32(std::vector<uint8_t>& bytes, size_t offset, int32_t value) {
    CHECK(offset <= bytes.size());
    CHECK(sizeof(value) <= bytes.size() - offset);
    bytes[offset] = static_cast<uint8_t>((static_cast<uint32_t>(value) >> 24) & 0xFF);
    bytes[offset + 1] = static_cast<uint8_t>((static_cast<uint32_t>(value) >> 16) & 0xFF);
    bytes[offset + 2] = static_cast<uint8_t>((static_cast<uint32_t>(value) >> 8) & 0xFF);
    bytes[offset + 3] = static_cast<uint8_t>(static_cast<uint32_t>(value) & 0xFF);
}

std::vector<uint8_t> makeFixturePayload(
    uint8_t offsetType,
    const std::array<int32_t, 16 * 16>& offsets,
    const std::vector<uint8_t>& columns = {}) {
    std::vector<uint8_t> bytes;
    InMemoryByteWriter writer(bytes);
    writer.writeU8(offsetType);
    for (int32_t offset : offsets) {
        switch (offsetType) {
        case 1:
            writer.writeU8(static_cast<uint8_t>(offset));
            break;
        case 2:
            writer.writeU16(static_cast<uint16_t>(offset));
            break;
        case 3:
            writer.writeI32(offset);
            break;
        default:
            break;
        }
    }
    writer.writeBytes(columns.data(), columns.size());
    return bytes;
}

TEST_CASE(InMemoryStorageBackend_uncommitted_writes_are_not_published) {
    InMemoryStorageBackend storage;
    const std::string existingPath = "worlds/existing.bin";
    const std::string missingPath = "worlds/missing.bin";
    const std::string previous = "previous";
    const std::string replacement = "replacement";

    writeText(storage, existingPath, previous);
    {
        auto session = storage.openWrite(existingPath);
        session->writer().writeBytes(
            reinterpret_cast<const uint8_t*>(replacement.data()),
            replacement.size());
        CHECK_EQ(storage.openRead(existingPath)->readAt(0, previous.size()),
                 (std::vector<uint8_t>(previous.begin(), previous.end())));
    }

    auto session = storage.openWrite(missingPath);
    session->writer().writeU8(42);
    session->abort();
    CHECK(!storage.exists(missingPath));
}

std::vector<uint8_t> makeUncompressedFixtureRegion(
    const std::vector<uint8_t>& payload,
    int32_t columnCount,
    int32_t fileVersion = 4) {
    std::vector<uint8_t> bytes;
    InMemoryByteWriter writer(bytes);
    writer.writeI32(static_cast<int32_t>(0xFFECCEAC));
    writer.writeI32(fileVersion);
    writer.writeI32(0);
    writer.writeI32(columnCount);
    writer.writeBytes(payload.data(), payload.size());
    return bytes;
}

std::vector<uint8_t> makeCompressedFixtureRegion(
    const std::vector<uint8_t>& compressed,
    int32_t decompressedSize,
    int32_t columnCount = 0) {
    std::vector<uint8_t> bytes;
    InMemoryByteWriter writer(bytes);
    writer.writeI32(static_cast<int32_t>(0xFFECCEAC));
    writer.writeI32(4);
    writer.writeI32(1);
    writer.writeI32(columnCount);
    writer.writeI32(static_cast<int32_t>(compressed.size()));
    writer.writeI32(decompressedSize);
    writer.writeBytes(compressed.data(), compressed.size());
    return bytes;
}

ChunkRegionSnapshot loadFixtureRegion(
    std::vector<uint8_t> bytes,
    const Rigel::Voxel::BlockRegistry* blockRegistry = nullptr) {
    auto storage = std::make_shared<InMemoryStorageBackend>();
    PersistenceContext context;
    context.rootPath = "worlds/envelope_fixture";
    context.preferredFormat = "cr";
    context.storage = storage;
    if (blockRegistry) {
        context.providers = std::make_shared<ProviderRegistry>();
        context.providers->add(
            kBlockRegistryProviderId,
            std::make_shared<BlockRegistryProvider>(blockRegistry));
    }
    const RegionKey key{"zone:default", 0, 0, 0};
    const std::string path = CRPaths::regionPath(key, context);
    auto session = storage->openWrite(path);
    session->writer().writeBytes(bytes.data(), bytes.size());
    session->commit();

    auto format = Backends::CR::factory()(context);
    return format->chunkContainer().loadRegion(key);
}

std::array<int32_t, 16 * 16> absentFixtureOffsets() {
    std::array<int32_t, 16 * 16> offsets;
    offsets.fill(-1);
    return offsets;
}

std::vector<uint8_t> makeSingleColumnFixtureRegion(
    uint8_t offsetType,
    const std::vector<uint8_t>& column,
    size_t columnIndex = 0,
    int32_t headerColumnCount = 1) {
    auto offsets = absentFixtureOffsets();
    offsets[columnIndex] = 0;
    return makeUncompressedFixtureRegion(
        makeFixturePayload(offsetType, offsets, column),
        headerColumnCount);
}

} // namespace

TEST_CASE(CRMemoryByteReader_bounds_random_access_reads) {
    const std::vector<uint8_t> fixture{10, 20, 30, 40};
    MemoryByteReader reader(fixture);

    CHECK_EQ(reader.readAt(0, fixture.size()), fixture);
    CHECK_EQ(reader.readAt(fixture.size(), 0), std::vector<uint8_t>{});

    reader.seek(1);
    checkMemoryRandomReadFailure(reader, fixture.size() - 1, 2);
    checkMemoryRandomReadFailure(reader, fixture.size() + 1, 0);
    checkMemoryRandomReadFailure(
        reader, std::numeric_limits<size_t>::max(), 0);
    checkMemoryRandomReadFailure(
        reader, 1, std::numeric_limits<size_t>::max());
    checkMemoryRandomReadFailure(
        reader,
        std::numeric_limits<size_t>::max(),
        std::numeric_limits<size_t>::max());
    CHECK_EQ(reader.readU8(), static_cast<uint8_t>(20));
}

TEST_CASE(CRMemoryByteReader_bounds_sequential_reads) {
    MemoryByteReader reader({10, 20, 30, 40});
    checkSequentialReadBounds(reader, "CRMemoryReader read out of range");
}

TEST_CASE(CRBackendInMemoryByteReader_bounds_sequential_reads) {
    InMemoryByteReader reader({10, 20, 30, 40});
    checkSequentialReadBounds(
        reader, "InMemoryByteReader read out of range");
}

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

TEST_CASE(CRBackend_region_accepts_supported_offset_widths) {
    const auto column = makeFixtureColumn(
        4, 1, {makeEmptyFixtureRecord(0, 0, 0)});

    for (uint8_t offsetType : {uint8_t{1}, uint8_t{2}, uint8_t{3}}) {
        auto loaded = loadFixtureRegion(
            makeSingleColumnFixtureRegion(offsetType, column));
        CHECK_EQ(loaded.chunks.size(), 1u);
        CHECK_EQ(loaded.chunks.front().key,
                 (ChunkKey{"zone:default", 0, 0, 0}));
    }
}

TEST_CASE(CRBackend_region_rejects_unsupported_versions_and_offset_types) {
    auto offsets = absentFixtureOffsets();
    const auto emptyPayload = makeFixturePayload(1, offsets);
    for (int32_t version : {3, 5}) {
        checkCRRegionError(
            [&]() { loadFixtureRegion(makeUncompressedFixtureRegion(
                emptyPayload, 0, version)); },
            "CRRegion: unsupported file version");
    }

    for (int32_t version : {3, 5}) {
        const auto column = makeFixtureColumn(
            version, 1, {makeEmptyFixtureRecord(0, 0, 0)});
        checkCRRegionError(
            [&]() { loadFixtureRegion(makeSingleColumnFixtureRegion(1, column)); },
            "CRRegion: unsupported column version");
    }

    for (uint8_t offsetType : {uint8_t{0}, uint8_t{4}}) {
        checkCRRegionError(
            [&]() { loadFixtureRegion(makeUncompressedFixtureRegion(
                makeFixturePayload(offsetType, offsets), 0)); },
            "CRRegion: unsupported offset type");
    }

    for (int32_t compressionType : {-1, 2}) {
        auto region = makeUncompressedFixtureRegion(emptyPayload, 0);
        overwriteFixtureI32(region, 8, compressionType);
        checkCRRegionError(
            [&]() { loadFixtureRegion(region); },
            "CRRegion: unknown compression type");
    }
}

TEST_CASE(CRBackend_region_rejects_invalid_offsets_and_column_extents) {
    const auto column = makeFixtureColumn(
        4, 1, {makeEmptyFixtureRecord(0, 0, 0)});

    for (uint8_t offsetType : {uint8_t{1}, uint8_t{2}, uint8_t{3}}) {
        auto negativeOffsets = absentFixtureOffsets();
        negativeOffsets[0] = -2;
        checkCRRegionError(
            [&]() { loadFixtureRegion(makeUncompressedFixtureRegion(
                makeFixturePayload(offsetType, negativeOffsets, column), 1)); },
            "CRRegion: invalid negative column offset");
    }

    auto outsideOffsets = absentFixtureOffsets();
    outsideOffsets[0] = static_cast<int32_t>(column.size());
    checkCRRegionError(
        [&]() { loadFixtureRegion(makeUncompressedFixtureRegion(
            makeFixturePayload(3, outsideOffsets, column), 1)); },
        "CRRegion: column offset exceeds payload");

    auto unboundedOffsets = absentFixtureOffsets();
    unboundedOffsets[0] = std::numeric_limits<int32_t>::max();
    checkCRRegionError(
        [&]() { loadFixtureRegion(makeUncompressedFixtureRegion(
            makeFixturePayload(3, unboundedOffsets, column), 1)); },
        "CRRegion: column offset exceeds payload");

    for (int32_t extent : {-1, 0, 8}) {
        auto invalidColumn = column;
        overwriteFixtureI32(invalidColumn, 0, extent);
        checkCRRegionError(
            [&]() { loadFixtureRegion(makeSingleColumnFixtureRegion(
                1, invalidColumn)); },
            "CRRegion: column extent is smaller than header");
    }

    auto oversizedColumn = column;
    overwriteFixtureI32(
        oversizedColumn, 0, static_cast<int32_t>(oversizedColumn.size() + 1));
    checkCRRegionError(
        [&]() { loadFixtureRegion(makeSingleColumnFixtureRegion(
            1, oversizedColumn)); },
        "CRRegion: column extent exceeds payload");

    auto unboundedColumn = column;
    overwriteFixtureI32(
        unboundedColumn, 0, std::numeric_limits<int32_t>::max());
    checkCRRegionError(
        [&]() { loadFixtureRegion(makeSingleColumnFixtureRegion(
            1, unboundedColumn)); },
        "CRRegion: column extent exceeds format limit");
}

TEST_CASE(CRBackend_region_rejects_duplicate_and_overlapping_columns) {
    const auto column = makeFixtureColumn(
        4, 1, {makeEmptyFixtureRecord(0, 0, 0)});
    std::array<int32_t, 16 * 16> repeatedOffsets;
    repeatedOffsets.fill(0);
    const auto repeated = makeUncompressedFixtureRegion(
        makeFixturePayload(1, repeatedOffsets, column), 16 * 16);

    for (int attempt = 0; attempt < 2; ++attempt) {
        checkCRRegionError(
            [&]() { loadFixtureRegion(repeated); },
            "CRRegion: duplicate column offset");
    }

    auto overlappingBytes = column;
    const auto secondColumn = makeFixtureColumn(4, 0);
    overlappingBytes.insert(
        overlappingBytes.end(), secondColumn.begin(), secondColumn.end());
    overwriteFixtureI32(
        overlappingBytes, 0, static_cast<int32_t>(overlappingBytes.size()));
    auto overlappingOffsets = absentFixtureOffsets();
    overlappingOffsets[0] = 0;
    overlappingOffsets[1] = static_cast<int32_t>(column.size());
    checkCRRegionError(
        [&]() { loadFixtureRegion(makeUncompressedFixtureRegion(
            makeFixturePayload(2, overlappingOffsets, overlappingBytes), 2)); },
        "CRRegion: overlapping column extents");
}

TEST_CASE(CRBackend_region_rejects_header_and_chunk_count_mismatches) {
    auto offsets = absentFixtureOffsets();
    const auto emptyPayload = makeFixturePayload(1, offsets);
    for (int32_t columnCount : {-1, 16 * 16 + 1}) {
        checkCRRegionError(
            [&]() { loadFixtureRegion(makeUncompressedFixtureRegion(
                emptyPayload, columnCount)); },
            "CRRegion: invalid column count");
    }

    const auto validColumn = makeFixtureColumn(
        4, 1, {makeEmptyFixtureRecord(0, 0, 0)});
    for (int32_t columnCount : {0, 2}) {
        checkCRRegionError(
            [&]() { loadFixtureRegion(makeSingleColumnFixtureRegion(
                1, validColumn, 0, columnCount)); },
            "CRRegion: column count does not match offset table");
    }

    std::vector<std::vector<uint8_t>> boundaryRecords;
    for (int32_t y = 0; y < 16; ++y) {
        boundaryRecords.push_back(makeEmptyFixtureRecord(0, y, 0));
    }
    auto boundary = loadFixtureRegion(makeSingleColumnFixtureRegion(
        2, makeFixtureColumn(4, 16, boundaryRecords)));
    CHECK_EQ(boundary.chunks.size(), 16u);

    checkCRRegionError(
        [&]() { loadFixtureRegion(makeSingleColumnFixtureRegion(
            1, makeFixtureColumn(4, 17))); },
        "CRRegion: column chunk count exceeds format limit");
    checkCRRegionError(
        [&]() { loadFixtureRegion(makeSingleColumnFixtureRegion(
            1, makeFixtureColumn(4, 1))); },
        "CRRegion: column chunk count exceeds payload");
    checkCRRegionError(
        [&]() { loadFixtureRegion(makeSingleColumnFixtureRegion(
            1, makeFixtureColumn(4, 0, {}, {0}))); },
        "CRRegion: column chunk count does not consume payload");
}

TEST_CASE(CRBackend_region_rejects_out_of_region_chunk_coordinates) {
    for (const auto& record : {
             makeEmptyFixtureRecord(1, 0, 0),
             makeEmptyFixtureRecord(0, 16, 0),
             makeEmptyFixtureRecord(0, 0, 1)}) {
        const auto column = makeFixtureColumn(4, 1, {record});
        checkCRRegionError(
            [&]() { loadFixtureRegion(makeSingleColumnFixtureRegion(1, column)); },
            "CRRegion: chunk coordinates do not match region column");
    }
}

TEST_CASE(CRBackend_region_rejects_duplicate_chunk_coordinates) {
    const auto record = makeEmptyFixtureRecord(0, 0, 0);
    checkCRRegionError(
        [&]() { loadFixtureRegion(makeSingleColumnFixtureRegion(
            1, makeFixtureColumn(4, 2, {record, record}))); },
        "CRRegion: duplicate chunk coordinates in column");
}

TEST_CASE(CRBackend_chunk_rejects_invalid_palette_declarations_and_references) {
    for (int32_t paletteSize : {
             -1,
             0,
             16 * 16 * 16 + 1,
             std::numeric_limits<int32_t>::max()}) {
        checkCRRegionError(
            [&]() { loadFixtureRegion(makeSingleColumnFixtureRegion(
                1, makeFixtureColumn(4, 1, {makeLayeredFixtureRecord(paletteSize)}))); },
            "CRChunkCodec: invalid palette size");
    }

    checkCRRegionError(
        [&]() { loadFixtureRegion(makeSingleColumnFixtureRegion(
            1, makeFixtureColumn(4, 1, {makeLayeredFixtureRecord(1)}))); },
        "CRChunkCodec: palette exceeds column extent");

    Rigel::Voxel::BlockRegistry registry;
    for (int32_t paletteIndex : {-65'536, 65'536}) {
        checkCRRegionError(
            [&]() { loadFixtureRegion(makeSingleColumnFixtureRegion(
                1, makeFixtureColumn(
                    4, 1, {makeInvalidPaletteReferenceRecord(paletteIndex)})),
                &registry); },
            "CRChunkCodec: palette index out of range");
    }
}

TEST_CASE(CRBackend_chunk_rejects_invalid_string_declarations) {
    constexpr int32_t kMaxStringBytes = 1'048'576;
    for (int32_t stringSize : {
             -1,
             kMaxStringBytes + 1,
             std::numeric_limits<int32_t>::max()}) {
        checkCRRegionError(
            [&]() { loadFixtureRegion(makeSingleColumnFixtureRegion(
                1, makeFixtureColumn(4, 1, {makeSingleBlockFixtureRecord(stringSize)}))); },
            stringSize < 0
                ? "CRChunkCodec: invalid string length"
                : "CRChunkCodec: string length exceeds format limit");
    }

    checkCRRegionError(
        [&]() { loadFixtureRegion(makeSingleColumnFixtureRegion(
            1, makeFixtureColumn(4, 1, {makeSingleBlockFixtureRecord(2, {'x'})}))); },
        "CRChunkCodec: string length exceeds column extent");
}

TEST_CASE(CRBackend_chunk_validates_block_entity_extents) {
    auto zero = loadFixtureRegion(makeSingleColumnFixtureRegion(
        1, makeFixtureColumn(4, 1, {makeBlockEntityFixtureRecord(0)})));
    CHECK_EQ(zero.chunks.size(), 1u);

    checkCRRegionError(
        [&]() { loadFixtureRegion(makeSingleColumnFixtureRegion(
            1, makeFixtureColumn(4, 1, {makeBlockEntityFixtureRecord(-1)}))); },
        "CRChunkCodec: invalid block entity size");

    for (int32_t entitySize : {1'048'577, std::numeric_limits<int32_t>::max()}) {
        checkCRRegionError(
            [&]() { loadFixtureRegion(makeSingleColumnFixtureRegion(
                1, makeFixtureColumn(4, 1, {makeBlockEntityFixtureRecord(entitySize)}))); },
            "CRChunkCodec: block entity size exceeds format limit");
    }
    checkCRRegionError(
        [&]() { loadFixtureRegion(makeSingleColumnFixtureRegion(
            1, makeFixtureColumn(4, 1, {makeBlockEntityFixtureRecord(2, {0})}))); },
        "CRChunkCodec: block entity size exceeds column extent");
}

TEST_CASE(CRBackend_writer_rejects_duplicate_chunks_before_commit) {
    auto storage = std::make_shared<InMemoryStorageBackend>();
    PersistenceContext context;
    context.rootPath = "worlds/writer_duplicate";
    context.preferredFormat = "cr";
    context.storage = storage;
    auto format = Backends::CR::factory()(context);

    ChunkSnapshot chunk;
    chunk.key = ChunkKey{"zone:default", 0, 0, 0};
    chunk.data = makeMinimalChunkData(chunk.key);
    ChunkRegionSnapshot valid{RegionKey{"zone:default", 0, 0, 0}, {chunk}};
    format->chunkContainer().saveRegion(valid);
    const auto path = CRPaths::regionPath(valid.key, context);
    const auto original = readAll(*storage, path);

    ChunkRegionSnapshot duplicate = valid;
    duplicate.chunks.push_back(chunk);
    checkCRRegionError(
        [&]() { format->chunkContainer().saveRegion(duplicate); },
        "CRRegion: duplicate chunk coordinates");
    CHECK_EQ(readAll(*storage, path), original);

    ChunkRegionSnapshot mismatchedZone = valid;
    mismatchedZone.chunks.front().key.zoneId = "zone:other";
    checkCRRegionError(
        [&]() { format->chunkContainer().saveRegion(mismatchedZone); },
        "CRRegion: chunk zone does not match region");
    CHECK_EQ(readAll(*storage, path), original);

    ChunkRegionSnapshot mismatchedSpan = valid;
    mismatchedSpan.chunks.front().data.span.offsetX = 16;
    checkCRRegionError(
        [&]() { format->chunkContainer().saveRegion(mismatchedSpan); },
        "CRChunkCodec: chunk span does not match storage key");
    CHECK_EQ(readAll(*storage, path), original);
}

TEST_CASE(CRBackend_writer_enforces_string_and_record_limits_before_commit) {
    constexpr size_t kMaxStringBytes = 1'048'576;
    constexpr size_t kMaxRecordBytes = 4 * 1024 * 1024;
    auto storage = std::make_shared<InMemoryStorageBackend>();
    Rigel::Voxel::BlockRegistry registry;
    auto providers = std::make_shared<ProviderRegistry>();
    providers->add(
        kBlockRegistryProviderId,
        std::make_shared<BlockRegistryProvider>(&registry));

    PersistenceContext context;
    context.rootPath = "worlds/writer_limits";
    context.preferredFormat = "cr";
    context.storage = storage;
    context.providers = providers;
    auto format = Backends::CR::factory()(context);

    ChunkSnapshot chunk;
    chunk.key = ChunkKey{"zone:default", 0, 0, 0};
    chunk.data = makeMinimalChunkData(chunk.key);
    ChunkRegionSnapshot valid{RegionKey{"zone:default", 0, 0, 0}, {chunk}};
    format->chunkContainer().saveRegion(valid);
    const auto path = CRPaths::regionPath(valid.key, context);
    const auto original = readAll(*storage, path);

    const auto oversizedId = registerTestBlock(
        registry, std::string(kMaxStringBytes + 1, 'x'));
    ChunkRegionSnapshot oversizedString = valid;
    oversizedString.chunks.front().data.blocks.front().id = oversizedId;
    checkCRRegionError(
        [&]() { format->chunkContainer().saveRegion(oversizedString); },
        "CRChunkCodec: string length exceeds format limit");
    CHECK_EQ(readAll(*storage, path), original);

    ChunkRegionSnapshot oversizedRecord = valid;
    oversizedRecord.chunks.front().opaquePayload.resize(kMaxRecordBytes + 1);
    checkCRRegionError(
        [&]() { format->chunkContainer().saveRegion(oversizedRecord); },
        "CRChunkCodec: record exceeds format limit");
    CHECK_EQ(readAll(*storage, path), original);
}

TEST_CASE(CRBackend_writer_enforces_aggregate_region_limit_before_commit) {
    constexpr size_t kIdentifierBytes = 1'048'200;
    auto storage = std::make_shared<InMemoryStorageBackend>();
    Rigel::Voxel::BlockRegistry registry;
    std::array<Rigel::Voxel::BlockID, 4> blockIds;
    for (size_t i = 0; i < blockIds.size(); ++i) {
        blockIds[i] = registerTestBlock(
            registry,
            std::string(kIdentifierBytes, static_cast<char>('a' + i)));
    }
    auto providers = std::make_shared<ProviderRegistry>();
    providers->add(
        kBlockRegistryProviderId,
        std::make_shared<BlockRegistryProvider>(&registry));

    PersistenceContext context;
    context.rootPath = "worlds/writer_region_limit";
    context.preferredFormat = "cr";
    context.storage = storage;
    context.providers = providers;
    auto format = Backends::CR::factory()(context);

    ChunkSnapshot originalChunk;
    originalChunk.key = ChunkKey{"zone:default", 0, 0, 0};
    originalChunk.data = makeMinimalChunkData(originalChunk.key);
    ChunkRegionSnapshot originalRegion{
        RegionKey{"zone:default", 0, 0, 0}, {originalChunk}};
    format->chunkContainer().saveRegion(originalRegion);
    const auto path = CRPaths::regionPath(originalRegion.key, context);
    const auto originalBytes = readAll(*storage, path);
    const size_t originalWriteSessions = storage->writeSessionCount();

    ChunkRegionSnapshot oversized;
    oversized.key = originalRegion.key;
    for (int32_t i = 0; i < 17; ++i) {
        ChunkSnapshot chunk;
        chunk.key = ChunkKey{
            "zone:default", i % 16, 0, i / 16};
        chunk.data = makeMinimalChunkData(chunk.key);
        for (size_t block = 0; block < chunk.data.blocks.size(); ++block) {
            chunk.data.blocks[block].id = blockIds[block % blockIds.size()];
        }
        oversized.chunks.push_back(std::move(chunk));
    }

    checkCRRegionError(
        [&]() { format->chunkContainer().saveRegion(oversized); },
        "CRRegion: region payload exceeds format limit");
    CHECK_EQ(storage->writeSessionCount(), originalWriteSessions);
    CHECK_EQ(readAll(*storage, path), originalBytes);
}

TEST_CASE(CRBackend_entity_regions_validate_records_before_commit) {
    auto storage = std::make_shared<InMemoryStorageBackend>();
    PersistenceContext context;
    context.rootPath = "worlds/entity_record_limits";
    context.preferredFormat = "cr";
    context.storage = storage;
    auto format = Backends::CR::factory()(context);

    EntityPersistedChunk chunk;
    chunk.coord = Rigel::Voxel::ChunkCoord{0, 0, 0};
    EntityRegionSnapshot valid{
        EntityRegionKey{"zone:default", 0, 0, 0}, {chunk}};
    format->entityContainer().saveRegion(valid);
    const auto path = CRPaths::entityRegionPath(valid.key, context);
    const auto original = readAll(*storage, path);

    EntityRegionSnapshot duplicate = valid;
    duplicate.chunks.push_back(chunk);
    checkCRRegionError(
        [&]() { format->entityContainer().saveRegion(duplicate); },
        "CRFormat: duplicate entity chunk coordinates");
    CHECK_EQ(readAll(*storage, path), original);

    EntityRegionSnapshot outside = valid;
    outside.chunks.front().coord.x = 16;
    checkCRRegionError(
        [&]() { format->entityContainer().saveRegion(outside); },
        "CRFormat: entity chunk lies outside its region");
    CHECK_EQ(readAll(*storage, path), original);

    EntityPersistedEntity invalidEntity;
    invalidEntity.position.x = std::numeric_limits<float>::infinity();
    EntityRegionSnapshot invalidVector = valid;
    invalidVector.chunks.front().entities.push_back(invalidEntity);
    checkCRRegionError(
        [&]() { format->entityContainer().saveRegion(invalidVector); },
        "CRFormat: invalid persistent entity position");
    CHECK_EQ(readAll(*storage, path), original);
}

TEST_CASE(CRBackend_entity_regions_reject_malformed_and_duplicate_records) {
    auto storage = std::make_shared<InMemoryStorageBackend>();
    PersistenceContext context;
    context.rootPath = "worlds/entity_record_input";
    context.preferredFormat = "cr";
    context.storage = storage;
    auto format = Backends::CR::factory()(context);
    const EntityRegionKey key{"zone:default", 0, 0, 0};
    const auto path = CRPaths::entityRegionPath(key, context);

    createEmptyFile(*storage, path);
    checkCRRegionError(
        [&]() { format->entityContainer().loadRegion(key); },
        "CRFormat: failed to decode entity region");

    storage->remove(path);
    EntityPersistedChunk chunk;
    chunk.coord = Rigel::Voxel::ChunkCoord{0, 0, 0};
    const auto duplicatePayload = Rigel::Entity::encodeEntityRegionPayload({chunk, chunk});
    auto session = storage->openWrite(path);
    session->writer().writeBytes(duplicatePayload.data(), duplicatePayload.size());
    session->commit();
    checkCRRegionError(
        [&]() { format->entityContainer().loadRegion(key); },
        "CRFormat: duplicate entity chunk coordinates");
}

TEST_CASE(CRBackend_entity_validation_precedes_live_world_mutation) {
    auto storage = std::make_shared<InMemoryStorageBackend>();
    FormatRegistry registry;
    registry.registerFormat(
        Backends::CR::descriptor(), Backends::CR::factory(), Backends::CR::probe());
    PersistenceService service(registry);
    PersistenceContext context;
    context.rootPath = "worlds/entity_validation_order";
    context.preferredFormat = "cr";
    context.storage = storage;

    const EntityRegionKey key{"rigel:default", 0, 0, 0};
    createEmptyFile(
        *storage, CRPaths::zoneRoot(key.zoneId, context) + "/entities");
    createEmptyFile(*storage, CRPaths::entityRegionPath(key, context));

    Rigel::Voxel::WorldResources resources;
    Rigel::Voxel::World world(resources);
    auto existing = std::make_unique<Rigel::Entity::Entity>("base:existing");
    const Rigel::Entity::EntityId existingId{1, 2, 3};
    existing->setId(existingId);
    CHECK_EQ(world.entities().spawn(std::move(existing)), existingId);
    context.providers = world.persistenceProvidersHandle();
    Rigel::Asset::AssetManager assets;

    checkCRRegionError(
        [&]() { loadBootstrapEntities(
            world, assets, service, context); },
        "CRFormat: failed to decode entity region");
    CHECK(world.entities().get(existingId) != nullptr);
}

TEST_CASE(CRBackend_region_rejects_unbounded_compression_declarations) {
    auto compressedLimit = makeCompressedFixtureRegion({}, 1);
    overwriteFixtureI32(
        compressedLimit, 16, std::numeric_limits<int32_t>::max());
    checkCRRegionError(
        [&]() { loadFixtureRegion(compressedLimit); },
        "CRRegion: compressed size exceeds format limit");

    auto decompressedLimit = makeCompressedFixtureRegion({0}, 1);
    overwriteFixtureI32(
        decompressedLimit, 20, std::numeric_limits<int32_t>::max());
    checkCRRegionError(
        [&]() { loadFixtureRegion(decompressedLimit); },
        "CRRegion: decompressed size exceeds format limit");

    checkCRRegionError(
        [&]() { loadFixtureRegion(makeCompressedFixtureRegion({}, 1)); },
        "CRRegion: invalid compressed sizes");

    auto truncated = makeCompressedFixtureRegion({0}, 1);
    overwriteFixtureI32(truncated, 16, 2);
    checkCRRegionError(
        [&]() { loadFixtureRegion(truncated); },
        "CRRegion: compressed size exceeds remaining input");
}

TEST_CASE(CRBackend_region_requires_exact_lz4_output_size) {
    if (!CRLz4::available()) {
        SKIP_TEST("LZ4 not available");
    }

    const auto payload = makeFixturePayload(1, absentFixtureOffsets());
    std::vector<uint8_t> compressed(
        static_cast<size_t>(CRLz4::compressBound(static_cast<int>(payload.size()))));
    int compressedSize = CRLz4::compress(
        payload.data(), payload.size(), compressed.data(), compressed.size());
    CHECK(compressedSize > 0);
    compressed.resize(static_cast<size_t>(compressedSize));

    checkCRRegionError(
        [&]() { loadFixtureRegion(makeCompressedFixtureRegion(
            compressed, static_cast<int32_t>(payload.size() + 1))); },
        "CRRegion: decompressed size does not match declaration");
}

TEST_CASE(CRBackend_region_rejects_corrupt_lz4_payload) {
    if (!CRLz4::available()) {
        SKIP_TEST("LZ4 not available");
    }

    checkCRRegionError(
        [&]() { loadFixtureRegion(makeCompressedFixtureRegion(
            {0xFF, 0xFF, 0xFF, 0xFF}, 16)); },
        "CRRegion: LZ4 decompression failed");
}

TEST_CASE(CRBackend_dirty_save_preserves_untouched_record_bytes) {
    auto storage = std::make_shared<InMemoryStorageBackend>();
    Rigel::Voxel::WorldResources resources;
    Rigel::Voxel::BlockID stone = registerTestBlock(resources.registry(), "base:test_stone");
    Rigel::Voxel::World world(resources);
    world.setBlock(0, 0, 0, Rigel::Voxel::BlockState{stone});

    FormatRegistry registry;
    registry.registerFormat(Backends::CR::descriptor(), Backends::CR::factory(), Backends::CR::probe());
    PersistenceService service(registry);

    PersistenceContext context;
    context.rootPath = "worlds/preserve_record";
    context.preferredFormat = "cr";
    context.storage = storage;
    context.providers = world.persistenceProvidersHandle();
    RegionKey regionKey{"rigel:default", 0, 0, 0};
    const std::string path = CRPaths::regionPath(regionKey, context);
    auto changedRecord = makeFixtureRecord(
        ChunkKey{regionKey.zoneId, 0, 0, 0}, "base:test_stone", false);
    auto untouchedRecord = makeFixtureRecord(
        ChunkKey{regionKey.zoneId, 2, 0, 0}, "base:unknown_id", true);

    for (UnknownIdPolicy policy : {UnknownIdPolicy::Placeholder, UnknownIdPolicy::Skip}) {
        context.policies.unknownBlockPolicy = policy;
        writeFixtureRegion(*storage, path, {changedRecord, untouchedRecord});
        auto untouchedBefore = readOnlyRecordInColumn(*storage, path, 2);

        saveChunkToDisk(world, service, context, Rigel::Voxel::ChunkCoord{0, 0, 0});

        auto untouchedAfter = readOnlyRecordInColumn(*storage, path, 2);
        CHECK_EQ(untouchedAfter, untouchedBefore);
    }
}

TEST_CASE(CRBackend_unknown_identifier_fail_policy_preserves_region) {
    auto storage = std::make_shared<InMemoryStorageBackend>();
    Rigel::Voxel::WorldResources resources;
    Rigel::Voxel::BlockID stone = registerTestBlock(resources.registry(), "base:test_stone");
    Rigel::Voxel::World world(resources);
    world.setBlock(0, 0, 0, Rigel::Voxel::BlockState{stone});

    FormatRegistry registry;
    registry.registerFormat(Backends::CR::descriptor(), Backends::CR::factory(), Backends::CR::probe());
    PersistenceService service(registry);

    PersistenceContext context;
    context.rootPath = "worlds/reject_unknown";
    context.preferredFormat = "cr";
    context.storage = storage;
    context.providers = world.persistenceProvidersHandle();

    RegionKey regionKey{"rigel:default", 0, 0, 0};
    const std::string path = CRPaths::regionPath(regionKey, context);
    writeFixtureRegion(*storage, path, {
        makeFixtureRecord(ChunkKey{regionKey.zoneId, 0, 0, 0}, "base:test_stone", false),
        makeFixtureRecord(ChunkKey{regionKey.zoneId, 2, 0, 0}, "base:unknown_id", true)
    });
    auto regionBefore = readAll(*storage, path);

    CHECK_THROWS(service.loadRegion(regionKey, context));
    CHECK_EQ(readAll(*storage, path), regionBefore);
    CHECK_THROWS(saveChunkToDisk(world, service, context, Rigel::Voxel::ChunkCoord{0, 0, 0}));
    CHECK_EQ(readAll(*storage, path), regionBefore);
}

TEST_CASE(CRBackend_modified_optional_payload_record_is_rejected) {
    auto storage = std::make_shared<InMemoryStorageBackend>();
    Rigel::Voxel::WorldResources resources;
    Rigel::Voxel::BlockID stone = registerTestBlock(resources.registry(), "base:test_stone");
    Rigel::Voxel::World world(resources);
    world.setBlock(0, 0, 0, Rigel::Voxel::BlockState{stone});

    FormatRegistry registry;
    registry.registerFormat(Backends::CR::descriptor(), Backends::CR::factory(), Backends::CR::probe());
    PersistenceService service(registry);

    PersistenceContext context;
    context.rootPath = "worlds/reject_optional_payload";
    context.preferredFormat = "cr";
    context.storage = storage;
    context.providers = world.persistenceProvidersHandle();

    RegionKey regionKey{"rigel:default", 0, 0, 0};
    const std::string path = CRPaths::regionPath(regionKey, context);
    writeFixtureRegion(*storage, path, {
        makeFixtureRecord(ChunkKey{regionKey.zoneId, 0, 0, 0}, "base:test_stone", true),
        makeFixtureRecord(ChunkKey{regionKey.zoneId, 2, 0, 0}, "base:test_stone", false)
    });
    auto regionBefore = readAll(*storage, path);

    CHECK_THROWS(saveChunkToDisk(world, service, context, Rigel::Voxel::ChunkCoord{0, 0, 0}));
    CHECK_EQ(readAll(*storage, path), regionBefore);
}

TEST_CASE(CRBackend_unrepresentable_block_state_is_rejected) {
    auto storage = std::make_shared<InMemoryStorageBackend>();
    Rigel::Voxel::WorldResources resources;
    Rigel::Voxel::BlockID stone = registerTestBlock(resources.registry(), "base:test_stone");
    Rigel::Voxel::World world(resources);

    FormatRegistry registry;
    registry.registerFormat(Backends::CR::descriptor(), Backends::CR::factory(), Backends::CR::probe());
    PersistenceService service(registry);

    PersistenceContext context;
    context.rootPath = "worlds/reject_block_state";
    context.preferredFormat = "cr";
    context.storage = storage;
    context.providers = world.persistenceProvidersHandle();

    RegionKey regionKey{"rigel:default", 0, 0, 0};
    const std::string path = CRPaths::regionPath(regionKey, context);
    auto originalRecord = makeFixtureRecord(
        ChunkKey{regionKey.zoneId, 0, 0, 0}, "base:test_stone", false);

    auto rejectState = [&](Rigel::Voxel::BlockState state) {
        writeFixtureRegion(*storage, path, {originalRecord});
        auto regionBefore = readAll(*storage, path);
        world.setBlock(0, 0, 0, state);
        CHECK_THROWS(saveChunkToDisk(
            world, service, context, Rigel::Voxel::ChunkCoord{0, 0, 0}));
        CHECK_EQ(readAll(*storage, path), regionBefore);
    };

    rejectState(Rigel::Voxel::BlockState{stone, 1, 0});
    rejectState(Rigel::Voxel::BlockState{stone, 0, 0x21});
}

TEST_CASE(CRBackend_region_discovery_requires_canonical_filenames) {
    auto storage = std::make_shared<InMemoryStorageBackend>();
    PersistenceContext context;
    context.rootPath = "worlds/test";
    context.storage = storage;
    const std::string zoneId = "zone:default";
    const auto zoneRoot = CRPaths::zoneRoot(zoneId, context);

    const auto regionsRoot = zoneRoot + "/regions";
    createEmptyFile(*storage, regionsRoot);
    createEmptyFile(*storage, regionsRoot + "/region_-12_3_0.cosmicreach");
    createEmptyFile(*storage, regionsRoot + "/region_4_5_6.cosmicreach.tmp.123");
    createEmptyFile(*storage, regionsRoot + "/region_7_8_9.cosmicreach.bak");
    createEmptyFile(*storage, regionsRoot + "/region_01_2_3.cosmicreach");
    createEmptyFile(*storage, regionsRoot + "/region_10_11_12");

    const auto entitiesRoot = zoneRoot + "/entities";
    createEmptyFile(*storage, entitiesRoot);
    createEmptyFile(*storage, entitiesRoot + "/entityRegion_6_-7_8.crbin");
    createEmptyFile(*storage, entitiesRoot + "/entityRegion_1_2_3.crbin.tmp.456");
    createEmptyFile(*storage, entitiesRoot + "/entityRegion_4_5_6.crbin.bak");
    createEmptyFile(*storage, entitiesRoot + "/entityRegion_-0_7_8.crbin");
    createEmptyFile(*storage, entitiesRoot + "/entityRegion_9_10_11");

    auto format = Backends::CR::factory()(context);
    const auto regions = format->chunkContainer().listRegions(zoneId);
    const auto entityRegions = format->entityContainer().listRegions(zoneId);

    CHECK_EQ(regions.size(), 1u);
    CHECK_EQ(regions.front(), (RegionKey{zoneId, -12, 3, 0}));
    CHECK_EQ(entityRegions.size(), 1u);
    CHECK_EQ(entityRegions.front(), (EntityRegionKey{zoneId, 6, -7, 8}));
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

    service.saveWorld(world, context);
    auto loaded = service.loadWorldMetadata(context);

    CHECK_EQ(loaded.worldId, "demo");
    CHECK_EQ(loaded.displayName, "Demo World");
}

TEST_CASE(CRBackend_world_metadata_escapes_json_strings) {
    auto storage = std::make_shared<InMemoryStorageBackend>();
    FormatRegistry registry;
    registry.registerFormat(Backends::CR::descriptor(), Backends::CR::factory(), Backends::CR::probe());
    PersistenceService service(registry);

    PersistenceContext context;
    context.rootPath = "worlds/escaped_metadata";
    context.preferredFormat = "cr";
    context.storage = storage;

    WorldSnapshot world;
    world.metadata.worldId = "escaped_metadata";
    world.metadata.displayName =
        std::string("Quoted \"world\" at C:\\saves\nnext") +
        '\b' + '\f' + '\r' + '\t' + '\x01';

    service.saveWorld(world, context);

    const auto bytes = readAll(*storage, CRPaths::worldInfoPath(context));
    const std::string document(bytes.begin(), bytes.end());
    CHECK(document.find("\\\"") != std::string::npos);
    CHECK(document.find("\\\\") != std::string::npos);
    CHECK(document.find("\\n") != std::string::npos);
    CHECK(document.find("\\b") != std::string::npos);
    CHECK(document.find("\\f") != std::string::npos);
    CHECK(document.find("\\r") != std::string::npos);
    CHECK(document.find("\\t") != std::string::npos);
    CHECK(document.find("\\u0001") != std::string::npos);
    CHECK(document.find('\x01') == std::string::npos);
    CHECK_EQ(service.loadWorldMetadata(context), world.metadata);
}

TEST_CASE(CRBackend_zone_metadata_decodes_json_unicode_escape) {
    auto storage = std::make_shared<InMemoryStorageBackend>();
    FormatRegistry registry;
    registry.registerFormat(Backends::CR::descriptor(), Backends::CR::factory(), Backends::CR::probe());
    PersistenceService service(registry);

    PersistenceContext context;
    context.rootPath = "worlds/escaped_zone_metadata";
    context.preferredFormat = "cr";
    context.storage = storage;

    const ZoneKey key{"rigel:default"};
    writeText(
        *storage,
        CRPaths::zoneInfoPath(key, context),
        R"({"zoneId":"rigel\u003adefault"})");

    CHECK_EQ(
        service.loadZoneMetadata(key, context),
        (ZoneMetadata{"rigel:default", "rigel:default"}));
}

TEST_CASE(CRBackend_metadata_uses_only_root_object_members) {
    auto storage = std::make_shared<InMemoryStorageBackend>();
    FormatRegistry registry;
    registry.registerFormat(
        Backends::CR::descriptor(),
        Backends::CR::factory(),
        Backends::CR::probe());
    PersistenceService service(registry);

    PersistenceContext context;
    context.rootPath = "worlds/root_metadata";
    context.preferredFormat = "cr";
    context.storage = storage;

    const std::string worldInfo = R"({
  "extension": {
    "defaultZoneId": "base:moon",
    "worldDisplayName": "Nested Object"
  },
  "entries": [
    {
      "defaultZoneId": "base:void",
      "worldDisplayName": "Nested Array"
    }
  ],
  "defaultZoneId": "rigel:default",
  "worldDisplayName": "Root World"
}
)";
    const std::string zoneInfo = R"({
  "extension": {"zoneId": "base:moon"},
  "entries": [{"zoneId": "base:void"}],
  "zoneId": "rigel:default"
}
)";
    const ZoneKey zoneKey{"rigel:default"};
    const auto worldPath = CRPaths::worldInfoPath(context);
    const auto zonePath = CRPaths::zoneInfoPath(zoneKey, context);
    writeText(*storage, worldPath, worldInfo);
    writeText(*storage, zonePath, zoneInfo);

    CHECK_EQ(
        service.loadWorldMetadata(context),
        (WorldMetadata{"root_metadata", "Root World"}));
    CHECK_EQ(
        service.loadZoneMetadata(zoneKey, context),
        (ZoneMetadata{"rigel:default", "rigel:default"}));

    saveCRWorld(storage, context.rootPath, true);

    CHECK_EQ(
        readAll(*storage, worldPath),
        (std::vector<uint8_t>(worldInfo.begin(), worldInfo.end())));
    CHECK_EQ(
        readAll(*storage, zonePath),
        (std::vector<uint8_t>(zoneInfo.begin(), zoneInfo.end())));
}

TEST_CASE(CRBackend_world_metadata_preserves_utf8_and_decodes_unicode) {
    auto storage = std::make_shared<InMemoryStorageBackend>();
    FormatRegistry registry;
    registry.registerFormat(
        Backends::CR::descriptor(),
        Backends::CR::factory(),
        Backends::CR::probe());
    PersistenceService service(registry);

    PersistenceContext context;
    context.rootPath = "worlds/unicode_metadata";
    context.preferredFormat = "cr";
    context.storage = storage;

    const std::string utf8Name =
        std::string("Caf\xc3\xa9 \xe4\xb8\x96\xe7\x95\x8c ") +
        "\xf0\x9f\x9a\x80";
    WorldSnapshot world;
    world.metadata = WorldMetadata{"unicode_metadata", utf8Name};
    service.saveWorld(world, context);

    const auto worldPath = CRPaths::worldInfoPath(context);
    const auto rawDocument = readAll(*storage, worldPath);
    CHECK(
        std::string(rawDocument.begin(), rawDocument.end()).find(utf8Name) !=
        std::string::npos);
    CHECK_EQ(service.loadWorldMetadata(context), world.metadata);

    writeText(
        *storage,
        worldPath,
        R"({"worldDisplayName":"Caf\u00e9 \u4e16\u754c \ud83d\ude80"})");
    CHECK_EQ(
        service.loadWorldMetadata(context),
        (WorldMetadata{"unicode_metadata", utf8Name}));
}

TEST_CASE(CRBackend_world_metadata_enforces_string_and_document_boundaries) {
    auto storage = std::make_shared<InMemoryStorageBackend>();
    FormatRegistry registry;
    registry.registerFormat(Backends::CR::descriptor(), Backends::CR::factory(), Backends::CR::probe());
    PersistenceService service(registry);

    PersistenceContext context;
    context.rootPath = "worlds/metadata_limit";
    context.preferredFormat = "cr";
    context.storage = storage;

    auto format = Backends::CR::factory()(context);
    std::vector<uint8_t> emptyDocument;
    InMemoryByteWriter emptyWriter(emptyDocument);
    format->worldMetadataCodec().write(
        WorldMetadata{"metadata_limit", ""}, emptyWriter);
    CHECK(emptyDocument.size() < kMaxMetadataDocumentBytes);

    const auto worldPath = CRPaths::worldInfoPath(context);
    WorldSnapshot stringBoundary;
    stringBoundary.metadata.worldId = "metadata_limit";
    stringBoundary.metadata.displayName =
        std::string(kMaxMetadataStringBytes, 'x');
    service.saveWorld(stringBoundary, context);
    CHECK_EQ(service.loadWorldMetadata(context), stringBoundary.metadata);

    const size_t mkdirCount = storage->mkdirCount();
    const size_t writeSessionCount = storage->writeSessionCount();
    const auto stringBoundaryPayload = readAll(*storage, worldPath);
    WorldSnapshot oversizedString = stringBoundary;
    oversizedString.metadata.displayName.push_back('x');
    checkCRMetadataError(
        [&]() { service.saveWorld(oversizedString, context); },
        "CRMetadata: string exceeds format limit");
    CHECK_EQ(storage->mkdirCount(), mkdirCount);
    CHECK_EQ(storage->writeSessionCount(), writeSessionCount);
    CHECK_EQ(readAll(*storage, worldPath), stringBoundaryPayload);

    const size_t encodedValueBytes =
        kMaxMetadataDocumentBytes - emptyDocument.size();
    CHECK(encodedValueBytes >= kMaxMetadataStringBytes);
    const size_t extraEncodedBytes =
        encodedValueBytes - kMaxMetadataStringBytes;
    const size_t unicodeEscapeCount = extraEncodedBytes / 5;
    const size_t quoteEscapeCount = extraEncodedBytes % 5;
    CHECK(unicodeEscapeCount + quoteEscapeCount <= kMaxMetadataStringBytes);

    WorldSnapshot documentBoundary;
    documentBoundary.metadata.worldId = "metadata_limit";
    documentBoundary.metadata.displayName.assign(
        kMaxMetadataStringBytes, 'x');
    std::fill_n(
        documentBoundary.metadata.displayName.begin(),
        unicodeEscapeCount,
        '\x01');
    std::fill_n(
        documentBoundary.metadata.displayName.begin() + unicodeEscapeCount,
        quoteEscapeCount,
        '"');

    service.saveWorld(documentBoundary, context);
    const auto documentBoundaryPayload = readAll(*storage, worldPath);
    CHECK_EQ(documentBoundaryPayload.size(), kMaxMetadataDocumentBytes);
    CHECK_EQ(service.loadWorldMetadata(context), documentBoundary.metadata);

    WorldSnapshot oversizedDocument = documentBoundary;
    oversizedDocument.metadata.displayName.back() = '\x01';
    checkCRMetadataError(
        [&]() { service.saveWorld(oversizedDocument, context); },
        "CRMetadata: document exceeds format limit");
    CHECK_EQ(readAll(*storage, worldPath), documentBoundaryPayload);
}

TEST_CASE(CRBackend_invalid_metadata_prevents_world_save_mutation) {
    const std::vector<std::string> invalidDocuments{
        R"({"defaultZoneId":"rigel:default","worldDisplayName":"bad\q"})",
        R"({"defaultZoneId":"rigel:default","worldDisplayName":"bad\u12xz"})",
        R"({"defaultZoneId":"rigel:default","worldDisplayName":"bad\u12")",
        R"({"defaultZoneId":"rigel:default","worldDisplayName":"bad\ud800"})",
        R"({"defaultZoneId":"rigel:default","worldDisplayName":"bad\udfff"})",
        R"({"defaultZoneId":"rigel:default","extension":[})",
        std::string(kMaxMetadataDocumentBytes + 1, 'x')
    };
    const std::vector<std::string> diagnostics{
        "CRMetadata: invalid JSON string",
        "CRMetadata: invalid JSON string",
        "CRMetadata: invalid JSON string",
        "CRMetadata: invalid JSON string",
        "CRMetadata: invalid JSON string",
        "CRMetadata: invalid JSON document",
        "CRMetadata: document exceeds format limit"
    };

    for (size_t index = 0; index < invalidDocuments.size(); ++index) {
        auto storage = std::make_shared<InMemoryStorageBackend>();
        PersistenceContext context;
        context.rootPath = "worlds/invalid_metadata_" + std::to_string(index);
        context.storage = storage;
        const auto worldPath = CRPaths::worldInfoPath(context);
        const auto zonePath = CRPaths::zoneInfoPath(
            ZoneKey{"rigel:default"}, context);
        const auto regionPath = CRPaths::regionPath(
            RegionKey{"rigel:default", 0, 0, 0}, context);
        writeText(*storage, worldPath, invalidDocuments[index]);
        const auto previousPayload = readAll(*storage, worldPath);
        const size_t mkdirCount = storage->mkdirCount();
        const size_t writeSessionCount = storage->writeSessionCount();

        checkCRMetadataError(
            [&]() { saveCRWorld(storage, context.rootPath, true); },
            diagnostics[index]);

        CHECK_EQ(storage->mkdirCount(), mkdirCount);
        CHECK_EQ(storage->writeSessionCount(), writeSessionCount);
        CHECK_EQ(readAll(*storage, worldPath), previousPayload);
        CHECK(!storage->exists(zonePath));
        CHECK(!storage->exists(regionPath));
    }
}

TEST_CASE(CRBackend_world_save_preserves_existing_metadata_bytes) {
    auto storage = std::make_shared<InMemoryStorageBackend>();
    PersistenceContext context;
    context.rootPath = "worlds/preserved_metadata";
    context.storage = storage;

    const std::string worldInfo = R"({
  "latestRegionFileVersion": 4,
  "defaultZoneId": "rigel:default",
  "worldDisplayName": "Imported World",
  "worldSeed": 782347234,
  "worldCreatedEpochMillis": 123456789,
  "lastPlayedEpochMillis": 987654321,
  "worldTick": 44332211,
  "extension": {"preserve": true}
}
)";
    const std::string zoneInfo = R"({
  "zoneId": "rigel:default",
  "worldGenSaveKey": "base:overworld",
  "seed": 918273645,
  "respawnHeight": -64,
  "spawnPoint": {"x":12.5,"y":94.0,"z":-33.25},
  "skyId": "base:starry_sky",
  "extension": [1, 2, 3]
}
)";
    const auto worldPath = CRPaths::worldInfoPath(context);
    const auto zonePath = CRPaths::zoneInfoPath(ZoneKey{"rigel:default"}, context);
    writeText(*storage, worldPath, worldInfo);
    writeText(*storage, zonePath, zoneInfo);

    saveCRWorld(storage, context.rootPath, true);

    CHECK_EQ(readAll(*storage, worldPath),
             (std::vector<uint8_t>(worldInfo.begin(), worldInfo.end())));
    CHECK_EQ(readAll(*storage, zonePath),
             (std::vector<uint8_t>(zoneInfo.begin(), zoneInfo.end())));
    CHECK(storage->exists(CRPaths::regionPath(
        RegionKey{"rigel:default", 0, 0, 0}, context)));
}

TEST_CASE(CRBackend_world_save_initializes_missing_metadata) {
    auto storage = std::make_shared<InMemoryStorageBackend>();
    PersistenceContext context;
    context.rootPath = "worlds/fresh_metadata";
    context.storage = storage;
    const auto worldPath = CRPaths::worldInfoPath(context);
    const auto zonePath = CRPaths::zoneInfoPath(ZoneKey{"rigel:default"}, context);

    saveCRWorld(storage, context.rootPath);

    CHECK(storage->exists(worldPath));
    CHECK(storage->exists(zonePath));
}

TEST_CASE(CRBackend_world_save_preserves_world_only_metadata) {
    auto storage = std::make_shared<InMemoryStorageBackend>();
    PersistenceContext context;
    context.rootPath = "worlds/world_only_metadata";
    context.storage = storage;
    const auto worldPath = CRPaths::worldInfoPath(context);
    const auto zonePath = CRPaths::zoneInfoPath(ZoneKey{"rigel:default"}, context);
    const std::string worldInfo = R"({
  "defaultZoneId": "rigel:default",
  "worldDisplayName": "Existing World",
  "worldSeed": 271828182,
  "worldTick": 314159265
}
)";
    writeText(*storage, worldPath, worldInfo);

    saveCRWorld(storage, context.rootPath);

    CHECK_EQ(readAll(*storage, worldPath),
             (std::vector<uint8_t>(worldInfo.begin(), worldInfo.end())));
    CHECK(storage->exists(zonePath));
}

TEST_CASE(CRBackend_world_save_preserves_zone_only_metadata) {
    auto storage = std::make_shared<InMemoryStorageBackend>();
    PersistenceContext context;
    context.rootPath = "worlds/zone_only_metadata";
    context.storage = storage;
    const auto worldPath = CRPaths::worldInfoPath(context);
    const auto zonePath = CRPaths::zoneInfoPath(ZoneKey{"rigel:default"}, context);
    const std::string zoneInfo = R"({
  "zoneId": "rigel:default",
  "worldGenSaveKey": "base:custom_generator",
  "seed": 161803398,
  "respawnHeight": 72,
  "spawnPoint": {"x":4,"y":80,"z":9},
  "skyId": "base:day_sky"
}
)";
    writeText(*storage, zonePath, zoneInfo);

    saveCRWorld(storage, context.rootPath);

    CHECK(storage->exists(worldPath));
    CHECK_EQ(readAll(*storage, zonePath),
             (std::vector<uint8_t>(zoneInfo.begin(), zoneInfo.end())));
}

TEST_CASE(CRBackend_world_save_rejects_alternate_default_zone) {
    auto storage = std::make_shared<InMemoryStorageBackend>();
    PersistenceContext context;
    context.rootPath = "worlds/alternate_default";
    context.storage = storage;
    const auto worldPath = CRPaths::worldInfoPath(context);
    const auto rigelZonePath =
        CRPaths::zoneInfoPath(ZoneKey{"rigel:default"}, context);
    const auto rigelRegionPath = CRPaths::regionPath(
        RegionKey{"rigel:default", 0, 0, 0}, context);
    const std::string worldInfo = R"({
  "defaultZoneId": "base:moon",
  "worldDisplayName": "Moon World",
  "worldSeed": 42424242
}
)";
    writeText(*storage, worldPath, worldInfo);

    std::string diagnostic;
    try {
        saveCRWorld(storage, context.rootPath, true);
    } catch (const std::exception& error) {
        diagnostic = error.what();
    }

    CHECK(diagnostic.find("base:moon") != std::string::npos);
    CHECK(diagnostic.find("rigel:default") != std::string::npos);
    CHECK_EQ(readAll(*storage, worldPath),
             (std::vector<uint8_t>(worldInfo.begin(), worldInfo.end())));
    CHECK(!storage->exists(rigelZonePath));
    CHECK(!storage->exists(rigelRegionPath));
}

TEST_CASE(CRBackend_async_loader_rejects_alternate_default_zone) {
    auto storage = std::make_shared<InMemoryStorageBackend>();
    Rigel::Voxel::WorldResources resources;
    Rigel::Voxel::World world(resources);

    FormatRegistry registry;
    registry.registerFormat(
        Backends::CR::descriptor(), Backends::CR::factory(), Backends::CR::probe());
    PersistenceService service(registry);

    PersistenceContext context;
    context.rootPath = "worlds/alternate_async_default";
    context.preferredFormat = "cr";
    context.storage = storage;
    context.providers = world.persistenceProvidersHandle();
    writeText(*storage, CRPaths::worldInfoPath(context), R"({
  "defaultZoneId": "base:moon"
}
)");
    std::string diagnostic;
    try {
        AsyncChunkLoader loader(
            service, context, world, 0, 0, 0, 4, nullptr);
    } catch (const std::exception& error) {
        diagnostic = error.what();
    }

    CHECK(diagnostic.find("base:moon") != std::string::npos);
    CHECK(diagnostic.find("rigel:default") != std::string::npos);
}

TEST_CASE(CRBackend_metadata_saves_preserve_existing_payloads) {
    auto storage = std::make_shared<InMemoryStorageBackend>();
    FormatRegistry registry;
    registry.registerFormat(Backends::CR::descriptor(), Backends::CR::factory(), Backends::CR::probe());
    PersistenceService service(registry);

    PersistenceContext context;
    context.rootPath = "worlds/demo";
    context.preferredFormat = "cr";
    context.storage = storage;

    ChunkSnapshot chunk;
    chunk.key = ChunkKey{"zone:default", 1, 0, 0};
    chunk.data = makeMinimalChunkData(chunk.key);

    ChunkRegionSnapshot chunkRegion;
    chunkRegion.key = RegionKey{"zone:default", 0, 0, 0};
    chunkRegion.chunks.push_back(chunk);
    service.saveRegion(chunkRegion, context);

    EntityPersistedEntity entity;
    entity.typeId = "rigel:test_entity";
    entity.id.time = 1;
    entity.id.random = 2;
    entity.id.counter = 3;
    entity.position = glm::vec3(1.0f, 2.0f, 3.0f);

    EntityPersistedChunk entityChunk;
    entityChunk.coord = Rigel::Voxel::ChunkCoord{1, 0, 0};
    entityChunk.entities.push_back(entity);

    EntityRegionSnapshot entityRegion;
    entityRegion.key = EntityRegionKey{"zone:default", 0, 0, 0};
    entityRegion.chunks.push_back(entityChunk);
    service.saveEntities(entityRegion, context);

    const auto chunkPath = CRPaths::regionPath(chunkRegion.key, context);
    const auto entityPath = CRPaths::entityRegionPath(entityRegion.key, context);
    const auto chunkPayload = readAll(*storage, chunkPath);
    const auto entityPayload = readAll(*storage, entityPath);

    ZoneMetadata zone{"zone:default", "Default Zone"};
    service.saveZoneMetadata(zone, context);

    CHECK_EQ(readAll(*storage, chunkPath), chunkPayload);
    CHECK_EQ(readAll(*storage, entityPath), entityPayload);

    WorldSnapshot world;
    world.metadata = WorldMetadata{"demo", "Demo World"};
    world.zones.push_back(zone);
    service.saveWorld(world, context);

    CHECK_EQ(readAll(*storage, chunkPath), chunkPayload);
    CHECK_EQ(readAll(*storage, entityPath), entityPayload);
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

TEST_CASE(CRBackend_filesystem_region_roundtrip) {
    Rigel::Test::TemporaryDirectory directory("rigel_cr_backend");

    auto storage = std::make_shared<FilesystemBackend>();
    FormatRegistry registry;
    registry.registerFormat(Backends::CR::descriptor(), Backends::CR::factory(), Backends::CR::probe());
    PersistenceService service(registry);

    PersistenceContext context;
    context.rootPath = directory.path().string();
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
