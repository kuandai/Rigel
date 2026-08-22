#include "Rigel/Persistence/Backends/CR/CRFormat.h"

#include "Rigel/Persistence/Backends/CR/CRChunkMapping.h"
#include "Rigel/Persistence/Backends/CR/CRPaths.h"
#include "Rigel/Persistence/Backends/CR/CRSettings.h"
#include "Rigel/Persistence/Backends/CR/CRLz4.h"
#include "Rigel/Persistence/Providers.h"
#include "Rigel/Persistence/Storage.h"
#include "Rigel/Entity/EntityPersistence.h"
#include "Rigel/Voxel/BlockRegistry.h"
#include "Rigel/Voxel/Chunk.h"
#include "CRWorldMetadata.h"
#include "MemoryByteReader.h"
#include "../../../entity/EntityPersistenceLimits.h"
#include "../../RegionFilename.h"
#include "../../ZoneIdentifier.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
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

constexpr size_t kRegionSpan = 16;
constexpr size_t kRegionColumnCount = kRegionSpan * kRegionSpan;
constexpr size_t kMaxChunksPerColumn = kRegionSpan;
constexpr size_t kColumnHeaderBytes = 2 * sizeof(int32_t) + sizeof(uint8_t);
constexpr size_t kMinChunkRecordBytes = 3 * sizeof(int32_t) + 4 * sizeof(uint8_t);
constexpr size_t kMaxDecompressedRegionBytes = 64 * 1024 * 1024;
constexpr size_t kMaxCompressedRegionBytes =
    kMaxDecompressedRegionBytes + kMaxDecompressedRegionBytes / 255 + 16;
constexpr size_t kMaxColumnBytes = kMaxDecompressedRegionBytes;
constexpr size_t kMaxChunkRecordBytes = 4 * 1024 * 1024;
constexpr size_t kMaxChunkStringBytes = 1024 * 1024;
constexpr size_t kMaxBlockEntityBytes = 1024 * 1024;
constexpr size_t kMaxMetadataStringBytes = 1024 * 1024;
constexpr size_t kMaxMetadataDocumentBytes = 4 * 1024 * 1024;
constexpr int32_t kMaxPaletteEntries = 16 * 16 * 16;

size_t remainingInput(const ByteReader& reader) {
    const size_t position = reader.tell();
    const size_t size = reader.size();
    if (position > size) {
        throw std::runtime_error("CRRegion: invalid reader position");
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

int32_t floorDiv(int32_t value, int32_t divisor) {
    int32_t q = value / divisor;
    int32_t r = value % divisor;
    if (r < 0) {
        q -= 1;
    }
    return q;
}

std::string parentPath(const std::string& path) {
    auto pos = path.find_last_of('/');
    if (pos == std::string::npos) {
        return std::string();
    }
    return path.substr(0, pos);
}

class CRRegionLayout final : public RegionLayout {
public:
    RegionKey regionForChunk(const std::string& zoneId, Voxel::ChunkCoord coord) const override {
        detail::validateZoneIdentifier(zoneId);
        constexpr int32_t rigelRegionSpan = 8;
        return RegionKey{
            zoneId,
            floorDiv(coord.x, rigelRegionSpan),
            floorDiv(coord.y, rigelRegionSpan),
            floorDiv(coord.z, rigelRegionSpan)
        };
    }

    std::vector<ChunkKey> storageKeysForChunk(const std::string& zoneId,
                                              Voxel::ChunkCoord coord) const override {
        detail::validateZoneIdentifier(zoneId);
        std::vector<ChunkKey> keys;
        keys.reserve(Voxel::Chunk::SUBCHUNK_COUNT);
        for (int subchunkIndex = 0; subchunkIndex < Voxel::Chunk::SUBCHUNK_COUNT; ++subchunkIndex) {
            ChunkKey key = toCRChunk({coord.x, coord.y, coord.z, subchunkIndex});
            key.zoneId = zoneId;
            keys.push_back(key);
        }
        return keys;
    }

    ChunkSpan spanForStorageKey(const ChunkKey& key) const override {
        detail::validateZoneIdentifier(key.zoneId);
        auto rigelCoord = toRigelChunk(key);
        ChunkSpan span;
        span.chunkX = rigelCoord.rigelChunkX;
        span.chunkY = rigelCoord.rigelChunkY;
        span.chunkZ = rigelCoord.rigelChunkZ;
        span.offsetX = (rigelCoord.subchunkIndex & 1) * 16;
        span.offsetY = ((rigelCoord.subchunkIndex >> 1) & 1) * 16;
        span.offsetZ = ((rigelCoord.subchunkIndex >> 2) & 1) * 16;
        span.sizeX = 16;
        span.sizeY = 16;
        span.sizeZ = 16;
        return span;
    }

};

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

bool parseRegionFilename(const std::string& name, int32_t& rx, int32_t& ry, int32_t& rz) {
    return detail::parseCanonicalRegionFilename(
        name, "region_", ".cosmicreach", rx, ry, rz);
}

bool parseEntityRegionFilename(const std::string& name, int32_t& rx, int32_t& ry, int32_t& rz) {
    return detail::parseCanonicalRegionFilename(
        name, "entityRegion_", ".crbin", rx, ry, rz);
}

std::array<uint8_t, 4> encodeI32(int32_t value) {
    return {
        static_cast<uint8_t>((value >> 24) & 0xFF),
        static_cast<uint8_t>((value >> 16) & 0xFF),
        static_cast<uint8_t>((value >> 8) & 0xFF),
        static_cast<uint8_t>(value & 0xFF)
    };
}

[[noreturn]] void throwInvalidJsonString() {
    throw std::runtime_error("CRMetadata: invalid JSON string");
}

[[noreturn]] void throwInvalidJsonDocument() {
    throw std::runtime_error("CRMetadata: invalid JSON document");
}

void requireMetadataStringSize(size_t size) {
    if (size > kMaxMetadataStringBytes) {
        throw std::runtime_error(
            "CRMetadata: string exceeds format limit");
    }
}

void requireMetadataDocumentSize(size_t size) {
    if (size > kMaxMetadataDocumentBytes) {
        throw std::runtime_error(
            "CRMetadata: document exceeds format limit");
    }
}

size_t encodedJsonStringSize(std::string_view value) {
    requireMetadataStringSize(value.size());
    size_t encodedSize = 2;
    for (const unsigned char byte : value) {
        size_t byteSize = 1;
        if (byte == '"' || byte == '\\' || byte == '\b' || byte == '\f' ||
            byte == '\n' || byte == '\r' || byte == '\t') {
            byteSize = 2;
        } else if (byte < 0x20) {
            byteSize = 6;
        }
        if (encodedSize > kMaxMetadataDocumentBytes ||
            byteSize > kMaxMetadataDocumentBytes - encodedSize) {
            throw std::runtime_error(
                "CRMetadata: document exceeds format limit");
        }
        encodedSize += byteSize;
    }
    return encodedSize;
}

void appendJsonString(std::string& output, std::string_view value) {
    constexpr char kHex[] = "0123456789abcdef";
    output.push_back('"');
    for (const unsigned char byte : value) {
        switch (byte) {
        case '"':
            output += "\\\"";
            break;
        case '\\':
            output += "\\\\";
            break;
        case '\b':
            output += "\\b";
            break;
        case '\f':
            output += "\\f";
            break;
        case '\n':
            output += "\\n";
            break;
        case '\r':
            output += "\\r";
            break;
        case '\t':
            output += "\\t";
            break;
        default:
            if (byte < 0x20) {
                output += "\\u00";
                output.push_back(kHex[(byte >> 4) & 0x0F]);
                output.push_back(kHex[byte & 0x0F]);
            } else {
                output.push_back(static_cast<char>(byte));
            }
            break;
        }
    }
    output.push_back('"');
}

std::string encodeMetadataDocument(std::string_view prefix,
                                   std::string_view value,
                                   std::string_view suffix) {
    const size_t valueSize = encodedJsonStringSize(value);
    if (prefix.size() > kMaxMetadataDocumentBytes ||
        valueSize > kMaxMetadataDocumentBytes - prefix.size()) {
        throw std::runtime_error(
            "CRMetadata: document exceeds format limit");
    }
    const size_t prefixAndValueSize = prefix.size() + valueSize;
    if (suffix.size() > kMaxMetadataDocumentBytes - prefixAndValueSize) {
        throw std::runtime_error(
            "CRMetadata: document exceeds format limit");
    }

    std::string document;
    document.reserve(prefixAndValueSize + suffix.size());
    document.append(prefix);
    appendJsonString(document, value);
    document.append(suffix);
    return document;
}

std::string readMetadataDocument(ByteReader& reader) {
    const size_t size = reader.size();
    requireMetadataDocumentSize(size);
    std::string text(size, '\0');
    reader.seek(0);
    if (!text.empty()) {
        reader.readBytes(
            reinterpret_cast<uint8_t*>(text.data()), text.size());
    }
    return text;
}

uint32_t readHexCodeUnit(std::string_view text, size_t& position) {
    if (position > text.size() || text.size() - position < 4) {
        throwInvalidJsonString();
    }
    uint32_t value = 0;
    for (size_t index = 0; index < 4; ++index) {
        const char digit = text[position++];
        value <<= 4;
        if (digit >= '0' && digit <= '9') {
            value |= static_cast<uint32_t>(digit - '0');
        } else if (digit >= 'a' && digit <= 'f') {
            value |= static_cast<uint32_t>(digit - 'a' + 10);
        } else if (digit >= 'A' && digit <= 'F') {
            value |= static_cast<uint32_t>(digit - 'A' + 10);
        } else {
            throwInvalidJsonString();
        }
    }
    return value;
}

void appendDecodedBytes(std::string& output,
                        const char* bytes,
                        size_t size) {
    if (output.size() > kMaxMetadataStringBytes ||
        size > kMaxMetadataStringBytes - output.size()) {
        throw std::runtime_error(
            "CRMetadata: string exceeds format limit");
    }
    output.append(bytes, size);
}

void appendCodePoint(std::string& output, uint32_t codePoint) {
    std::array<char, 4> bytes{};
    size_t size = 0;
    if (codePoint <= 0x7F) {
        bytes[0] = static_cast<char>(codePoint);
        size = 1;
    } else if (codePoint <= 0x7FF) {
        bytes[0] = static_cast<char>(0xC0 | (codePoint >> 6));
        bytes[1] = static_cast<char>(0x80 | (codePoint & 0x3F));
        size = 2;
    } else if (codePoint <= 0xFFFF) {
        bytes[0] = static_cast<char>(0xE0 | (codePoint >> 12));
        bytes[1] = static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
        bytes[2] = static_cast<char>(0x80 | (codePoint & 0x3F));
        size = 3;
    } else {
        bytes[0] = static_cast<char>(0xF0 | (codePoint >> 18));
        bytes[1] = static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F));
        bytes[2] = static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
        bytes[3] = static_cast<char>(0x80 | (codePoint & 0x3F));
        size = 4;
    }
    appendDecodedBytes(output, bytes.data(), size);
}

