#include "Rigel/Persistence/Backends/CR/CRFormat.h"

#include "Rigel/Persistence/Backends/CR/CRPaths.h"
#include "Rigel/Persistence/Backends/CR/CRSettings.h"
#include "Rigel/Persistence/Backends/CR/CRLz4.h"
#include "Rigel/Persistence/Storage.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Rigel::Persistence::Backends::CR {

namespace {

constexpr int32_t kMagic = 0xFFECCEAC;
constexpr int32_t kFileVersion = 4;
constexpr int32_t kCompressionNone = 0;
constexpr int32_t kCompressionLz4 = 1;

constexpr int32_t kBlockNull = 0;
constexpr int32_t kBlockSingle = 1;
constexpr int32_t kBlockLayered = 2;

constexpr int32_t kBlockLayerSingleByte = 1;
constexpr int32_t kBlockLayerSingleInt = 2;
constexpr int32_t kBlockLayerHalfNibble = 3;
constexpr int32_t kBlockLayerNibble = 4;
constexpr int32_t kBlockLayerByte = 5;
constexpr int32_t kBlockLayerShort = 6;
constexpr int32_t kBlockLayerBit = 7;

constexpr int32_t kSkyNull = 1;
constexpr int32_t kSkyLayered = 2;
constexpr int32_t kSkySingle = 3;

constexpr int32_t kSkyLayerSingle = 1;
constexpr int32_t kSkyLayerNibble = 2;

constexpr int32_t kBlockLightNull = 1;
constexpr int32_t kBlockLightLayered = 2;

constexpr int32_t kBlockLightLayerSingle = 1;
constexpr int32_t kBlockLightLayerShort = 2;
constexpr int32_t kBlockLightLayerMonoRed = 3;
constexpr int32_t kBlockLightLayerMonoGreen = 4;
constexpr int32_t kBlockLightLayerMonoBlue = 5;

constexpr int32_t kBlockEntityNull = 0;
constexpr int32_t kBlockEntityData = 1;

constexpr int32_t kLayerBlocks = 16 * 16;
constexpr size_t kLayerBytesBit = kLayerBlocks / 8;
constexpr size_t kLayerBytesHalfNibble = kLayerBlocks / 4;
constexpr size_t kLayerBytesNibble = kLayerBlocks / 2;
constexpr size_t kLayerBytesByte = kLayerBlocks;
constexpr size_t kLayerBytesShort = kLayerBlocks * 2;

std::string parentPath(const std::string& path) {
    auto pos = path.find_last_of('/');
    if (pos == std::string::npos) {
        return std::string();
    }
    return path.substr(0, pos);
}

std::string basename(const std::string& path) {
    if (path.empty()) {
        return path;
    }
    std::string trimmed = path;
    while (!trimmed.empty() && trimmed.back() == '/') {
        trimmed.pop_back();
    }
    auto pos = trimmed.find_last_of('/');
    if (pos == std::string::npos) {
        return trimmed;
    }
    return trimmed.substr(pos + 1);
}

std::array<uint8_t, 4> encodeI32(int32_t value) {
    return {
        static_cast<uint8_t>((value >> 24) & 0xFF),
        static_cast<uint8_t>((value >> 16) & 0xFF),
        static_cast<uint8_t>((value >> 8) & 0xFF),
        static_cast<uint8_t>(value & 0xFF)
    };
}

std::optional<std::string> extractJsonString(const std::string& text, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    auto pos = text.find(needle);
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    pos = text.find(':', pos);
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    pos = text.find('"', pos);
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    auto end = text.find('"', pos + 1);
    if (end == std::string::npos || end <= pos + 1) {
        return std::nullopt;
    }
    return text.substr(pos + 1, end - pos - 1);
}

class TrackingReader {
public:
    explicit TrackingReader(ByteReader& reader)
        : m_reader(reader) {
    }

    uint8_t readU8() {
        uint8_t value = m_reader.readU8();
        m_bytes.push_back(value);
        return value;
    }

    uint16_t readU16() {
        uint16_t value = 0;
        value |= static_cast<uint16_t>(readU8()) << 8;
        value |= static_cast<uint16_t>(readU8());
        return value;
    }

    uint32_t readU32() {
        uint32_t value = 0;
        value |= static_cast<uint32_t>(readU8()) << 24;
        value |= static_cast<uint32_t>(readU8()) << 16;
        value |= static_cast<uint32_t>(readU8()) << 8;
        value |= static_cast<uint32_t>(readU8());
        return value;
    }

