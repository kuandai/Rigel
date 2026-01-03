#include "Rigel/Persistence/Backends/CR/CRChunkData.h"

#include "Rigel/Persistence/Backends/CR/CRChunkMapping.h"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <unordered_map>

namespace Rigel::Persistence::Backends::CR {

namespace {

constexpr uint8_t kBlockNull = 0;
constexpr uint8_t kBlockSingle = 1;
constexpr uint8_t kBlockLayered = 2;

constexpr uint8_t kBlockLayerSingleByte = 1;
constexpr uint8_t kBlockLayerSingleInt = 2;
constexpr uint8_t kBlockLayerHalfNibble = 3;
constexpr uint8_t kBlockLayerNibble = 4;
constexpr uint8_t kBlockLayerByte = 5;
constexpr uint8_t kBlockLayerShort = 6;
constexpr uint8_t kBlockLayerBit = 7;

constexpr uint8_t kSkyNull = 1;
constexpr uint8_t kBlockLightNull = 1;
constexpr uint8_t kBlockEntityNull = 0;

struct BufferWriter {
    std::vector<uint8_t> data;

    void writeU8(uint8_t value) {
        data.push_back(value);
    }

    void writeU16(uint16_t value) {
        data.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
        data.push_back(static_cast<uint8_t>(value & 0xFF));
    }

    void writeI32(int32_t value) {
        data.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
        data.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
        data.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
        data.push_back(static_cast<uint8_t>(value & 0xFF));
    }

    void writeBytes(const uint8_t* src, size_t len) {
        if (len == 0) {
            return;
        }
        data.insert(data.end(), src, src + len);
    }

    void writeString(const std::string& value) {
        writeI32(static_cast<int32_t>(value.size()));
        if (!value.empty()) {
            writeBytes(reinterpret_cast<const uint8_t*>(value.data()), value.size());
        }
    }
};

class BufferReader {
public:
    explicit BufferReader(const std::vector<uint8_t>& data)
        : m_data(data) {
    }

    uint8_t readU8() {
        ensure(1);
        return m_data[m_pos++];
    }

    uint16_t readU16() {
        uint16_t value = 0;
        value |= static_cast<uint16_t>(readU8()) << 8;
        value |= static_cast<uint16_t>(readU8());
        return value;
    }

    int32_t readI32() {
        uint32_t value = 0;
        value |= static_cast<uint32_t>(readU8()) << 24;
        value |= static_cast<uint32_t>(readU8()) << 16;
        value |= static_cast<uint32_t>(readU8()) << 8;
        value |= static_cast<uint32_t>(readU8());
        return static_cast<int32_t>(value);
    }

    void readBytes(uint8_t* dst, size_t len) {
        ensure(len);
        if (len == 0) {
            return;
        }
        std::copy_n(m_data.data() + m_pos, len, dst);
        m_pos += len;
    }

    std::string readString() {
        int32_t len = readI32();
        if (len <= 0) {
            return std::string();
        }
        std::string out;
        out.resize(static_cast<size_t>(len));
        readBytes(reinterpret_cast<uint8_t*>(&out[0]), static_cast<size_t>(len));
        return out;
    }

private:
    void ensure(size_t len) {
        if (m_pos + len > m_data.size()) {
            throw std::runtime_error("CRChunkData read out of range");
        }
    }