size_t consumeJsonString(std::string_view text,
                         size_t start,
                         std::string* decoded) {
    if (start >= text.size() || text[start] != '"') {
        throwInvalidJsonString();
    }
    size_t position = start + 1;
    while (position < text.size()) {
        const unsigned char byte =
            static_cast<unsigned char>(text[position++]);
        if (byte == '"') {
            return position;
        }
        if (byte < 0x20) {
            throwInvalidJsonString();
        }
        if (byte != '\\') {
            if (decoded) {
                const char value = static_cast<char>(byte);
                appendDecodedBytes(*decoded, &value, 1);
            }
            continue;
        }
        if (position >= text.size()) {
            throwInvalidJsonString();
        }

        const char escape = text[position++];
        char decodedEscape = '\0';
        switch (escape) {
        case '"':
        case '\\':
        case '/':
            decodedEscape = escape;
            break;
        case 'b':
            decodedEscape = '\b';
            break;
        case 'f':
            decodedEscape = '\f';
            break;
        case 'n':
            decodedEscape = '\n';
            break;
        case 'r':
            decodedEscape = '\r';
            break;
        case 't':
            decodedEscape = '\t';
            break;
        case 'u': {
            uint32_t codePoint = readHexCodeUnit(text, position);
            if (codePoint >= 0xD800 && codePoint <= 0xDBFF) {
                if (position > text.size() || text.size() - position < 6 ||
                    text[position] != '\\' || text[position + 1] != 'u') {
                    throwInvalidJsonString();
                }
                position += 2;
                const uint32_t lowSurrogate =
                    readHexCodeUnit(text, position);
                if (lowSurrogate < 0xDC00 || lowSurrogate > 0xDFFF) {
                    throwInvalidJsonString();
                }
                codePoint = 0x10000 +
                    ((codePoint - 0xD800) << 10) +
                    (lowSurrogate - 0xDC00);
            } else if (codePoint >= 0xDC00 && codePoint <= 0xDFFF) {
                throwInvalidJsonString();
            }
            if (decoded) {
                appendCodePoint(*decoded, codePoint);
            }
            continue;
        }
        default:
            throwInvalidJsonString();
        }
        if (decoded) {
            appendDecodedBytes(*decoded, &decodedEscape, 1);
        }
    }
    throwInvalidJsonString();
}

bool isJsonWhitespace(char value) {
    return value == ' ' || value == '\t' || value == '\n' || value == '\r';
}

std::optional<std::string> extractJsonString(std::string_view text,
                                             std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\"";
    std::optional<std::string> result;
    size_t position = 0;
    while (position < text.size() && isJsonWhitespace(text[position])) {
        ++position;
    }
    if (position >= text.size() || text[position] != '{') {
        throwInvalidJsonDocument();
    }

    std::vector<char> closingDelimiters{'}'};
    ++position;
    while (!closingDelimiters.empty()) {
        if (position >= text.size()) {
            throwInvalidJsonDocument();
        }
        if (text[position] != '"') {
            const char token = text[position++];
            if (token == '{') {
                closingDelimiters.push_back('}');
            } else if (token == '[') {
                closingDelimiters.push_back(']');
            } else if (token == '}' || token == ']') {
                if (closingDelimiters.back() != token) {
                    throwInvalidJsonDocument();
                }
                closingDelimiters.pop_back();
            }
            continue;
        }

        const size_t tokenStart = position;
        const size_t tokenEnd = consumeJsonString(text, tokenStart, nullptr);
        if (closingDelimiters.size() != 1 ||
            closingDelimiters.back() != '}') {
            position = tokenEnd;
            continue;
        }

        const bool matchesKey =
            tokenEnd - tokenStart == needle.size() &&
            text.compare(tokenStart, needle.size(), needle) == 0;
        if (!matchesKey) {
            position = tokenEnd;
            continue;
        }

        size_t valueStart = tokenEnd;
        while (valueStart < text.size() && isJsonWhitespace(text[valueStart])) {
            ++valueStart;
        }
        if (valueStart >= text.size() || text[valueStart] != ':') {
            position = tokenEnd;
            continue;
        }
        ++valueStart;
        while (valueStart < text.size() && isJsonWhitespace(text[valueStart])) {
            ++valueStart;
        }
        if (valueStart >= text.size() || text[valueStart] != '"') {
            position = tokenEnd;
            continue;
        }

        std::string value;
        position = consumeJsonString(text, valueStart, &value);
        if (!result) {
            result = std::move(value);
        }
    }

    while (position < text.size() && isJsonWhitespace(text[position])) {
        ++position;
    }
    if (position != text.size()) {
        throwInvalidJsonDocument();
    }
    return result;
}

