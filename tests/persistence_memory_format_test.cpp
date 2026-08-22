#include "TestFramework.h"

#include "Rigel/Persistence/Backends/Memory/MemoryFormat.h"
#include "Rigel/Persistence/Format.h"
#include "Rigel/Persistence/Storage.h"
#include "Rigel/Persistence/ChunkSerializer.h"
#include "Rigel/Voxel/BlockRegistry.h"
#include "Rigel/Voxel/BlockType.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace Rigel::Persistence;

namespace {

constexpr size_t kMaxMemoryStringBytes = 1'048'576;
constexpr size_t kMaxMemoryRegionChunks = 16 * 16 * 16;

void appendU8(std::vector<uint8_t>& bytes, uint8_t value) {
    bytes.push_back(value);
}

void appendU16(std::vector<uint8_t>& bytes, uint16_t value) {
    appendU8(bytes, static_cast<uint8_t>((value >> 8) & 0xFF));
    appendU8(bytes, static_cast<uint8_t>(value & 0xFF));
}

void appendU32(std::vector<uint8_t>& bytes, uint32_t value) {
    appendU8(bytes, static_cast<uint8_t>((value >> 24) & 0xFF));
    appendU8(bytes, static_cast<uint8_t>((value >> 16) & 0xFF));
    appendU8(bytes, static_cast<uint8_t>((value >> 8) & 0xFF));
    appendU8(bytes, static_cast<uint8_t>(value & 0xFF));
}

void appendI32(std::vector<uint8_t>& bytes, int32_t value) {
    appendU32(bytes, static_cast<uint32_t>(value));
}

void appendSpan(std::vector<uint8_t>& bytes, const ChunkSpan& span) {
    appendI32(bytes, span.chunkX);
    appendI32(bytes, span.chunkY);
    appendI32(bytes, span.chunkZ);
    appendI32(bytes, span.offsetX);
    appendI32(bytes, span.offsetY);
    appendI32(bytes, span.offsetZ);
    appendI32(bytes, span.sizeX);
    appendI32(bytes, span.sizeY);
    appendI32(bytes, span.sizeZ);
}

void appendBlock(std::vector<uint8_t>& bytes,
                 uint16_t id = 0,
                 uint8_t metadata = 0,
                 uint8_t lightLevel = 0) {
    appendU16(bytes, id);
    appendU8(bytes, metadata);
    appendU8(bytes, lightLevel);
}

std::vector<uint8_t> chunkFixture(const ChunkSpan& span, uint32_t blockCount) {
    std::vector<uint8_t> bytes;
    appendI32(bytes, span.chunkX);
    appendI32(bytes, span.chunkY);
    appendI32(bytes, span.chunkZ);
    appendSpan(bytes, span);
    appendU32(bytes, blockCount);
    return bytes;
}

template <typename Fn>
void checkFormatError(Fn&& fn, const std::string& expected) {
    try {
        fn();
    } catch (const std::runtime_error& error) {
        CHECK_EQ(std::string(error.what()), expected);
        return;
    }
    throw Rigel::Test::TestFailure("Expected format error");
}

class MemoryFixture {
public:
    MemoryFixture()
        : m_directory("rigel_memory_format"),
          m_storage(std::make_shared<FilesystemBackend>()) {
        m_context.rootPath = m_directory.path().string();
        m_context.preferredFormat = "memory";
        m_context.storage = m_storage;
        m_format = Backends::Memory::factory()(m_context);
    }

    ChunkSnapshot loadChunk(std::vector<uint8_t> bytes,
                            bool satisfyRegionMinimum = false) {
        if (satisfyRegionMinimum) {
            appendBlock(bytes);
        }
        std::vector<uint8_t> regionBytes;
        appendU32(regionBytes, 1);
        regionBytes.insert(regionBytes.end(), bytes.begin(), bytes.end());
        return loadRegion(std::move(regionBytes)).chunks.front();
    }

    WorldMetadata loadWorldMetadata(std::vector<uint8_t> bytes) {
        const std::string path = m_format->worldMetadataCodec().metadataPath(m_context);
        write(path, std::move(bytes));
        auto reader = m_storage->openRead(path);
        return m_format->worldMetadataCodec().read(*reader);
    }

    ChunkRegionSnapshot loadRegion(std::vector<uint8_t> bytes) {
        const RegionKey key{"zone", 0, 0, 0};
        write(regionPath(key), std::move(bytes));
        return m_format->chunkContainer().loadRegion(key);
    }

