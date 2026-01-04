#include "Rigel/Entity/EntityPersistence.h"

#include <bit>
#include <cstring>

namespace Rigel::Entity {

namespace {
constexpr uint32_t kEntityRegionMagic = 0x52474531; // "RGE1"
constexpr uint16_t kEntityRegionVersion = 1;

class BufferWriter {
public:
    void writeU8(uint8_t value) {
        m_data.push_back(value);
    }

    void writeU16(uint16_t value) {
        m_data.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
        m_data.push_back(static_cast<uint8_t>(value & 0xFF));
    }

    void writeU32(uint32_t value) {
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
        if (!ensure(len)) {
            return false;
        }
        value.assign(reinterpret_cast<const char*>(m_data.data() + m_pos), len);
        m_pos += len;
        return true;
    }

private:
    bool ensure(size_t len) const {
        return m_pos + len <= m_data.size();
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

std::vector<uint8_t> encodeEntityRegionPayload(
    const std::vector<EntityPersistedChunk>& chunks) {
    BufferWriter writer;
    writer.writeU32(kEntityRegionMagic);
    writer.writeU16(kEntityRegionVersion);
    writer.writeU16(0);
    writer.writeU32(static_cast<uint32_t>(chunks.size()));

    for (const auto& chunk : chunks) {
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
    if (!reader.readU16(reserved)) {
        return false;
    }

    uint32_t chunkCount = 0;
    if (!reader.readU32(chunkCount)) {
        return false;
    }

    outChunks.clear();
    outChunks.reserve(chunkCount);

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

        outChunks.push_back(std::move(chunk));
    }

    return true;
}

} // namespace Rigel::Entity