class TrackingReader {
public:
    explicit TrackingReader(ByteReader& reader)
        : m_reader(reader), m_start(reader.tell()) {
    }

    uint8_t readU8() {
        requireReadable(1);
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
        requireReadable(len);
        const size_t offset = m_bytes.size();
        m_bytes.resize(offset + len);
        m_reader.readBytes(m_bytes.data() + offset, len);
    }

    void readBytes(uint8_t* dst, size_t len) {
        if (len == 0) {
            return;
        }
        requireReadable(len);
        m_reader.readBytes(dst, len);
        m_bytes.insert(m_bytes.end(), dst, dst + len);
    }

    size_t remaining() const {
        const size_t input = remainingInput(m_reader);
        const size_t consumed = bytesConsumed();
        const size_t record = kMaxChunkRecordBytes - consumed;
        return std::min(input, record);
    }

    std::vector<uint8_t> takeBytes() {
        return std::move(m_bytes);
    }

private:
    size_t bytesConsumed() const {
        const size_t position = m_reader.tell();
        if (position < m_start || position - m_start > kMaxChunkRecordBytes) {
            throw std::runtime_error("CRChunkCodec: invalid reader position");
        }
        return position - m_start;
    }

    void requireReadable(size_t len) const {
        if (len > remainingInput(m_reader)) {
            throw std::runtime_error("CRChunkCodec: payload exceeds column extent");
        }
        if (len > kMaxChunkRecordBytes - bytesConsumed()) {
            throw std::runtime_error("CRChunkCodec: record exceeds format limit");
        }
    }

    ByteReader& m_reader;
    size_t m_start = 0;
    std::vector<uint8_t> m_bytes;
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

class BoundedWriter final : public ByteWriter {
public:
    BoundedWriter(ByteWriter& writer, size_t limit, const char* diagnostic)
        : m_writer(writer),
          m_start(writer.tell()),
          m_limit(limit),
          m_diagnostic(diagnostic) {
    }

    void writeU8(uint8_t value) override {
        requireRange(m_writer.tell(), sizeof(value));
        m_writer.writeU8(value);
    }

    void writeU16(uint16_t value) override {
        requireRange(m_writer.tell(), sizeof(value));
        m_writer.writeU16(value);
    }

    void writeU32(uint32_t value) override {
        requireRange(m_writer.tell(), sizeof(value));
        m_writer.writeU32(value);
    }

    void writeI32(int32_t value) override {
        requireRange(m_writer.tell(), sizeof(value));
        m_writer.writeI32(value);
    }

    void writeBytes(const uint8_t* src, size_t len) override {
        requireRange(m_writer.tell(), len);
        m_writer.writeBytes(src, len);
    }

    size_t size() const override {
        return m_writer.size();
    }

    size_t tell() const override {
        return m_writer.tell();
    }

    void seek(size_t offset) override {
        requireRange(offset, 0);
        m_writer.seek(offset);
    }

    void writeAt(size_t offset, const uint8_t* src, size_t len) override {
        requireRange(offset, len);
        m_writer.writeAt(offset, src, len);
    }

    void flush() override {
        m_writer.flush();
    }

private:
    void requireRange(size_t offset, size_t len) const {
        if (offset < m_start || offset - m_start > m_limit ||
            len > m_limit - (offset - m_start)) {
            throw std::runtime_error(m_diagnostic);
        }
    }