    int32_t readI32() {
        return static_cast<int32_t>(readU32());
    }

    void readBytes(size_t len) {
        if (len == 0) {
            return;
        }
        std::vector<uint8_t> buffer(len);
        m_reader.readBytes(buffer.data(), len);
        m_bytes.insert(m_bytes.end(), buffer.begin(), buffer.end());
    }

    std::vector<uint8_t> takeBytes() {
        return std::move(m_bytes);
    }

private:
    ByteReader& m_reader;
    std::vector<uint8_t> m_bytes;
};

class MemoryByteReader final : public ByteReader {
public:
    explicit MemoryByteReader(std::vector<uint8_t> data)
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
            throw std::runtime_error("CRMemoryReader seek out of range");
        }
        m_pos = offset;
    }

    std::vector<uint8_t> readAt(size_t offset, size_t len) override {
        if (offset + len > m_data.size()) {
            throw std::runtime_error("CRMemoryReader readAt out of range");
        }
        return std::vector<uint8_t>(m_data.begin() + offset, m_data.begin() + offset + len);
    }

private:
    void ensureAvailable(size_t len) {
        if (m_pos + len > m_data.size()) {
            throw std::runtime_error("CRMemoryReader read out of range");
        }
    }

    std::vector<uint8_t> m_data;
    size_t m_pos = 0;
};

class VectorWriter final : public ByteWriter {
public:
    explicit VectorWriter(std::vector<uint8_t>& target)
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

std::string readString(TrackingReader& reader) {
    int32_t len = reader.readI32();
    if (len <= 0) {
        return std::string();
    }
    reader.readBytes(static_cast<size_t>(len));
    return std::string();
}

void writeString(ByteWriter& writer, const std::string& value) {
    writer.writeI32(static_cast<int32_t>(value.size()));
    if (!value.empty()) {
        writer.writeBytes(reinterpret_cast<const uint8_t*>(value.data()), value.size());
    }
}

void readBlockLayerPayload(TrackingReader& reader, uint8_t layerType) {
    switch (layerType) {
    case kBlockLayerSingleByte:
        reader.readU8();
        break;
    case kBlockLayerSingleInt:
        reader.readI32();
        break;
    case kBlockLayerHalfNibble:
        reader.readBytes(kLayerBytesHalfNibble);
        break;
    case kBlockLayerNibble:
        reader.readBytes(kLayerBytesNibble);
        break;
    case kBlockLayerByte:
        reader.readBytes(kLayerBytesByte);
        break;
    case kBlockLayerShort:
        reader.readBytes(kLayerBytesShort);
        break;
    case kBlockLayerBit:
        reader.readBytes(kLayerBytesBit);
        break;
    default:
        throw std::runtime_error("CRChunkCodec: unknown block layer type");
    }
}

void readBlockData(TrackingReader& reader) {
    uint8_t type = reader.readU8();
    switch (type) {
    case kBlockNull:
        return;
    case kBlockSingle:
        readString(reader);
        return;
    case kBlockLayered: {
        int32_t paletteSize = reader.readI32();
        for (int32_t i = 0; i < paletteSize; ++i) {
            readString(reader);
        }
        for (int layer = 0; layer < 16; ++layer) {
            uint8_t layerType = reader.readU8();
            readBlockLayerPayload(reader, layerType);
        }
        return;
    }
    default:
        throw std::runtime_error("CRChunkCodec: unknown block data type");
    }
}

void readSkylightData(TrackingReader& reader) {
    uint8_t type = reader.readU8();
    switch (type) {
    case kSkyNull:
        return;
    case kSkySingle:
        reader.readU8();
        return;
    case kSkyLayered:
        for (int layer = 0; layer < 16; ++layer) {
            uint8_t layerType = reader.readU8();
            if (layerType == kSkyLayerSingle) {
                reader.readU8();
            } else if (layerType == kSkyLayerNibble) {
                reader.readBytes(kLayerBytesNibble);
            } else {
                throw std::runtime_error("CRChunkCodec: unknown skylight layer type");
            }
        }
        return;
    default:
        throw std::runtime_error("CRChunkCodec: unknown skylight type");
    }
}

void readBlockLightData(TrackingReader& reader) {
    uint8_t type = reader.readU8();
    switch (type) {
    case kBlockLightNull:
        return;
    case kBlockLightLayered:
        for (int layer = 0; layer < 16; ++layer) {
            uint8_t layerType = reader.readU8();
            switch (layerType) {
            case kBlockLightLayerSingle:
                reader.readBytes(3);
                break;
            case kBlockLightLayerShort:
                reader.readBytes(kLayerBytesShort);
                break;
            case kBlockLightLayerMonoRed:
            case kBlockLightLayerMonoGreen:
            case kBlockLightLayerMonoBlue:
                reader.readBytes(3);
                reader.readBytes(kLayerBytesNibble);
                break;
            default:
                throw std::runtime_error("CRChunkCodec: unknown blocklight layer type");
            }
        }
        return;
    default:
        throw std::runtime_error("CRChunkCodec: unknown blocklight type");
    }
}

class CRChunkCodec {
public:
    ChunkSnapshot read(ByteReader& reader, const ChunkKey& keyHint) {
        TrackingReader tracker(reader);
        ChunkSnapshot out;
        out.key = keyHint;
        out.key.x = tracker.readI32();
        out.key.y = tracker.readI32();
        out.key.z = tracker.readI32();
        readBlockData(tracker);
        readSkylightData(tracker);
        readBlockLightData(tracker);
        uint8_t entityFlag = tracker.readU8();
        if (entityFlag == kBlockEntityData) {
            int32_t size = tracker.readI32();
            if (size > 0) {
                tracker.readBytes(static_cast<size_t>(size));
            }
        } else if (entityFlag != kBlockEntityNull) {
            throw std::runtime_error("CRChunkCodec: unknown block entity flag");
        }
        out.payload = tracker.takeBytes();
        return out;
    }

