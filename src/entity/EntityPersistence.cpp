#include "Rigel/Entity/EntityPersistence.h"
#include "EntityPersistenceDetail.h"
#include "EntityPersistenceLimits.h"

#include <bit>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace Rigel::Entity {

namespace {
constexpr int kPersistenceRegionChunkSpan = 16;
constexpr uint32_t kEntityRegionMagic = 0x52474531; // "RGE1"
constexpr uint16_t kEntityRegionVersion = 1;

int floorDiv(int value, int divisor) {
    int quotient = value / divisor;
    if (value % divisor < 0) {
        --quotient;
    }
    return quotient;
}

class BufferWriter {
public:
    explicit BufferWriter(size_t encodedBytes) {
        m_data.reserve(encodedBytes);
    }

    void writeU8(uint8_t value) {
        requireSpace(1);
        m_data.push_back(value);
    }

    void writeU16(uint16_t value) {
        requireSpace(2);
        m_data.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
        m_data.push_back(static_cast<uint8_t>(value & 0xFF));
    }

    void writeU32(uint32_t value) {
        requireSpace(4);
        m_data.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
        m_data.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
        m_data.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
        m_data.push_back(static_cast<uint8_t>(value & 0xFF));
    }

    void writeU64(uint64_t value) {
        writeU32(static_cast<uint32_t>((value >> 32) & 0xFFFFFFFFu));
        writeU32(static_cast<uint32_t>(value & 0xFFFFFFFFu));
    }

    void writeF32(float value) {
        uint32_t bits = std::bit_cast<uint32_t>(value);
        writeU32(bits);
    }

    void writeString(const std::string& value) {
        if (value.size() > detail::MaxEntityStringBytes) {
            throw std::runtime_error("Entity persistence string is too large");
        }
        if (value.size() > detail::MaxEntityRegionBytes - sizeof(uint32_t)) {
            throw std::runtime_error("Entity region payload is too large");
        }
        requireSpace(sizeof(uint32_t) + value.size());
        writeU32(static_cast<uint32_t>(value.size()));
        if (!value.empty()) {
            m_data.insert(m_data.end(),
                          reinterpret_cast<const uint8_t*>(value.data()),
                          reinterpret_cast<const uint8_t*>(value.data()) + value.size());
        }
    }

    std::vector<uint8_t> take() {
        return std::move(m_data);
    }

private:
    void requireSpace(size_t bytes) const {
        if (m_data.size() > detail::MaxEntityRegionBytes ||
            bytes > detail::MaxEntityRegionBytes - m_data.size()) {
            throw std::runtime_error("Entity region payload is too large");
        }
    }

    std::vector<uint8_t> m_data;
};

class BufferReader {
public:
    explicit BufferReader(std::span<const uint8_t> data)
        : m_data(data) {}

    bool readU8(uint8_t& value) {
        if (!ensure(1)) {
            return false;
        }
        value = m_data[m_pos++];
        return true;
    }

    bool readU16(uint16_t& value) {
        if (!ensure(2)) {
            return false;
        }
        value = (static_cast<uint16_t>(m_data[m_pos]) << 8) |
                static_cast<uint16_t>(m_data[m_pos + 1]);
        m_pos += 2;
        return true;
    }

    bool readU32(uint32_t& value) {
        if (!ensure(4)) {
            return false;
        }
        value = (static_cast<uint32_t>(m_data[m_pos]) << 24) |
                (static_cast<uint32_t>(m_data[m_pos + 1]) << 16) |
                (static_cast<uint32_t>(m_data[m_pos + 2]) << 8) |
                static_cast<uint32_t>(m_data[m_pos + 3]);
        m_pos += 4;
        return true;
    }

    bool readU64(uint64_t& value) {
        uint32_t hi = 0;
        uint32_t lo = 0;
        if (!readU32(hi) || !readU32(lo)) {
            return false;
        }
        value = (static_cast<uint64_t>(hi) << 32) | static_cast<uint64_t>(lo);
        return true;
    }

    bool readF32(float& value) {
        uint32_t bits = 0;
        if (!readU32(bits)) {
            return false;
        }
        value = std::bit_cast<float>(bits);
        return true;
    }

    bool readString(std::string& value) {
        uint32_t len = 0;
        if (!readU32(len)) {
            return false;
        }
        if (len > detail::MaxEntityStringBytes) {
            return false;
        }
        if (!ensure(len)) {
            return false;
        }
        value.assign(reinterpret_cast<const char*>(m_data.data() + m_pos), len);
        m_pos += len;
        return true;
    }

    bool skipString(size_t& length) {
        uint32_t encodedLength = 0;
        if (!readU32(encodedLength) ||
            encodedLength > detail::MaxEntityStringBytes ||
            !ensure(encodedLength)) {
            return false;
        }
        length = encodedLength;
        m_pos += encodedLength;
        return true;
    }

    bool skip(size_t length) {
        if (!ensure(length)) {
            return false;
        }
        m_pos += length;
        return true;
    }

    bool atEnd() const {
        return m_pos == m_data.size();
    }