    ByteWriter& m_writer;
    size_t m_start = 0;
    size_t m_limit = 0;
    const char* m_diagnostic = nullptr;
};

std::string readString(TrackingReader& reader) {
    int32_t len = reader.readI32();
    if (len < 0) {
        throw std::runtime_error("CRChunkCodec: invalid string length");
    }
    if (static_cast<size_t>(len) > kMaxChunkStringBytes) {
        throw std::runtime_error("CRChunkCodec: string length exceeds format limit");
    }
    if (len == 0) {
        return std::string();
    }
    if (static_cast<size_t>(len) > reader.remaining()) {
        throw std::runtime_error("CRChunkCodec: string length exceeds column extent");
    }
    std::string out;
    out.resize(static_cast<size_t>(len));
    reader.readBytes(reinterpret_cast<uint8_t*>(&out[0]), static_cast<size_t>(len));
    return out;
}

void writeString(ByteWriter& writer, const std::string& value) {
    if (value.size() > kMaxChunkStringBytes) {
        throw std::runtime_error("CRChunkCodec: string length exceeds format limit");
    }
    writer.writeI32(static_cast<int32_t>(value.size()));
    if (!value.empty()) {
        writer.writeBytes(reinterpret_cast<const uint8_t*>(value.data()), value.size());
    }
}

void readLayer(TrackingReader& reader, uint8_t layerType, std::array<uint16_t, 256>& indices) {
    switch (layerType) {
    case kBlockLayerSingleByte: {
        uint16_t value = reader.readU8();
        indices.fill(value);
        return;
    }
    case kBlockLayerSingleInt: {
        const int32_t value = reader.readI32();
        if (value < 0 ||
            value > static_cast<int32_t>(std::numeric_limits<uint16_t>::max())) {
            throw std::runtime_error("CRChunkCodec: palette index out of range");
        }
        indices.fill(static_cast<uint16_t>(value));
        return;
    }
    case kBlockLayerHalfNibble: {
        std::array<uint8_t, 64> bytes{};
        reader.readBytes(bytes.data(), bytes.size());
        for (int z = 0; z < 16; ++z) {
            for (int x = 0; x < 16; ++x) {
                int idx = (x + z * 16) / 4;
                uint8_t b = bytes[static_cast<size_t>(idx)];
                int mod = x % 4;
                uint8_t value = (b >> (mod * 2)) & 0x03;
                indices[static_cast<size_t>(x + z * 16)] = value;
            }
        }
        return;
    }
    case kBlockLayerNibble: {
        std::array<uint8_t, 128> bytes{};
        reader.readBytes(bytes.data(), bytes.size());
        for (int z = 0; z < 16; ++z) {
            for (int x = 0; x < 16; ++x) {
                int idx = (x + z * 16) / 2;
                uint8_t b = bytes[static_cast<size_t>(idx)];
                uint16_t value = (x % 2 == 0) ? (b & 0x0F) : ((b & 0xF0) >> 4);
                indices[static_cast<size_t>(x + z * 16)] = value;
            }
        }
        return;
    }
    case kBlockLayerByte: {
        for (size_t i = 0; i < indices.size(); ++i) {
            indices[i] = reader.readU8();
        }
        return;
    }
    case kBlockLayerShort: {
        for (size_t i = 0; i < indices.size(); ++i) {
            indices[i] = reader.readU16();
        }
        return;
    }
    case kBlockLayerBit: {
        std::array<uint8_t, 32> bytes{};
        reader.readBytes(bytes.data(), bytes.size());
        for (int z = 0; z < 16; ++z) {
            for (int x = 0; x < 16; ++x) {
                int idx = (x + z * 16) / 8;
                uint8_t b = bytes[static_cast<size_t>(idx)];
                int mod = x % 8;
                uint16_t value = (b >> mod) & 0x01;
                indices[static_cast<size_t>(x + z * 16)] = value;
            }
        }
        return;
    }
    default:
        throw std::runtime_error("CRChunkCodec: unknown block layer type");
    }
}

std::vector<std::string> buildPalette(const std::vector<Voxel::BlockState>& blocks,
                                      const Voxel::BlockRegistry& registry,
                                      const PersistencePolicies& policies,
                                      std::unordered_map<uint16_t, uint16_t>& paletteIndex) {
    std::vector<std::string> palette;
    for (const auto& state : blocks) {
        uint16_t id = state.id.type;
        if (paletteIndex.find(id) != paletteIndex.end()) {
            continue;
        }
        if (palette.size() >= static_cast<size_t>(kMaxPaletteEntries)) {
            throw std::runtime_error("CRChunkCodec: palette exceeds format limit");
        }
        uint16_t index = static_cast<uint16_t>(palette.size());
        paletteIndex[id] = index;
        if (id < registry.size()) {
            palette.push_back(registry.getType(Voxel::BlockID{id}).identifier);
            continue;
        }
        if (policies.unknownBlockPolicy == UnknownIdPolicy::Fail) {
            throw std::runtime_error(
                "CRChunkCodec: unknown runtime block identifier " + std::to_string(id));
        }
        palette.push_back(registry.getType(Voxel::BlockRegistry::airId()).identifier);
    }
    return palette;
}

void writeLayer(ByteWriter& writer,
                const std::array<uint16_t, 256>& indices,
                uint16_t paletteSize) {
    if (paletteSize <= 2) {
        std::array<uint8_t, 32> bytes{};
        for (int z = 0; z < 16; ++z) {
            for (int x = 0; x < 16; ++x) {
                int idx = (x + z * 16) / 8;
                uint8_t bit = static_cast<uint8_t>(indices[static_cast<size_t>(x + z * 16)] & 0x01);
                bytes[static_cast<size_t>(idx)] |= (bit << (x % 8));
            }
        }
        writer.writeU8(kBlockLayerBit);
        writer.writeBytes(bytes.data(), bytes.size());
        return;
    }

    if (paletteSize <= 4) {
        std::array<uint8_t, 64> bytes{};
        for (int z = 0; z < 16; ++z) {
            for (int x = 0; x < 16; ++x) {
                int idx = (x + z * 16) / 4;
                uint8_t value = static_cast<uint8_t>(indices[static_cast<size_t>(x + z * 16)] & 0x03);
                bytes[static_cast<size_t>(idx)] |= (value << ((x % 4) * 2));
            }
        }
        writer.writeU8(kBlockLayerHalfNibble);
        writer.writeBytes(bytes.data(), bytes.size());
        return;
    }

    if (paletteSize <= 16) {
        std::array<uint8_t, 128> bytes{};
        for (int z = 0; z < 16; ++z) {
            for (int x = 0; x < 16; ++x) {
                int idx = (x + z * 16) / 2;
                uint8_t value = static_cast<uint8_t>(indices[static_cast<size_t>(x + z * 16)] & 0x0F);
                if (x % 2 == 0) {
                    bytes[static_cast<size_t>(idx)] |= value;
                } else {
                    bytes[static_cast<size_t>(idx)] |= static_cast<uint8_t>(value << 4);
                }
            }
        }
        writer.writeU8(kBlockLayerNibble);
        writer.writeBytes(bytes.data(), bytes.size());
        return;
    }

    if (paletteSize <= 256) {
        writer.writeU8(kBlockLayerByte);
        for (size_t i = 0; i < indices.size(); ++i) {
            writer.writeU8(static_cast<uint8_t>(indices[i]));
        }
        return;
    }

    writer.writeU8(kBlockLayerShort);
    for (size_t i = 0; i < indices.size(); ++i) {
        writer.writeU16(indices[i]);
    }
}

std::vector<Voxel::BlockState> decodeBlocks(TrackingReader& reader,
                                            const Voxel::BlockRegistry* registry,
                                            const PersistencePolicies& policies) {
    std::vector<Voxel::BlockState> blocks(16 * 16 * 16, Voxel::BlockState{});
    auto resolveBlockId = [registry, &policies](const std::string& id) -> Voxel::BlockID {
        if (registry) {
            if (auto found = registry->findByIdentifier(id)) {
                return *found;
            }
            constexpr std::string_view kLegacyNamespace = "rigel:";
            constexpr std::string_view kBaseNamespace = "base:";
            if (id.rfind(kLegacyNamespace, 0) == 0) {
                std::string fallback = std::string(kBaseNamespace) + id.substr(kLegacyNamespace.size());
                if (auto found = registry->findByIdentifier(fallback)) {
                    return *found;
                }
            } else if (id.rfind(kBaseNamespace, 0) == 0) {
                std::string fallback = std::string(kLegacyNamespace) + id.substr(kBaseNamespace.size());
                if (auto found = registry->findByIdentifier(fallback)) {
                    return *found;
                }
            }
        }
        if (policies.unknownBlockPolicy == UnknownIdPolicy::Fail) {
            throw std::runtime_error("CRChunkCodec: unknown block identifier '" + id + "'");
        }
        return Voxel::BlockRegistry::airId();
    };

    uint8_t blockType = reader.readU8();
    if (blockType == kBlockNull) {
        return blocks;
    }

    std::vector<Voxel::BlockID> paletteIds;

    if (blockType == kBlockSingle) {
        std::string keyString = readString(reader);
        Voxel::BlockID blockId = resolveBlockId(keyString);
        for (auto& state : blocks) {
            state.id = blockId;
        }
        return blocks;
    }

    if (blockType == kBlockLayered) {
        int32_t paletteSize = reader.readI32();
        if (paletteSize <= 0 || paletteSize > kMaxPaletteEntries) {
            throw std::runtime_error("CRChunkCodec: invalid palette size");
        }
        if (static_cast<size_t>(paletteSize) > reader.remaining() / sizeof(int32_t)) {
            throw std::runtime_error("CRChunkCodec: palette exceeds column extent");
        }
        paletteIds.reserve(static_cast<size_t>(paletteSize));
        for (int32_t i = 0; i < paletteSize; ++i) {
            std::string id = readString(reader);
            paletteIds.push_back(resolveBlockId(id));
        }

        for (int layer = 0; layer < 16; ++layer) {
            uint8_t layerType = reader.readU8();
            std::array<uint16_t, 256> indices{};
            readLayer(reader, layerType, indices);
            for (int z = 0; z < 16; ++z) {
                for (int x = 0; x < 16; ++x) {
                    uint16_t paletteIndex = indices[static_cast<size_t>(x + z * 16)];
                    if (paletteIndex >= paletteIds.size()) {
                        throw std::runtime_error("CRChunkCodec: palette index out of range");
                    }
                    Voxel::BlockID blockId = paletteIds[paletteIndex];
                    size_t idx = static_cast<size_t>(x + z * 16 + layer * 256);
                    blocks[idx].id = blockId;
                }
            }
        }
        return blocks;
    }

    throw std::runtime_error("CRChunkCodec: unknown block data type");
}

void writeBlockData(ByteWriter& writer,
                    const std::vector<Voxel::BlockState>& blocks,
                    const Voxel::BlockRegistry* registry,
                    const PersistencePolicies& policies) {
    if (blocks.empty()) {
        writer.writeU8(kBlockNull);
        return;
    }

    bool hasSolid = false;
    for (const auto& state : blocks) {
        if (!state.isAir()) {
            hasSolid = true;
            break;
        }
    }
    if (!hasSolid) {
        writer.writeU8(kBlockNull);
        return;
    }

    if (!registry) {
        if (policies.unknownBlockPolicy == UnknownIdPolicy::Fail) {
            throw std::runtime_error("CRChunkCodec: missing block registry");
        }
        static const Voxel::BlockRegistry fallbackRegistry;
        registry = &fallbackRegistry;
    }

    std::unordered_map<uint16_t, uint16_t> paletteIndex;
    auto palette = buildPalette(blocks, *registry, policies, paletteIndex);
    if (palette.size() == 1) {
        writer.writeU8(kBlockSingle);
        writeString(writer, palette[0]);
        return;
    }

    writer.writeU8(kBlockLayered);
    writer.writeI32(static_cast<int32_t>(palette.size()));
    for (const auto& key : palette) {
        writeString(writer, key);
    }

    for (int layer = 0; layer < 16; ++layer) {
        std::array<uint16_t, 256> indices{};
        for (int z = 0; z < 16; ++z) {
            for (int x = 0; x < 16; ++x) {
                size_t index = static_cast<size_t>(x + z * 16 + layer * 256);
                uint16_t blockId = blocks[index].id.type;
                auto it = paletteIndex.find(blockId);
                if (it == paletteIndex.end()) {
                    throw std::runtime_error("CRChunkCodec: missing palette entry");
                }
                uint16_t paletteId = it->second;
                indices[static_cast<size_t>(x + z * 16)] = paletteId;
            }
        }
        writeLayer(writer, indices, static_cast<uint16_t>(palette.size()));
    }
}

bool readSkylightData(TrackingReader& reader) {
    uint8_t type = reader.readU8();
    switch (type) {
    case kSkyNull:
        return false;
    case kSkySingle:
        reader.readU8();
        return true;
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
        return true;
    default:
        throw std::runtime_error("CRChunkCodec: unknown skylight type");
    }
}

bool readBlockLightData(TrackingReader& reader) {
    uint8_t type = reader.readU8();
    switch (type) {
    case kBlockLightNull:
        return false;
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
        return true;
    default:
        throw std::runtime_error("CRChunkCodec: unknown blocklight type");
    }
}

class CRChunkCodec {
public:
    void setRegistry(const Voxel::BlockRegistry* registry) {
        m_registry = registry;
    }