    void write(const ChunkSnapshot& chunk, ByteWriter& writer) {
        if (!chunk.payload.empty()) {
            writer.writeBytes(chunk.payload.data(), chunk.payload.size());
            return;
        }
        writer.writeI32(chunk.key.x);
        writer.writeI32(chunk.key.y);
        writer.writeI32(chunk.key.z);
        writer.writeU8(static_cast<uint8_t>(kBlockNull));
        writer.writeU8(static_cast<uint8_t>(kSkyNull));
        writer.writeU8(static_cast<uint8_t>(kBlockLightNull));
        writer.writeU8(static_cast<uint8_t>(kBlockEntityNull));
    }
};

class CRWorldMetadataCodec final : public WorldMetadataCodec {
public:
    std::string metadataPath(const PersistenceContext& context) const override {
        return CRPaths::worldInfoPath(context);
    }

    void write(const WorldMetadata& metadata, ByteWriter& writer) override {
        std::string text = "{\n";
        text += "  \"latestRegionFileVersion\": " + std::to_string(kFileVersion) + ",\n";
        text += "  \"defaultZoneId\": \"rigel:default\",\n";
        text += "  \"worldDisplayName\": \"" + metadata.displayName + "\",\n";
        text += "  \"worldSeed\": 0,\n";
        text += "  \"worldCreatedEpochMillis\": 0,\n";
        text += "  \"lastPlayedEpochMillis\": 0,\n";
        text += "  \"worldTick\": 0\n";
        text += "}\n";
        writer.writeBytes(reinterpret_cast<const uint8_t*>(text.data()), text.size());
    }

    WorldMetadata read(ByteReader& reader) override {
        std::vector<uint8_t> bytes(reader.size());
        reader.seek(0);
        if (!bytes.empty()) {
            reader.readBytes(bytes.data(), bytes.size());
        }
        std::string text(bytes.begin(), bytes.end());
        WorldMetadata out;
        out.worldId = basename(m_context.rootPath);
        auto displayName = extractJsonString(text, "worldDisplayName");
        if (displayName) {
            out.displayName = *displayName;
        } else {
            out.displayName = out.worldId;
        }
        return out;
    }

    void setContext(PersistenceContext context) {
        m_context = std::move(context);
    }

private:
    PersistenceContext m_context;
};

class CRZoneMetadataCodec final : public ZoneMetadataCodec {
public:
    std::string metadataPath(const ZoneKey& key, const PersistenceContext& context) const override {
        return CRPaths::zoneInfoPath(key, context);
    }