    size_t remaining() const {
        return m_pos <= m_data.size() ? m_data.size() - m_pos : 0;
    }

private:
    bool ensure(size_t len) const {
        return m_pos <= m_data.size() && len <= m_data.size() - m_pos;
    }

    std::span<const uint8_t> m_data;
    size_t m_pos = 0;
};

void writeVec3(BufferWriter& writer, const glm::vec3& value) {
    writer.writeF32(value.x);
    writer.writeF32(value.y);
    writer.writeF32(value.z);
}

bool readVec3(BufferReader& reader, glm::vec3& value) {
    return reader.readF32(value.x) && reader.readF32(value.y) && reader.readF32(value.z);
}

} // namespace

namespace detail {

EntityRegionPayloadInfo measureEntityRegionPayload(
    const std::vector<EntityPersistedChunk>& chunks) {
    if (chunks.size() > MaxChunksPerEntityRegion ||
        chunks.size() > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error("Entity region has too many chunks");
    }

    EntityRegionPayloadInfo info;
    info.encodedBytes = 3 * sizeof(uint32_t);
    info.chunks = chunks.size();
    auto addEncodedBytes = [&](size_t bytes) {
        if (info.encodedBytes > MaxEntityRegionBytes ||
            bytes > MaxEntityRegionBytes - info.encodedBytes) {
            throw std::runtime_error("Entity region payload is too large");
        }
        info.encodedBytes += bytes;
    };
    auto addString = [&](const std::string& value) {
        if (value.size() > MaxEntityStringBytes ||
            value.size() > std::numeric_limits<uint32_t>::max()) {
            throw std::runtime_error("Entity persistence string is too large");
        }
        addEncodedBytes(sizeof(uint32_t));
        addEncodedBytes(value.size());
        if (value.size() > std::numeric_limits<size_t>::max() -
                               info.stringBytes) {
            throw std::runtime_error("Entity region payload is too large");
        }
        info.stringBytes += value.size();
    };

    for (const auto& chunk : chunks) {
        if (chunk.entities.size() > MaxEntitiesPerChunk ||
            chunk.entities.size() > std::numeric_limits<uint32_t>::max()) {
            throw std::runtime_error("Entity chunk has too many entities");
        }
        addEncodedBytes(MinEncodedChunkBytes);
        if (chunk.entities.size() >
            std::numeric_limits<size_t>::max() - info.entities) {
            throw std::runtime_error("Entity region payload is too large");
        }
        info.entities += chunk.entities.size();
        for (const auto& entity : chunk.entities) {
            addString(entity.typeId);
            addEncodedBytes(MinEncodedEntityBytes - 2 * sizeof(uint32_t));
            addString(entity.modelId);
        }
    }
    return info;
}

EntityRegionPayloadInspection inspectEntityRegionPayload(
    std::span<const uint8_t> payload,
    size_t maxChunks,
    size_t maxEntities,
    EntityRegionPayloadInfo& info) {
    info = {};
    if (payload.size() > MaxEntityRegionBytes) {
        return EntityRegionPayloadInspection::Invalid;
    }

    BufferReader reader(payload);
    uint32_t magic = 0;
    uint16_t version = 0;
    uint16_t reserved = 0;
    if (!reader.readU32(magic) || magic != kEntityRegionMagic ||
        !reader.readU16(version) || version != kEntityRegionVersion ||
        !reader.readU16(reserved) || reserved != 0) {
        return EntityRegionPayloadInspection::Invalid;
    }

    uint32_t chunkCount = 0;
    if (!reader.readU32(chunkCount) ||
        chunkCount > MaxChunksPerEntityRegion) {
        return EntityRegionPayloadInspection::Invalid;
    }
    if (chunkCount > maxChunks) {
        return EntityRegionPayloadInspection::ChunkLimitExceeded;
    }
    if (chunkCount > reader.remaining() / MinEncodedChunkBytes) {
        return EntityRegionPayloadInspection::Invalid;
    }
    info.encodedBytes = payload.size();
    info.chunks = chunkCount;

    for (uint32_t i = 0; i < chunkCount; ++i) {
        if (!reader.skip(3 * sizeof(uint32_t))) {
            return EntityRegionPayloadInspection::Invalid;
        }
        uint32_t entityCount = 0;
        if (!reader.readU32(entityCount) ||
            entityCount > MaxEntitiesPerChunk) {
            return EntityRegionPayloadInspection::Invalid;
        }
        if (info.entities > maxEntities ||
            entityCount > maxEntities - info.entities) {
            return EntityRegionPayloadInspection::EntityLimitExceeded;
        }
        if (entityCount > reader.remaining() / MinEncodedEntityBytes) {
            return EntityRegionPayloadInspection::Invalid;
        }
        info.entities += entityCount;

        for (uint32_t e = 0; e < entityCount; ++e) {
            size_t stringBytes = 0;
            if (!reader.skipString(stringBytes)) {
                return EntityRegionPayloadInspection::Invalid;
            }
            if (stringBytes > std::numeric_limits<size_t>::max() -
                                  info.stringBytes) {
                return EntityRegionPayloadInspection::Invalid;
            }
            info.stringBytes += stringBytes;
            constexpr size_t kEncodedEntityFixedBytes =
                2 * sizeof(uint32_t) + sizeof(uint64_t) +
                9 * sizeof(uint32_t);
            if (!reader.skip(kEncodedEntityFixedBytes) ||
                !reader.skipString(stringBytes)) {
                return EntityRegionPayloadInspection::Invalid;
            }
            if (stringBytes > std::numeric_limits<size_t>::max() -
                                  info.stringBytes) {
                return EntityRegionPayloadInspection::Invalid;
            }
            info.stringBytes += stringBytes;
        }
    }

    return reader.atEnd()
        ? EntityRegionPayloadInspection::Valid
        : EntityRegionPayloadInspection::Invalid;
}

} // namespace detail