    void setPolicies(PersistencePolicies policies) {
        m_policies = policies;
    }

    ChunkSnapshot read(ByteReader& reader, const ChunkKey& keyHint) {
        auto decoded = decodeRecord(reader, keyHint);
        decoded.chunk.opaquePayload = std::move(decoded.bytes);
        return std::move(decoded.chunk);
    }

    void write(const ChunkSnapshot& chunk, ByteWriter& writer) {
        const auto rigelCoord = toRigelChunk(chunk.key);
        const ChunkSpan& span = chunk.data.span;
        if (span.sizeX != 16 || span.sizeY != 16 || span.sizeZ != 16) {
            throw std::runtime_error("CRChunkCodec: chunk span size mismatch");
        }
        if (span.chunkX != rigelCoord.rigelChunkX ||
            span.chunkY != rigelCoord.rigelChunkY ||
            span.chunkZ != rigelCoord.rigelChunkZ ||
            span.offsetX != (rigelCoord.subchunkIndex & 1) * 16 ||
            span.offsetY != ((rigelCoord.subchunkIndex >> 1) & 1) * 16 ||
            span.offsetZ != ((rigelCoord.subchunkIndex >> 2) & 1) * 16) {
            throw std::runtime_error(
                "CRChunkCodec: chunk span does not match storage key");
        }
        if (!chunk.opaquePayload.empty()) {
            if (chunk.opaquePayload.size() > kMaxChunkRecordBytes) {
                throw std::runtime_error("CRChunkCodec: record exceeds format limit");
            }
            MemoryByteReader sourceReader(chunk.opaquePayload);
            auto source = decodeRecord(sourceReader, ChunkKey{chunk.key.zoneId, 0, 0, 0});
            if (sourceReader.tell() != sourceReader.size()) {
                throw std::runtime_error("CRChunkCodec: opaque record has trailing data");
            }
            if (source.chunk.key == chunk.key && source.chunk.data == chunk.data) {
                writer.writeBytes(chunk.opaquePayload.data(), chunk.opaquePayload.size());
                return;
            }
            if (source.hasUnsupportedPayload) {
                throw std::runtime_error(
                    "CRChunkCodec: modified record contains unsupported light or block-entity data");
            }
        }

        if (chunk.data.blocks.size() != 16 * 16 * 16) {
            throw std::runtime_error("CRChunkCodec: chunk block data size mismatch");
        }
        for (const auto& state : chunk.data.blocks) {
            if (state.metadata != 0 || state.lightLevel != 0) {
                throw std::runtime_error(
                    "CRChunkCodec: block metadata and light are not representable");
            }
        }
        writer.writeI32(chunk.key.x);
        writer.writeI32(chunk.key.y);
        writer.writeI32(chunk.key.z);
        writeBlockData(writer, chunk.data.blocks, m_registry, m_policies);
        writer.writeU8(static_cast<uint8_t>(kSkyNull));
        writer.writeU8(static_cast<uint8_t>(kBlockLightNull));
        writer.writeU8(static_cast<uint8_t>(kBlockEntityNull));
    }

private:
    struct DecodedRecord {
        ChunkSnapshot chunk;
        std::vector<uint8_t> bytes;
        bool hasUnsupportedPayload = false;
    };

    DecodedRecord decodeRecord(ByteReader& reader, const ChunkKey& keyHint) const {
        TrackingReader tracker(reader);
        DecodedRecord decoded;
        ChunkSnapshot& out = decoded.chunk;
        out.key = keyHint;
        out.key.x = tracker.readI32();
        out.key.y = tracker.readI32();
        out.key.z = tracker.readI32();
        auto rigelCoord = toRigelChunk(out.key);
        out.data.span.chunkX = rigelCoord.rigelChunkX;
        out.data.span.chunkY = rigelCoord.rigelChunkY;
        out.data.span.chunkZ = rigelCoord.rigelChunkZ;
        out.data.span.offsetX = (rigelCoord.subchunkIndex & 1) * 16;
        out.data.span.offsetY = ((rigelCoord.subchunkIndex >> 1) & 1) * 16;
        out.data.span.offsetZ = ((rigelCoord.subchunkIndex >> 2) & 1) * 16;
        out.data.span.sizeX = 16;
        out.data.span.sizeY = 16;
        out.data.span.sizeZ = 16;
        out.data.blocks = decodeBlocks(tracker, m_registry, m_policies);
        decoded.hasUnsupportedPayload = readSkylightData(tracker);
        decoded.hasUnsupportedPayload =
            readBlockLightData(tracker) || decoded.hasUnsupportedPayload;
        uint8_t entityFlag = tracker.readU8();
        if (entityFlag == kBlockEntityData) {
            decoded.hasUnsupportedPayload = true;
            int32_t size = tracker.readI32();
            if (size < 0) {
                throw std::runtime_error("CRChunkCodec: invalid block entity size");
            }
            if (static_cast<size_t>(size) > kMaxBlockEntityBytes) {
                throw std::runtime_error(
                    "CRChunkCodec: block entity size exceeds format limit");
            }
            if (static_cast<size_t>(size) > tracker.remaining()) {
                throw std::runtime_error(
                    "CRChunkCodec: block entity size exceeds column extent");
            }
            if (size != 0) {
                tracker.readBytes(static_cast<size_t>(size));
            }
        } else if (entityFlag != kBlockEntityNull) {
            throw std::runtime_error("CRChunkCodec: unknown block entity flag");
        }
        decoded.bytes = tracker.takeBytes();
        return decoded;
    }

    const Voxel::BlockRegistry* m_registry = nullptr;
    PersistencePolicies m_policies{};
};

class CRWorldMetadataCodec final : public WorldMetadataCodec {
public:
    std::string metadataPath(const PersistenceContext& context) const override {
        return CRPaths::worldInfoPath(context);
    }