    void write(const ZoneMetadata& metadata, ByteWriter& writer) override {
        std::string text = "{\n";
        text += "  \"zoneId\": \"" + metadata.zoneId + "\",\n";
        text += "  \"worldGenSaveKey\": \"rigel:default\",\n";
        text += "  \"seed\": 0,\n";
        text += "  \"respawnHeight\": 0,\n";
        text += "  \"spawnPoint\": {\"x\":0,\"y\":0,\"z\":0},\n";
        text += "  \"skyId\": \"rigel:default\"\n";
        text += "}\n";
        writer.writeBytes(reinterpret_cast<const uint8_t*>(text.data()), text.size());
    }

    ZoneMetadata read(ByteReader& reader) override {
        std::vector<uint8_t> bytes(reader.size());
        reader.seek(0);
        if (!bytes.empty()) {
            reader.readBytes(bytes.data(), bytes.size());
        }
        std::string text(bytes.begin(), bytes.end());
        ZoneMetadata out;
        auto zoneId = extractJsonString(text, "zoneId");
        if (zoneId) {
            out.zoneId = *zoneId;
        }
        out.displayName = out.zoneId;
        return out;
    }
};

class CRChunkContainer final : public ChunkContainer {
public:
    CRChunkContainer(std::shared_ptr<StorageBackend> storage, PersistenceContext context, CRChunkCodec& codec)
        : m_storage(std::move(storage)), m_context(std::move(context)), m_codec(codec) {
    }

    void saveRegion(const ChunkRegionSnapshot& region) override {
        auto path = CRPaths::regionPath(region.key, m_context);
        if (region.chunks.empty()) {
            m_storage->remove(path);
            return;
        }

        std::vector<std::vector<ChunkSnapshot>> columns(16 * 16);
        int32_t baseX = region.key.x * 16;
        int32_t baseY = region.key.y * 16;
        int32_t baseZ = region.key.z * 16;
        for (const auto& chunk : region.chunks) {
            int32_t localX = chunk.key.x - baseX;
            int32_t localY = chunk.key.y - baseY;
            int32_t localZ = chunk.key.z - baseZ;
            if (localX < 0 || localX >= 16 || localZ < 0 || localZ >= 16 || localY < 0 || localY >= 16) {
                continue;
            }
            int index = localX + localZ * 16;
            columns[index].push_back(chunk);
        }

        std::vector<int32_t> offsets(16 * 16, -1);
        std::vector<uint8_t> columnsBytes;
        VectorWriter columnsWriter(columnsBytes);
        int columnsWritten = 0;

        for (int index = 0; index < 16 * 16; ++index) {
            auto& col = columns[index];
            if (col.empty()) {
                continue;
            }
            std::sort(col.begin(), col.end(), [](const ChunkSnapshot& a, const ChunkSnapshot& b) {
                return a.key.y < b.key.y;
            });
            offsets[index] = static_cast<int32_t>(columnsWriter.size());
            ++columnsWritten;

            size_t columnStart = columnsWriter.tell();
            columnsWriter.writeI32(0);
            columnsWriter.writeI32(kFileVersion);
            size_t numChunksOffset = columnsWriter.tell();
            columnsWriter.writeU8(0);
            uint8_t numChunks = 0;

            for (const auto& chunk : col) {
                m_codec.write(chunk, columnsWriter);
                ++numChunks;
            }

            size_t columnEnd = columnsWriter.tell();
            int32_t columnSize = static_cast<int32_t>(columnEnd - columnStart);
            auto columnSizeBytes = encodeI32(columnSize);
            columnsWriter.writeAt(columnStart, columnSizeBytes.data(), columnSizeBytes.size());
            columnsWriter.writeAt(numChunksOffset, &numChunks, sizeof(numChunks));
        }

        int32_t maxOffset = 0;
        for (int32_t offset : offsets) {
            if (offset > maxOffset) {
                maxOffset = offset;
            }
        }

        uint8_t offsetType = 3;
        if (maxOffset < 0x7FFF) {
            offsetType = 2;
        }

        std::vector<uint8_t> payload;
        VectorWriter payloadWriter(payload);
        payloadWriter.writeU8(offsetType);
        for (int32_t offset : offsets) {
            int32_t value = offset;
            if (offsetType == 2) {
                payloadWriter.writeU16(static_cast<uint16_t>(value));
            } else {
                payloadWriter.writeI32(value);
            }
        }
        if (!columnsBytes.empty()) {
            payloadWriter.writeBytes(columnsBytes.data(), columnsBytes.size());
        }

        bool useCompression = false;
        if (m_context.providers) {
            auto settings = m_context.providers->findAs<CRPersistenceSettings>(kCRSettingsProviderId);
            if (settings) {
                useCompression = settings->enableLz4;
            }
        }

        auto session = m_storage->openWrite(path, AtomicWriteOptions{});
        auto& writer = session->writer();
        writer.writeI32(kMagic);
        writer.writeI32(kFileVersion);

        if (useCompression) {
            if (!CRLz4::available()) {
                throw std::runtime_error("CRRegion: LZ4 compression requested but unavailable");
            }
            int32_t decompressedSize = static_cast<int32_t>(payload.size());
            int bound = CRLz4::compressBound(decompressedSize);
            std::vector<uint8_t> compressed(static_cast<size_t>(bound));
            int compressedSize = CRLz4::compress(payload.data(), payload.size(), compressed.data(), compressed.size());
            if (compressedSize <= 0) {
                throw std::runtime_error("CRRegion: LZ4 compression failed");
            }
            compressed.resize(static_cast<size_t>(compressedSize));

            writer.writeI32(kCompressionLz4);
            writer.writeI32(columnsWritten);
            writer.writeI32(compressedSize);
            writer.writeI32(decompressedSize);
            writer.writeBytes(compressed.data(), compressed.size());
        } else {
            writer.writeI32(kCompressionNone);
            writer.writeI32(columnsWritten);
            if (!payload.empty()) {
                writer.writeBytes(payload.data(), payload.size());
            }
        }

        writer.flush();
        session->commit();
    }