PersistenceRegionCoord persistenceRegionForChunk(Voxel::ChunkCoord coord) {
    return PersistenceRegionCoord{
        floorDiv(coord.x, kPersistenceRegionChunkSpan),
        floorDiv(coord.y, kPersistenceRegionChunkSpan),
        floorDiv(coord.z, kPersistenceRegionChunkSpan)};
}

std::vector<uint8_t> encodeEntityRegionPayload(
    const std::vector<EntityPersistedChunk>& chunks) {
    const detail::EntityRegionPayloadInfo info =
        detail::measureEntityRegionPayload(chunks);
    BufferWriter writer(info.encodedBytes);
    writer.writeU32(kEntityRegionMagic);
    writer.writeU16(kEntityRegionVersion);
    writer.writeU16(0);
    writer.writeU32(static_cast<uint32_t>(chunks.size()));

    for (const auto& chunk : chunks) {
        if (chunk.entities.size() > detail::MaxEntitiesPerChunk ||
            chunk.entities.size() > std::numeric_limits<uint32_t>::max()) {
            throw std::runtime_error("Entity chunk has too many entities");
        }
        writer.writeU32(static_cast<uint32_t>(chunk.coord.x));
        writer.writeU32(static_cast<uint32_t>(chunk.coord.y));
        writer.writeU32(static_cast<uint32_t>(chunk.coord.z));
        writer.writeU32(static_cast<uint32_t>(chunk.entities.size()));

        for (const auto& entity : chunk.entities) {
            writer.writeString(entity.typeId);
            writer.writeU64(entity.id.time);
            writer.writeU32(entity.id.random);
            writer.writeU32(entity.id.counter);
            writeVec3(writer, entity.position);
            writeVec3(writer, entity.velocity);
            writeVec3(writer, entity.viewDirection);
            writer.writeString(entity.modelId);
        }
    }

    return writer.take();
}

bool decodeEntityRegionPayload(std::span<const uint8_t> payload,
                               std::vector<EntityPersistedChunk>& outChunks) {
    detail::EntityRegionPayloadInfo info;
    if (detail::inspectEntityRegionPayload(
            payload,
            detail::MaxChunksPerEntityRegion,
            std::numeric_limits<size_t>::max(),
            info) != detail::EntityRegionPayloadInspection::Valid) {
        return false;
    }
    BufferReader reader(payload);
    uint32_t magic = 0;
    uint16_t version = 0;
    uint16_t reserved = 0;
    if (!reader.readU32(magic) || magic != kEntityRegionMagic) {
        return false;
    }
    if (!reader.readU16(version) || version != kEntityRegionVersion) {
        return false;
    }
    if (!reader.readU16(reserved) || reserved != 0) {
        return false;
    }

    uint32_t chunkCount = 0;
    if (!reader.readU32(chunkCount)) {
        return false;
    }
    std::vector<EntityPersistedChunk> chunks;
    chunks.reserve(info.chunks);

    for (uint32_t i = 0; i < chunkCount; ++i) {
        EntityPersistedChunk chunk;
        uint32_t cx = 0;
        uint32_t cy = 0;
        uint32_t cz = 0;
        if (!reader.readU32(cx) || !reader.readU32(cy) || !reader.readU32(cz)) {
            return false;
        }
        chunk.coord.x = static_cast<int32_t>(cx);
        chunk.coord.y = static_cast<int32_t>(cy);
        chunk.coord.z = static_cast<int32_t>(cz);

        uint32_t entityCount = 0;
        if (!reader.readU32(entityCount)) {
            return false;
        }
        chunk.entities.reserve(entityCount);
        for (uint32_t e = 0; e < entityCount; ++e) {
            EntityPersistedEntity entity;
            if (!reader.readString(entity.typeId)) {
                return false;
            }
            if (!reader.readU64(entity.id.time) ||
                !reader.readU32(entity.id.random) ||
                !reader.readU32(entity.id.counter)) {
                return false;
            }
            if (!readVec3(reader, entity.position) ||
                !readVec3(reader, entity.velocity) ||
                !readVec3(reader, entity.viewDirection)) {
                return false;
            }
            if (!reader.readString(entity.modelId)) {
                return false;
            }
            chunk.entities.push_back(std::move(entity));
        }

        chunks.push_back(std::move(chunk));
    }

    if (!reader.atEnd()) {
        return false;
    }
    outChunks = std::move(chunks);
    return true;
}

} // namespace Rigel::Entity