    void write(const WorldMetadata& metadata, ByteWriter& writer) override {
        const std::string prefix =
            "{\n"
            "  \"latestRegionFileVersion\": " +
            std::to_string(kFileVersion) +
            ",\n"
            "  \"defaultZoneId\": \"rigel:default\",\n"
            "  \"worldDisplayName\": ";
        constexpr std::string_view suffix =
            ",\n"
            "  \"worldSeed\": 0,\n"
            "  \"worldCreatedEpochMillis\": 0,\n"
            "  \"lastPlayedEpochMillis\": 0,\n"
            "  \"worldTick\": 0\n"
            "}\n";
        const std::string text = encodeMetadataDocument(
            prefix, metadata.displayName, suffix);
        writer.writeBytes(reinterpret_cast<const uint8_t*>(text.data()), text.size());
    }

    WorldMetadata read(ByteReader& reader) override {
        const std::string text = readMetadataDocument(reader);
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
        detail::validateZoneIdentifier(metadata.zoneId);
        constexpr std::string_view prefix =
            "{\n"
            "  \"zoneId\": ";
        constexpr std::string_view suffix =
            ",\n"
            "  \"worldGenSaveKey\": \"rigel:default\",\n"
            "  \"seed\": 0,\n"
            "  \"respawnHeight\": 0,\n"
            "  \"spawnPoint\": {\"x\":0,\"y\":0,\"z\":0},\n"
            "  \"skyId\": \"rigel:default\"\n"
            "}\n";
        const std::string text = encodeMetadataDocument(
            prefix, metadata.zoneId, suffix);
        writer.writeBytes(reinterpret_cast<const uint8_t*>(text.data()), text.size());
    }

    ZoneMetadata read(ByteReader& reader) override {
        const std::string text = readMetadataDocument(reader);
        ZoneMetadata out;
        auto zoneId = extractJsonString(text, "zoneId");
        if (zoneId) {
            out.zoneId = *zoneId;
        }
        detail::validateZoneIdentifier(out.zoneId);
        out.displayName = out.zoneId;
        return out;
    }
};

struct CRColumnEnvelope {
    size_t tableIndex = 0;
    size_t offset = 0;
    size_t extent = 0;
    uint8_t chunkCount = 0;
};

int32_t readColumnOffset(ByteReader& reader, uint8_t offsetType) {
    switch (offsetType) {
    case 1:
        return static_cast<int8_t>(reader.readU8());
    case 2:
        return static_cast<int16_t>(reader.readU16());
    case 3:
        return reader.readI32();
    default:
        throw std::runtime_error("CRRegion: unsupported offset type");
    }
}

std::vector<CRColumnEnvelope> validateRegionPayload(
    ByteReader& reader,
    int32_t declaredColumnCount) {
    requireRemaining(reader, sizeof(uint8_t), "CRRegion: truncated offset type");
    const uint8_t offsetType = reader.readU8();
    if (offsetType < 1 || offsetType > 3) {
        throw std::runtime_error("CRRegion: unsupported offset type");
    }

    const size_t offsetWidth = static_cast<size_t>(offsetType == 3 ? 4 : offsetType);
    const size_t offsetTableBytes = kRegionColumnCount * offsetWidth;
    requireRemaining(
        reader, offsetTableBytes,
        "CRRegion: offset table exceeds payload");
    const size_t tableStart = reader.tell();
    const size_t columnsStart = tableStart + offsetTableBytes;
    const size_t columnPayloadBytes = reader.size() - columnsStart;

    std::vector<CRColumnEnvelope> columns;
    columns.reserve(static_cast<size_t>(declaredColumnCount));
    std::unordered_set<size_t> referencedOffsets;
    for (size_t index = 0; index < kRegionColumnCount; ++index) {
        const int32_t relativeOffset = readColumnOffset(reader, offsetType);
        if (relativeOffset == -1) {
            continue;
        }
        if (relativeOffset < 0) {
            throw std::runtime_error("CRRegion: invalid negative column offset");
        }
        const size_t relative = static_cast<size_t>(relativeOffset);
        if (relative > columnPayloadBytes ||
            kColumnHeaderBytes > columnPayloadBytes - relative) {
            throw std::runtime_error("CRRegion: column offset exceeds payload");
        }
        const size_t absoluteOffset = columnsStart + relative;
        if (!referencedOffsets.insert(absoluteOffset).second) {
            throw std::runtime_error("CRRegion: duplicate column offset");
        }
        columns.push_back(CRColumnEnvelope{index, absoluteOffset, 0, 0});
    }

    if (static_cast<size_t>(declaredColumnCount) != columns.size()) {
        throw std::runtime_error(
            "CRRegion: column count does not match offset table");
    }

    for (auto& column : columns) {
        reader.seek(column.offset);
        const int32_t declaredExtent = reader.readI32();
        if (declaredExtent < static_cast<int32_t>(kColumnHeaderBytes)) {
            throw std::runtime_error(
                "CRRegion: column extent is smaller than header");
        }
        column.extent = static_cast<size_t>(declaredExtent);
        if (column.extent > kMaxColumnBytes) {
            throw std::runtime_error("CRRegion: column extent exceeds format limit");
        }
        if (column.extent > reader.size() - column.offset) {
            throw std::runtime_error("CRRegion: column extent exceeds payload");
        }

        const int32_t columnVersion = reader.readI32();
        if (columnVersion != kFileVersion) {
            throw std::runtime_error("CRRegion: unsupported column version");
        }
        column.chunkCount = reader.readU8();
        if (column.chunkCount > kMaxChunksPerColumn) {
            throw std::runtime_error(
                "CRRegion: column chunk count exceeds format limit");
        }
        const size_t columnPayloadSize = column.extent - kColumnHeaderBytes;
        if (static_cast<size_t>(column.chunkCount) >
            columnPayloadSize / kMinChunkRecordBytes) {
            throw std::runtime_error(
                "CRRegion: column chunk count exceeds payload");
        }
    }

    std::vector<const CRColumnEnvelope*> sortedColumns;
    sortedColumns.reserve(columns.size());
    for (const auto& column : columns) {
        sortedColumns.push_back(&column);
    }
    std::sort(
        sortedColumns.begin(), sortedColumns.end(),
        [](const CRColumnEnvelope* lhs, const CRColumnEnvelope* rhs) {
            return lhs->offset < rhs->offset;
        });
    for (size_t i = 1; i < sortedColumns.size(); ++i) {
        const auto& previous = *sortedColumns[i - 1];
        const auto& current = *sortedColumns[i];
        if (current.offset < previous.offset + previous.extent) {
            throw std::runtime_error("CRRegion: overlapping column extents");
        }
    }

    return columns;
}

class CRChunkContainer final : public ChunkContainer {
public:
    CRChunkContainer(std::shared_ptr<StorageBackend> storage, PersistenceContext context, CRChunkCodec& codec)
        : m_storage(std::move(storage)), m_context(std::move(context)), m_codec(codec) {
    }

    bool regionExists(const RegionKey& key) override {
        return m_storage->exists(CRPaths::regionPath(key, m_context));
    }