    EntityRegionSnapshot loadEntityRegion(std::vector<uint8_t> bytes) {
        const EntityRegionKey key{"zone", 0, 0, 0};
        write(entityRegionPath(key), std::move(bytes));
        return m_format->entityContainer().loadRegion(key);
    }

    void saveWorldMetadata(const WorldMetadata& metadata) {
        const std::string path =
            m_format->worldMetadataCodec().metadataPath(m_context);
        auto session = m_storage->openWrite(path, AtomicWriteOptions{});
        m_format->worldMetadataCodec().write(metadata, session->writer());
        session->writer().flush();
        session->commit();
    }

    WorldMetadata loadSavedWorldMetadata() {
        const std::string path =
            m_format->worldMetadataCodec().metadataPath(m_context);
        auto reader = m_storage->openRead(path);
        return m_format->worldMetadataCodec().read(*reader);
    }

    void saveSingleChunkRegion(const ChunkSnapshot& chunk) {
        m_format->chunkContainer().saveRegion(ChunkRegionSnapshot{
            RegionKey{chunk.key.zoneId, 0, 0, 0}, {chunk}});
    }

    ChunkSnapshot loadSavedSingleChunkRegion(const ChunkKey& key) {
        return m_format->chunkContainer()
            .loadRegion(RegionKey{key.zoneId, 0, 0, 0})
            .chunks.front();
    }

    void saveRegion(const ChunkRegionSnapshot& region) {
        m_format->chunkContainer().saveRegion(region);
    }

    ChunkRegionSnapshot loadSavedRegion(const RegionKey& key) {
        return m_format->chunkContainer().loadRegion(key);
    }

    void saveEntityRegion(const EntityRegionSnapshot& region) {
        m_format->entityContainer().saveRegion(region);
    }

    EntityRegionSnapshot loadSavedEntityRegion(const EntityRegionKey& key) {
        return m_format->entityContainer().loadRegion(key);
    }

private:
    std::string regionPath(const RegionKey& key) const {
        return m_context.rootPath + "/zones/" + key.zoneId +
            "/regions/region_" + std::to_string(key.x) + "_" +
            std::to_string(key.y) + "_" + std::to_string(key.z) + ".mem";
    }

    std::string entityRegionPath(const EntityRegionKey& key) const {
        return m_context.rootPath + "/zones/" + key.zoneId +
            "/entities/entityRegion_" + std::to_string(key.x) + "_" +
            std::to_string(key.y) + "_" + std::to_string(key.z) + ".mem";
    }

    void write(const std::string& path, std::vector<uint8_t> bytes) {
        auto session = m_storage->openWrite(path, AtomicWriteOptions{});
        if (!bytes.empty()) {
            session->writer().writeBytes(bytes.data(), bytes.size());
        }
        session->commit();
    }

    Rigel::Test::TemporaryDirectory m_directory;
    std::shared_ptr<FilesystemBackend> m_storage;
    PersistenceContext m_context;
    std::unique_ptr<PersistenceFormat> m_format;
};

} // namespace

TEST_CASE(MemoryFormat_WriterEnforcesStringBoundaryBeforeCommit) {
    MemoryFixture fixture;
    WorldMetadata boundary{
        "world",
        std::string(kMaxMemoryStringBytes, 'x')
    };
    fixture.saveWorldMetadata(boundary);
    CHECK(fixture.loadSavedWorldMetadata() == boundary);

    WorldMetadata oversized = boundary;
    oversized.displayName.push_back('x');
    checkFormatError(
        [&]() { fixture.saveWorldMetadata(oversized); },
        "MemoryFormat: string length exceeds format limit");
    CHECK(fixture.loadSavedWorldMetadata() == boundary);
}

TEST_CASE(MemoryFormat_WriterEnforcesRegionCollectionBoundaryBeforeCommit) {
    MemoryFixture fixture;
    ChunkSnapshot chunk;
    chunk.key = ChunkKey{"zone", 0, 0, 0};
    chunk.data.span.sizeX = 1;
    chunk.data.span.sizeY = 1;
    chunk.data.span.sizeZ = 1;
    chunk.data.blocks.emplace_back();

    ChunkRegionSnapshot boundary;
    boundary.key = RegionKey{"zone", 0, 0, 0};
    boundary.chunks.resize(kMaxMemoryRegionChunks, chunk);
    fixture.saveRegion(boundary);
    CHECK(fixture.loadSavedRegion(boundary.key) == boundary);

    ChunkRegionSnapshot oversized = boundary;
    oversized.chunks.push_back(chunk);
    checkFormatError(
        [&]() { fixture.saveRegion(oversized); },
        "MemoryFormat: chunk count exceeds format limit");
    CHECK(fixture.loadSavedRegion(boundary.key) == boundary);
}