    const std::vector<uint8_t>& m_data;
    size_t m_pos = 0;
};

std::vector<std::string> buildPalette(const std::vector<Voxel::BlockState>& blocks,
                                     const Voxel::BlockRegistry& registry,
                                     std::unordered_map<uint16_t, uint16_t>& paletteIndex) {
    std::vector<std::string> palette;
    for (const auto& state : blocks) {
        uint16_t id = state.id.type;
        if (paletteIndex.find(id) != paletteIndex.end()) {
            continue;
        }
        uint16_t index = static_cast<uint16_t>(palette.size());
        paletteIndex[id] = index;
        palette.push_back(registry.getType(Voxel::BlockID{id}).identifier);
    }
    return palette;
}

void writeLayer(BufferWriter& writer,
                const std::array<uint16_t, 256>& indices,
                uint16_t paletteSize) {
    bool uniform = true;
    uint16_t first = indices[0];
    for (size_t i = 1; i < indices.size(); ++i) {
        if (indices[i] != first) {
            uniform = false;
            break;
        }
    }

    if (uniform) {
        if (paletteSize <= 0xFF) {
            writer.writeU8(kBlockLayerSingleByte);
            writer.writeU8(static_cast<uint8_t>(first));
        } else {
            writer.writeU8(kBlockLayerSingleInt);
            writer.writeI32(static_cast<int32_t>(first));
        }
        return;
    }

    if (paletteSize <= 0xFF) {
        writer.writeU8(kBlockLayerByte);
        for (uint16_t value : indices) {
            writer.writeU8(static_cast<uint8_t>(value));
        }
    } else {
        writer.writeU8(kBlockLayerShort);
        for (uint16_t value : indices) {
            writer.writeU16(value);
        }
    }
}

void readLayer(BufferReader& reader, uint8_t layerType, std::array<uint16_t, 256>& indices) {
    switch (layerType) {
    case kBlockLayerSingleByte: {
        uint8_t value = reader.readU8();
        indices.fill(value);
        return;
    }
    case kBlockLayerSingleInt: {
        int32_t value = reader.readI32();
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
                uint16_t value = 0;
                switch (mod) {
                case 0: value = b & 0x03; break;
                case 1: value = (b & 0x0C) >> 2; break;
                case 2: value = (b & 0x30) >> 4; break;
                case 3: value = (b & 0xC0) >> 6; break;
                }
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
        throw std::runtime_error("CRChunkData: unknown block layer type");
    }
}

} // namespace

std::vector<ChunkSnapshot> encodeRigelChunk(const Voxel::Chunk& chunk,
                                            const Voxel::BlockRegistry& registry,
                                            const Voxel::ChunkCoord& coord,
                                            const std::string& zoneId) {
    std::vector<ChunkSnapshot> out;

    for (int subchunkIndex = 0; subchunkIndex < 8; ++subchunkIndex) {
        std::vector<Voxel::BlockState> blocks;
        blocks.resize(16 * 16 * 16);
        bool hasSolid = false;

        for (int y = 0; y < 16; ++y) {
            for (int z = 0; z < 16; ++z) {
                for (int x = 0; x < 16; ++x) {
                    auto local = toRigelLocal(x, y, z, subchunkIndex);
                    Voxel::BlockState state = chunk.getBlock(local.x, local.y, local.z);
                    blocks[static_cast<size_t>(x + z * 16 + y * 256)] = state;
                    if (!state.isAir()) {
                        hasSolid = true;
                    }
                }
            }
        }

        if (!hasSolid) {
            continue;
        }

        ChunkKey crKey = toCRChunk({coord.x, coord.y, coord.z, subchunkIndex});
        crKey.zoneId = zoneId;

        std::unordered_map<uint16_t, uint16_t> paletteIndex;
        auto palette = buildPalette(blocks, registry, paletteIndex);

        BufferWriter writer;
        writer.writeI32(crKey.x);
        writer.writeI32(crKey.y);
        writer.writeI32(crKey.z);

        if (palette.size() == 1) {
            writer.writeU8(kBlockSingle);
            writer.writeString(palette[0]);
        } else {
            writer.writeU8(kBlockLayered);
            writer.writeI32(static_cast<int32_t>(palette.size()));
            for (const auto& key : palette) {
                writer.writeString(key);
            }

            for (int layer = 0; layer < 16; ++layer) {
                std::array<uint16_t, 256> indices{};
                for (int z = 0; z < 16; ++z) {
                    for (int x = 0; x < 16; ++x) {
                        size_t index = static_cast<size_t>(x + z * 16 + layer * 256);
                        uint16_t blockId = blocks[index].id.type;
                        auto it = paletteIndex.find(blockId);
                        uint16_t paletteId = (it == paletteIndex.end()) ? 0 : it->second;
                        indices[static_cast<size_t>(x + z * 16)] = paletteId;
                    }
                }
                writeLayer(writer, indices, static_cast<uint16_t>(palette.size()));
            }
        }

        writer.writeU8(kSkyNull);
        writer.writeU8(kBlockLightNull);
        writer.writeU8(kBlockEntityNull);

        ChunkSnapshot snapshot;
        snapshot.key = crKey;
        snapshot.payload = std::move(writer.data);
        out.push_back(std::move(snapshot));
    }

    return out;
}

void decodeChunkSnapshot(const ChunkSnapshot& snapshot,
                         Voxel::ChunkManager& manager,
                         const Voxel::BlockRegistry& registry,
                         std::optional<uint32_t> worldGenVersion,
                         bool markPersistClean) {
    BufferReader reader(snapshot.payload);
    ChunkKey key = snapshot.key;
    key.x = reader.readI32();
    key.y = reader.readI32();
    key.z = reader.readI32();

    auto rigelCoord = toRigelChunk(key);
    Voxel::ChunkCoord coord{rigelCoord.rigelChunkX, rigelCoord.rigelChunkY, rigelCoord.rigelChunkZ};
    Voxel::Chunk& chunk = manager.getOrCreateChunk(coord);
    if (worldGenVersion) {
        chunk.setWorldGenVersion(*worldGenVersion);
    }

    uint8_t blockType = reader.readU8();
    std::vector<Voxel::BlockID> paletteIds;

    if (blockType == kBlockNull) {
        return;
    }

    if (blockType == kBlockSingle) {
        std::string keyString = reader.readString();
        auto blockId = registry.findByIdentifier(keyString).value_or(Voxel::BlockRegistry::airId());
        paletteIds.push_back(blockId);
        std::array<uint16_t, 256> indices{};
        indices.fill(0);
        for (int layer = 0; layer < 16; ++layer) {
            for (int z = 0; z < 16; ++z) {
                for (int x = 0; x < 16; ++x) {
                    auto local = toRigelLocal(x, layer, z, rigelCoord.subchunkIndex);
                    chunk.setBlock(local.x, local.y, local.z, Voxel::BlockState{blockId}, registry);
                }
            }
        }
    } else if (blockType == kBlockLayered) {
        int32_t paletteSize = reader.readI32();
        paletteIds.reserve(static_cast<size_t>(paletteSize));
        for (int32_t i = 0; i < paletteSize; ++i) {
            std::string id = reader.readString();
            auto blockId = registry.findByIdentifier(id).value_or(Voxel::BlockRegistry::airId());
            paletteIds.push_back(blockId);
        }

        for (int layer = 0; layer < 16; ++layer) {
            uint8_t layerType = reader.readU8();
            std::array<uint16_t, 256> indices{};
            readLayer(reader, layerType, indices);
            for (int z = 0; z < 16; ++z) {
                for (int x = 0; x < 16; ++x) {
                    uint16_t paletteIndex = indices[static_cast<size_t>(x + z * 16)];
                    Voxel::BlockID blockId = Voxel::BlockRegistry::airId();
                    if (paletteIndex < paletteIds.size()) {
                        blockId = paletteIds[paletteIndex];
                    }
                    auto local = toRigelLocal(x, layer, z, rigelCoord.subchunkIndex);
                    chunk.setBlock(local.x, local.y, local.z, Voxel::BlockState{blockId}, registry);
                }
            }
        }
    } else {
        throw std::runtime_error("CRChunkData: unknown block data type");
    }

    uint8_t skylightType = reader.readU8();
    if (skylightType == 3) {
        reader.readU8();
    } else if (skylightType == 2) {
        for (int layer = 0; layer < 16; ++layer) {
            uint8_t layerType = reader.readU8();
            if (layerType == 1) {
                reader.readU8();
            } else if (layerType == 2) {
                std::array<uint8_t, 128> bytes{};
                reader.readBytes(bytes.data(), bytes.size());
            }
        }
    }

    uint8_t blockLightType = reader.readU8();
    if (blockLightType == 2) {
        for (int layer = 0; layer < 16; ++layer) {
            uint8_t layerType = reader.readU8();
            if (layerType == 1) {
                reader.readU8();
                reader.readU8();
                reader.readU8();
            } else if (layerType == 2) {
                std::array<uint8_t, 512> bytes{};
                reader.readBytes(bytes.data(), bytes.size());
            } else if (layerType == 3 || layerType == 4 || layerType == 5) {
                reader.readU8();
                reader.readU8();
                reader.readU8();
                std::array<uint8_t, 128> bytes{};
                reader.readBytes(bytes.data(), bytes.size());
            }
        }
    }

    uint8_t blockEntityFlag = reader.readU8();
    if (blockEntityFlag == 1) {
        int32_t size = reader.readI32();
        if (size > 0) {
            std::vector<uint8_t> payload(static_cast<size_t>(size));
            reader.readBytes(payload.data(), payload.size());
        }
    }

    if (markPersistClean) {
        chunk.clearPersistDirty();
    }
}

} // namespace Rigel::Persistence::Backends::CR