    ChunkRegionSnapshot loadRegion(const RegionKey& key) override {
        ChunkRegionSnapshot region;
        region.key = key;
        auto path = CRPaths::regionPath(key, m_context);
        if (!m_storage->exists(path)) {
            return region;
        }
        auto reader = m_storage->openRead(path);
        int32_t magic = reader->readI32();
        if (magic != kMagic) {
            throw std::runtime_error("CRRegion: invalid magic");
        }
        int32_t version = reader->readI32();
        if (version > kFileVersion) {
            throw std::runtime_error("CRRegion: unsupported version");
        }
        int32_t compressionType = reader->readI32();
        reader->readI32();

        std::unique_ptr<ByteReader> payloadReader;
        if (compressionType == kCompressionLz4) {
            int32_t compressedSize = reader->readI32();
            int32_t decompressedSize = reader->readI32();
            if (!CRLz4::available()) {
                throw std::runtime_error("CRRegion: LZ4 compression unavailable");
            }
            if (compressedSize <= 0 || decompressedSize <= 0) {
                throw std::runtime_error("CRRegion: invalid compressed sizes");
            }
            std::vector<uint8_t> compressed(static_cast<size_t>(compressedSize));
            reader->readBytes(compressed.data(), compressed.size());
            std::vector<uint8_t> decompressed(static_cast<size_t>(decompressedSize));
            int result = CRLz4::decompress(compressed.data(), compressed.size(), decompressed.data(), decompressed.size());
            if (result < 0) {
                throw std::runtime_error("CRRegion: LZ4 decompression failed");
            }
            payloadReader = std::make_unique<MemoryByteReader>(std::move(decompressed));
        } else if (compressionType != kCompressionNone) {
            throw std::runtime_error("CRRegion: unknown compression type");
        }

        ByteReader* dataReader = payloadReader ? payloadReader.get() : reader.get();
        uint8_t offsetType = dataReader->readU8();
        std::vector<int32_t> offsets(16 * 16, -1);
        size_t tableStart = dataReader->tell();
        size_t offsetTableSize = 0;
        if (offsetType == 1) {
            offsetTableSize = offsets.size();
        } else if (offsetType == 2) {
            offsetTableSize = offsets.size() * 2;
        } else {
            offsetTableSize = offsets.size() * 4;
        }
        size_t offsetOffset = tableStart + offsetTableSize;

        for (size_t i = 0; i < offsets.size(); ++i) {
            int32_t value = -1;
            if (offsetType == 1) {
                value = static_cast<int8_t>(dataReader->readU8());
            } else if (offsetType == 2) {
                value = static_cast<int16_t>(dataReader->readU16());
            } else {
                value = dataReader->readI32();
            }
            if (value != -1) {
                offsets[i] = static_cast<int32_t>(offsetOffset + value);
            }
        }

        ChunkKey hint{key.zoneId, 0, 0, 0};
        for (size_t index = 0; index < offsets.size(); ++index) {
            int32_t offset = offsets[index];
            if (offset < 0) {
                continue;
            }
            dataReader->seek(static_cast<size_t>(offset));
            int32_t columnByteSize = dataReader->readI32();
            if (columnByteSize <= 0) {
                continue;
            }
            dataReader->readI32();
            uint8_t numChunks = dataReader->readU8();
            for (uint8_t i = 0; i < numChunks; ++i) {
                region.chunks.push_back(m_codec.read(*dataReader, hint));
            }
        }
        return region;
    }

private:
    std::shared_ptr<StorageBackend> m_storage;
    PersistenceContext m_context;
    CRChunkCodec& m_codec;
};

class CREntityContainer final : public EntityContainer {
public:
    CREntityContainer(std::shared_ptr<StorageBackend> storage, PersistenceContext context)
        : m_storage(std::move(storage)), m_context(std::move(context)) {
    }