TEST_CASE(MemoryFormat_WriterEnforcesEntityCollectionBoundaryBeforeCommit) {
    MemoryFixture fixture;
    EntityPersistedChunk chunk;
    EntityRegionSnapshot boundary;
    boundary.key = EntityRegionKey{"zone", 0, 0, 0};
    boundary.chunks.resize(kMaxMemoryRegionChunks, chunk);
    fixture.saveEntityRegion(boundary);
    CHECK(fixture.loadSavedEntityRegion(boundary.key) == boundary);

    EntityRegionSnapshot oversized = boundary;
    oversized.chunks.push_back(chunk);
    checkFormatError(
        [&]() { fixture.saveEntityRegion(oversized); },
        "MemoryFormat: entity-region chunk count exceeds format limit");
    CHECK(fixture.loadSavedEntityRegion(boundary.key) == boundary);
}

TEST_CASE(MemoryFormat_WriterRejectsUnreadableChunkBeforeCommit) {
    MemoryFixture fixture;
    ChunkSnapshot valid;
    valid.key = ChunkKey{"zone", 1, 2, 3};
    valid.data.span.chunkX = 1;
    valid.data.span.chunkY = 2;
    valid.data.span.chunkZ = 3;
    valid.data.span.sizeX = 1;
    valid.data.span.sizeY = 1;
    valid.data.span.sizeZ = 1;
    valid.data.blocks.emplace_back();
    fixture.saveSingleChunkRegion(valid);

    ChunkSnapshot invalid = valid;
    invalid.data.blocks.clear();
    checkFormatError(
        [&]() { fixture.saveSingleChunkRegion(invalid); },
        "ChunkSerializer: block data size mismatch");
    CHECK(fixture.loadSavedSingleChunkRegion(valid.key) == valid);
}

TEST_CASE(MemoryFormat_RejectsUnboundedStringLengths) {
    MemoryFixture fixture;
    for (const uint32_t length : {
             std::numeric_limits<uint32_t>::max(),
             static_cast<uint32_t>(std::numeric_limits<int32_t>::max())}) {
        std::vector<uint8_t> bytes;
        appendU32(bytes, length);
        checkFormatError(
            [&]() { fixture.loadWorldMetadata(bytes); },
            "MemoryFormat: string length exceeds format limit");
    }

    std::vector<uint8_t> truncated;
    appendU32(truncated, 4);
    appendU8(truncated, 'x');
    checkFormatError(
        [&]() { fixture.loadWorldMetadata(truncated); },
        "MemoryFormat: string length exceeds remaining input");
}

TEST_CASE(MemoryFormat_RejectsUnboundedRegionChunkCounts) {
    MemoryFixture fixture;
    std::vector<uint8_t> unbounded;
    appendU32(unbounded, std::numeric_limits<uint32_t>::max());
    checkFormatError(
        [&]() { fixture.loadRegion(unbounded); },
        "MemoryFormat: chunk count exceeds format limit");

    std::vector<uint8_t> truncated;
    appendU32(truncated, 1);
    checkFormatError(
        [&]() { fixture.loadRegion(truncated); },
        "MemoryFormat: chunk count exceeds remaining input");
}

TEST_CASE(MemoryFormat_RejectsUnboundedEntityCounts) {
    MemoryFixture fixture;
    std::vector<uint8_t> unboundedChunks;
    appendU32(unboundedChunks, std::numeric_limits<uint32_t>::max());
    checkFormatError(
        [&]() { fixture.loadEntityRegion(unboundedChunks); },
        "MemoryFormat: entity-region chunk count exceeds format limit");

    std::vector<uint8_t> truncatedChunks;
    appendU32(truncatedChunks, 1);
    checkFormatError(
        [&]() { fixture.loadEntityRegion(truncatedChunks); },
        "MemoryFormat: entity-region chunk count exceeds remaining input");

    std::vector<uint8_t> unboundedEntities;
    appendU32(unboundedEntities, 1);
    appendI32(unboundedEntities, 0);
    appendI32(unboundedEntities, 0);
    appendI32(unboundedEntities, 0);
    appendU32(unboundedEntities, std::numeric_limits<uint32_t>::max());
    checkFormatError(
        [&]() { fixture.loadEntityRegion(unboundedEntities); },
        "MemoryFormat: entity count exceeds format limit");

    std::vector<uint8_t> truncatedEntities;
    appendU32(truncatedEntities, 1);
    appendI32(truncatedEntities, 0);
    appendI32(truncatedEntities, 0);
    appendI32(truncatedEntities, 0);
    appendU32(truncatedEntities, 1);
    checkFormatError(
        [&]() { fixture.loadEntityRegion(truncatedEntities); },
        "MemoryFormat: entity count exceeds remaining input");
}