    void saveRegion(const ChunkRegionSnapshot& region) override {
        auto path = CRPaths::regionPath(region.key, m_context);
        if (region.chunks.empty()) {
            m_storage->remove(path);
            return;
        }

        std::vector<std::vector<const ChunkSnapshot*>> columns(kRegionColumnCount);
        std::array<std::unordered_set<int32_t>, kRegionColumnCount> chunkYs;
        const int64_t baseX =
            static_cast<int64_t>(region.key.x) * static_cast<int64_t>(kRegionSpan);
        const int64_t baseY =
            static_cast<int64_t>(region.key.y) * static_cast<int64_t>(kRegionSpan);
        const int64_t baseZ =
            static_cast<int64_t>(region.key.z) * static_cast<int64_t>(kRegionSpan);
        for (const auto& chunk : region.chunks) {
            detail::validateZoneIdentifier(chunk.key.zoneId);
            if (chunk.key.zoneId != region.key.zoneId) {
                throw std::runtime_error("CRRegion: chunk zone does not match region");
            }
            const int64_t localX = static_cast<int64_t>(chunk.key.x) - baseX;
            const int64_t localY = static_cast<int64_t>(chunk.key.y) - baseY;
            const int64_t localZ = static_cast<int64_t>(chunk.key.z) - baseZ;
            if (localX < 0 || localX >= 16 || localZ < 0 || localZ >= 16 || localY < 0 || localY >= 16) {
                throw std::runtime_error("CRRegion: chunk lies outside its region");
            }
            const size_t index = static_cast<size_t>(localX + localZ * 16);
            if (!chunkYs[index].insert(chunk.key.y).second) {
                throw std::runtime_error("CRRegion: duplicate chunk coordinates");
            }
            columns[index].push_back(&chunk);
        }

        std::vector<int32_t> offsets(16 * 16, -1);
        std::vector<uint8_t> columnsBytes;
        VectorWriter columnsBuffer(columnsBytes);
        constexpr size_t minimumOffsetTableBytes =
            sizeof(uint8_t) + kRegionColumnCount * sizeof(uint16_t);
        BoundedWriter columnsWriter(
            columnsBuffer,
            kMaxDecompressedRegionBytes - minimumOffsetTableBytes,
            "CRRegion: region payload exceeds format limit");
        int columnsWritten = 0;

        for (int index = 0; index < 16 * 16; ++index) {
            auto& col = columns[index];
            if (col.empty()) {
                continue;
            }
            std::sort(col.begin(), col.end(), [](const ChunkSnapshot* a, const ChunkSnapshot* b) {
                return a->key.y < b->key.y;
            });
            if (col.size() > kMaxChunksPerColumn) {
                throw std::runtime_error(
                    "CRRegion: column chunk count exceeds format limit");
            }
            if (columnsWriter.size() >
                static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
                throw std::runtime_error("CRRegion: column offset exceeds format limit");
            }
            offsets[index] = static_cast<int32_t>(columnsWriter.size());
            ++columnsWritten;

            size_t columnStart = columnsWriter.tell();
            columnsWriter.writeI32(0);
            columnsWriter.writeI32(kFileVersion);
            size_t numChunksOffset = columnsWriter.tell();
            columnsWriter.writeU8(0);
            uint8_t numChunks = 0;

            for (const ChunkSnapshot* chunk : col) {
                BoundedWriter recordWriter(
                    columnsWriter,
                    kMaxChunkRecordBytes,
                    "CRChunkCodec: record exceeds format limit");
                m_codec.write(*chunk, recordWriter);
                ++numChunks;
            }

            size_t columnEnd = columnsWriter.tell();
            if (columnEnd - columnStart > kMaxColumnBytes) {
                throw std::runtime_error("CRRegion: column extent exceeds format limit");
            }
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
        if (payload.size() > kMaxDecompressedRegionBytes) {
            throw std::runtime_error("CRRegion: region payload exceeds format limit");
        }

        bool useCompression = false;
        if (m_context.providers) {
            auto settings = m_context.providers->findAs<CRPersistenceSettings>(kCRSettingsProviderId);
            if (settings) {
                useCompression = settings->enableLz4;
            }
        }

        std::vector<uint8_t> compressed;
        int compressedSize = 0;
        const int32_t decompressedSize = static_cast<int32_t>(payload.size());
        if (useCompression) {
            if (!CRLz4::available()) {
                throw std::runtime_error("CRRegion: LZ4 compression requested but unavailable");
            }
            const int bound = CRLz4::compressBound(decompressedSize);
            if (bound <= 0 || static_cast<size_t>(bound) > kMaxCompressedRegionBytes) {
                throw std::runtime_error("CRRegion: compressed size exceeds format limit");
            }
            compressed.resize(static_cast<size_t>(bound));
            compressedSize = CRLz4::compress(
                payload.data(), payload.size(), compressed.data(), compressed.size());
            if (compressedSize <= 0) {
                throw std::runtime_error("CRRegion: LZ4 compression failed");
            }
            compressed.resize(static_cast<size_t>(compressedSize));
        }

        auto session = m_storage->openWrite(path);
        auto& writer = session->writer();
        writer.writeI32(kMagic);
        writer.writeI32(kFileVersion);

        if (useCompression) {
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
        requireRemaining(*reader, 4 * sizeof(int32_t), "CRRegion: truncated file header");
        int32_t magic = reader->readI32();
        if (magic != kMagic) {
            throw std::runtime_error("CRRegion: invalid magic");
        }
        int32_t version = reader->readI32();
        if (version != kFileVersion) {
            throw std::runtime_error("CRRegion: unsupported file version");
        }
        int32_t compressionType = reader->readI32();
        int32_t declaredColumnCount = reader->readI32();
        if (declaredColumnCount < 0 ||
            declaredColumnCount > static_cast<int32_t>(kRegionColumnCount)) {
            throw std::runtime_error("CRRegion: invalid column count");
        }

        std::unique_ptr<ByteReader> payloadReader;
        if (compressionType == kCompressionLz4) {
            requireRemaining(
                *reader, 2 * sizeof(int32_t),
                "CRRegion: truncated compression header");
            int32_t compressedSize = reader->readI32();
            int32_t decompressedSize = reader->readI32();
            if (compressedSize <= 0 || decompressedSize <= 0) {
                throw std::runtime_error("CRRegion: invalid compressed sizes");
            }
            if (static_cast<size_t>(compressedSize) > kMaxCompressedRegionBytes) {
                throw std::runtime_error(
                    "CRRegion: compressed size exceeds format limit");
            }
            if (static_cast<size_t>(decompressedSize) > kMaxDecompressedRegionBytes) {
                throw std::runtime_error(
                    "CRRegion: decompressed size exceeds format limit");
            }
            if (static_cast<size_t>(compressedSize) > remainingInput(*reader)) {
                throw std::runtime_error(
                    "CRRegion: compressed size exceeds remaining input");
            }
            if (static_cast<size_t>(compressedSize) != remainingInput(*reader)) {
                throw std::runtime_error(
                    "CRRegion: compressed size does not consume input");
            }
            if (!CRLz4::available()) {
                throw std::runtime_error("CRRegion: LZ4 compression unavailable");
            }
            std::vector<uint8_t> compressed(static_cast<size_t>(compressedSize));
            reader->readBytes(compressed.data(), compressed.size());
            std::vector<uint8_t> decompressed(static_cast<size_t>(decompressedSize));
            int result = CRLz4::decompress(compressed.data(), compressed.size(), decompressed.data(), decompressed.size());
            if (result < 0) {
                throw std::runtime_error("CRRegion: LZ4 decompression failed");
            }
            if (result != decompressedSize) {
                throw std::runtime_error(
                    "CRRegion: decompressed size does not match declaration");
            }
            payloadReader = std::make_unique<MemoryByteReader>(std::move(decompressed));
        } else if (compressionType != kCompressionNone) {
            throw std::runtime_error("CRRegion: unknown compression type");
        } else if (remainingInput(*reader) > kMaxDecompressedRegionBytes) {
            throw std::runtime_error("CRRegion: region payload exceeds format limit");
        }

        ByteReader* dataReader = payloadReader ? payloadReader.get() : reader.get();
        auto columns = validateRegionPayload(*dataReader, declaredColumnCount);
        size_t totalChunkCount = 0;
        for (const auto& column : columns) {
            totalChunkCount += column.chunkCount;
        }
        region.chunks.reserve(totalChunkCount);

        ChunkKey hint{key.zoneId, 0, 0, 0};
        for (const auto& column : columns) {
            auto columnBytes = dataReader->readAt(
                column.offset + kColumnHeaderBytes,
                column.extent - kColumnHeaderBytes);
            MemoryByteReader columnReader(std::move(columnBytes));
            const int64_t expectedX =
                static_cast<int64_t>(key.x) * static_cast<int64_t>(kRegionSpan) +
                static_cast<int64_t>(column.tableIndex % kRegionSpan);
            const int64_t expectedZ =
                static_cast<int64_t>(key.z) * static_cast<int64_t>(kRegionSpan) +
                static_cast<int64_t>(column.tableIndex / kRegionSpan);
            const int64_t minimumY =
                static_cast<int64_t>(key.y) * static_cast<int64_t>(kRegionSpan);
            const int64_t maximumY = minimumY + static_cast<int64_t>(kRegionSpan);
            std::unordered_set<int32_t> chunkYs;

            for (uint8_t i = 0; i < column.chunkCount; ++i) {
                requireRemaining(
                    columnReader, kMinChunkRecordBytes,
                    "CRRegion: column chunk count exceeds payload");
                const size_t recordStart = columnReader.tell();
                const int32_t chunkX = columnReader.readI32();
                const int32_t chunkY = columnReader.readI32();
                const int32_t chunkZ = columnReader.readI32();
                if (static_cast<int64_t>(chunkX) != expectedX ||
                    static_cast<int64_t>(chunkZ) != expectedZ ||
                    static_cast<int64_t>(chunkY) < minimumY ||
                    static_cast<int64_t>(chunkY) >= maximumY) {
                    throw std::runtime_error(
                        "CRRegion: chunk coordinates do not match region column");
                }
                if (!chunkYs.insert(chunkY).second) {
                    throw std::runtime_error(
                        "CRRegion: duplicate chunk coordinates in column");
                }
                columnReader.seek(recordStart);
                region.chunks.push_back(m_codec.read(columnReader, hint));
            }
            if (columnReader.tell() != columnReader.size()) {
                throw std::runtime_error(
                    "CRRegion: column chunk count does not consume payload");
            }
        }
        return region;
    }

    std::vector<RegionKey> listRegions(const std::string& zoneId) override {
        std::vector<RegionKey> regions;
        std::string dir = CRPaths::zoneRoot(zoneId, m_context) + "/regions";
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
    CRChunkCodec& m_codec;
};

class CREntityContainer final : public EntityContainer {
public:
    CREntityContainer(std::shared_ptr<StorageBackend> storage, PersistenceContext context)
        : m_storage(std::move(storage)), m_context(std::move(context)) {
    }

    void saveRegion(const EntityRegionSnapshot& region) override {
        auto path = CRPaths::entityRegionPath(region.key, m_context);
        if (region.chunks.empty()) {
            if (m_storage->exists(path)) {
                m_storage->remove(path);
            }
            return;
        }
        validateRegionChunks(region);
        auto payload = Entity::encodeEntityRegionPayload(region.chunks);
        auto session = m_storage->openWrite(path);
        if (!payload.empty()) {
            session->writer().writeBytes(payload.data(), payload.size());
        }
        session->writer().flush();
        session->commit();
    }

    void removeRegion(const EntityRegionKey& key) override {
        auto path = CRPaths::entityRegionPath(key, m_context);
        m_storage->remove(path);
    }

    EntityRegionSnapshot loadRegion(const EntityRegionKey& key) override {
        EntityRegionSnapshot out;
        out.key = key;
        auto path = CRPaths::entityRegionPath(key, m_context);
        if (!m_storage->exists(path)) {
            return out;
        }
        auto reader = m_storage->openRead(path);
        if (reader->size() > Entity::detail::MaxEntityRegionBytes) {
            throw std::runtime_error(
                "CRFormat: entity region payload exceeds format limit");
        }
        std::vector<uint8_t> payload(reader->size());
        reader->seek(0);
        if (!payload.empty()) {
            reader->readBytes(payload.data(), payload.size());
        }
        std::vector<Entity::EntityPersistedChunk> chunks;
        if (!Entity::decodeEntityRegionPayload(payload, chunks)) {
            throw std::runtime_error("CRFormat: failed to decode entity region");
        }
        out.chunks = std::move(chunks);
        validateRegionChunks(out);
        return out;
    }

    std::vector<EntityRegionKey> listRegions(const std::string& zoneId) override {
        std::vector<EntityRegionKey> regions;
        std::string dir = CRPaths::zoneRoot(zoneId, m_context) + "/entities";
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
    static void validateRegionChunks(const EntityRegionSnapshot& region) {
        std::unordered_set<Voxel::ChunkCoord, Voxel::ChunkCoordHash> coordinates;
        for (const auto& chunk : region.chunks) {
            if (!coordinates.insert(chunk.coord).second) {
                throw std::runtime_error(
                    "CRFormat: duplicate entity chunk coordinates");
            }
            const Entity::PersistenceRegionCoord expected =
                Entity::persistenceRegionForChunk(chunk.coord);
            if (expected.x != region.key.x ||
                expected.y != region.key.y ||
                expected.z != region.key.z) {
                throw std::runtime_error(
                    "CRFormat: entity chunk lies outside its region");
            }
            for (const auto& entity : chunk.entities) {
                if (!Entity::detail::isPersistablePosition(entity.position)) {
                    throw std::runtime_error(
                        "CRFormat: invalid persistent entity position");
                }
                if (!Entity::detail::isFiniteVector(entity.velocity) ||
                    !Entity::detail::isFiniteVector(entity.viewDirection)) {
                    throw std::runtime_error(
                        "CRFormat: invalid persistent entity vector");
                }
            }
        }
    }

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
        m_chunkCodec.setPolicies(m_context.policies);
        if (m_context.providers) {
            auto provider = m_context.providers->findAs<BlockRegistryProvider>(kBlockRegistryProviderId);
            if (provider) {
                m_chunkCodec.setRegistry(provider->registry());
            }
        }
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

    RegionLayout& regionLayout() override {
        return m_layout;
    }

private:
    std::shared_ptr<StorageBackend> m_storage;
    PersistenceContext m_context;
    CRWorldMetadataCodec m_worldCodec;
    CRZoneMetadataCodec m_zoneCodec;
    CRRegionLayout m_layout;
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
        init.capabilities.fillMissingChunkSpans = false;
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

void requireSupportedDefaultZone(const PersistenceContext& context,
                                 const std::string& supportedZoneId) {
    detail::validateZoneIdentifier(supportedZoneId);
    if (!context.storage) {
        throw std::runtime_error("CR default-zone validation requires a storage backend");
    }

    const std::string path = CRPaths::worldInfoPath(context);
    if (!context.storage->exists(path)) {
        return;
    }

    auto reader = context.storage->openRead(path);
    const std::string text = readMetadataDocument(*reader);
    const auto defaultZoneId = extractJsonString(text, "defaultZoneId");
    if (!defaultZoneId || defaultZoneId->empty()) {
        throw std::runtime_error(
            "CR world metadata at '" + path +
            "' does not declare defaultZoneId; refusing to open this world "
            "because its persistence zone cannot be determined");
    }
    detail::validateZoneIdentifier(*defaultZoneId);
    if (*defaultZoneId != supportedZoneId) {
        throw std::runtime_error(
            "CR world metadata at '" + path + "' declares default zone '" +
            *defaultZoneId + "', but Rigel supports only '" + supportedZoneId +
            "'; refusing to open this world because alternate-zone persistence "
            "and async loading are not supported");
    }
}

} // namespace Rigel::Persistence::Backends::CR