    void saveRegion(const EntityRegionSnapshot& region) override {
        auto path = CRPaths::entityRegionPath(region.key, m_context);
        if (region.payload.empty()) {
            if (m_storage->exists(path)) {
                m_storage->remove(path);
            }
            return;
        }
        auto session = m_storage->openWrite(path, AtomicWriteOptions{});
        session->writer().writeBytes(region.payload.data(), region.payload.size());
        session->writer().flush();
        session->commit();
    }

    EntityRegionSnapshot loadRegion(const EntityRegionKey& key) override {
        EntityRegionSnapshot out;
        out.key = key;
        auto path = CRPaths::entityRegionPath(key, m_context);
        if (!m_storage->exists(path)) {
            return out;
        }
        auto reader = m_storage->openRead(path);
        out.payload.resize(reader->size());
        reader->seek(0);
        if (!out.payload.empty()) {
            reader->readBytes(out.payload.data(), out.payload.size());
        }
        return out;
    }

private:
    std::shared_ptr<StorageBackend> m_storage;
    PersistenceContext m_context;
};

class CRFormat final : public PersistenceFormat {
public:
    CRFormat(std::shared_ptr<StorageBackend> storage, PersistenceContext context)
        : m_storage(std::move(storage)),
          m_context(std::move(context)),
          m_chunkContainer(m_storage, m_context, m_chunkCodec),
          m_entityContainer(m_storage, m_context) {
        m_worldCodec.setContext(m_context);
    }

    const FormatDescriptor& descriptor() const override {
        return Backends::CR::descriptor();
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
    CRWorldMetadataCodec m_worldCodec;
    CRZoneMetadataCodec m_zoneCodec;
    CRChunkCodec m_chunkCodec;
    CRChunkContainer m_chunkContainer;
    CREntityContainer m_entityContainer;
};

} // namespace

const FormatDescriptor& descriptor() {
    static FormatDescriptor desc = []() {
        FormatDescriptor init;
        init.id = "cr";
        init.version = kFileVersion;
        init.extensions = {"cosmicreach", "crbin", "json"};
        init.capabilities.supportsPartialChunkSave = false;
        init.capabilities.supportsRandomAccess = false;
        init.capabilities.supportsEntityRegions = true;
        init.capabilities.supportsVersions = true;
        init.capabilities.metadataFormat = "json";
        init.capabilities.regionIndexType = "byte|short|int";
        init.capabilities.compression = CompressionType::Lz4;
        return init;
    }();
    return desc;
}

FormatFactory factory() {
    return [](const PersistenceContext& context) -> std::unique_ptr<PersistenceFormat> {
        if (!context.storage) {
            throw std::runtime_error("CRFormat: storage backend is required");
        }
        return std::make_unique<CRFormat>(context.storage, context);
    };
}

FormatProbe probe() {
    return [](StorageBackend& storage, const PersistenceContext& context) -> std::optional<ProbeResult> {
        if (storage.exists(CRPaths::worldInfoPath(context))) {
            return ProbeResult{descriptor().id, descriptor().version, 0.8f};
        }
        auto zonesPath = context.rootPath + "/zones";
        if (storage.exists(zonesPath)) {
            return ProbeResult{descriptor().id, descriptor().version, 0.4f};
        }
        return std::nullopt;
    };
}

} // namespace Rigel::Persistence::Backends::CR