TEST_CASE(MemoryFormat_RejectsInvalidChunkSpans) {
    MemoryFixture fixture;

    ChunkSpan negative;
    negative.sizeX = -1;
    negative.sizeY = 1;
    negative.sizeZ = 1;
    checkFormatError(
        [&]() { fixture.loadChunk(chunkFixture(negative, 0), true); },
        "ChunkSerializer: span size must be positive");

    ChunkSpan oversized;
    oversized.sizeX = std::numeric_limits<int32_t>::max();
    oversized.sizeY = 1;
    oversized.sizeZ = 1;
    checkFormatError(
        [&]() { fixture.loadChunk(chunkFixture(oversized, 0), true); },
        "ChunkSerializer: span out of chunk bounds");

    ChunkSpan overflowing;
    overflowing.offsetX = std::numeric_limits<int32_t>::max();
    overflowing.sizeX = std::numeric_limits<int32_t>::max();
    overflowing.sizeY = 1;
    overflowing.sizeZ = 1;
    checkFormatError(
        [&]() { fixture.loadChunk(chunkFixture(overflowing, 0), true); },
        "ChunkSerializer: span out of chunk bounds");
}

TEST_CASE(MemoryFormat_RejectsMismatchedBlockCount) {
    MemoryFixture fixture;
    ChunkSpan span;
    span.sizeX = 2;
    span.sizeY = 1;
    span.sizeZ = 1;
    auto bytes = chunkFixture(span, 1);
    appendBlock(bytes);

    checkFormatError(
        [&]() { fixture.loadChunk(bytes); },
        "ChunkSerializer: block data size mismatch");

    ChunkSpan single;
    single.sizeX = 1;
    single.sizeY = 1;
    single.sizeZ = 1;
    checkFormatError(
        [&]() {
            fixture.loadChunk(
                chunkFixture(single, std::numeric_limits<uint32_t>::max()),
                true);
        },
        "ChunkSerializer: block data size mismatch");

    ChunkSpan truncated = single;
    truncated.sizeX = 2;
    auto truncatedBytes = chunkFixture(truncated, 2);
    appendBlock(truncatedBytes);
    checkFormatError(
        [&]() { fixture.loadChunk(truncatedBytes); },
        "MemoryFormat: chunk block count exceeds remaining input");
}

TEST_CASE(MemoryFormat_InvalidNumericBlockIdDoesNotMutateChunk) {
    Rigel::Voxel::BlockRegistry registry;
    Rigel::Voxel::BlockType originalType;
    originalType.identifier = "test:original";
    originalType.isOpaque = true;
    const auto originalId = registry.registerBlock(
        "test:original", std::move(originalType));
    Rigel::Voxel::BlockType replacementType;
    replacementType.identifier = "test:replacement";
    replacementType.isOpaque = true;
    const auto replacementId = registry.registerBlock(
        "test:replacement", std::move(replacementType));

    ChunkSpan span;
    span.sizeX = 2;
    span.sizeY = 1;
    span.sizeZ = 1;
    auto bytes = chunkFixture(span, 2);
    appendBlock(bytes, replacementId.type);
    appendBlock(bytes, std::numeric_limits<uint16_t>::max());
    MemoryFixture fixture;
    const ChunkSnapshot snapshot = fixture.loadChunk(std::move(bytes));

    Rigel::Voxel::Chunk chunk({0, 0, 0});
    Rigel::Voxel::BlockState original;
    original.id = originalId;
    chunk.fill(original, registry);
    chunk.clearDirty();
    chunk.clearPersistDirty();

    checkFormatError(
        [&]() { applyChunkData(snapshot.data, chunk, registry); },
        "ChunkSerializer: invalid block ID 65535");
    CHECK_EQ(chunk.getBlock(0, 0, 0).id, originalId);
    CHECK_EQ(chunk.getBlock(1, 0, 0).id, originalId);
    CHECK(!chunk.isDirty());
    CHECK(!chunk.isPersistDirty());
}
