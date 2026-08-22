#include "Rigel/Persistence/ChunkSerializer.h"
#include "ChunkValidation.h"

#include <stdexcept>
#include <string>

namespace Rigel::Persistence {
namespace detail {

size_t validateChunkSpan(const ChunkSpan& span) {
    if (span.sizeX <= 0 || span.sizeY <= 0 || span.sizeZ <= 0) {
        throw std::runtime_error("ChunkSerializer: span size must be positive");
    }
    if (span.offsetX < 0 || span.offsetY < 0 || span.offsetZ < 0) {
        throw std::runtime_error("ChunkSerializer: span offset must be non-negative");
    }
    const auto isOutOfBounds = [](int32_t offset, int32_t size) {
        return offset > Voxel::Chunk::SIZE ||
            size > Voxel::Chunk::SIZE - offset;
    };
    if (isOutOfBounds(span.offsetX, span.sizeX) ||
        isOutOfBounds(span.offsetY, span.sizeY) ||
        isOutOfBounds(span.offsetZ, span.sizeZ)) {
        throw std::runtime_error("ChunkSerializer: span out of chunk bounds");
    }

    return static_cast<size_t>(span.sizeX) *
        static_cast<size_t>(span.sizeY) *
        static_cast<size_t>(span.sizeZ);
}

size_t validateChunkBlockCount(const ChunkSpan& span, size_t blockCount) {
    const size_t expected = validateChunkSpan(span);
    if (blockCount != expected) {
        throw std::runtime_error("ChunkSerializer: block data size mismatch");
    }
    return expected;
}

void validateChunkBlockIds(const ChunkData& data, const Voxel::BlockRegistry& registry) {
    for (const auto& block : data.blocks) {
        if (block.id.type >= registry.size()) {
            throw std::runtime_error(
                "ChunkSerializer: invalid block ID " +
                std::to_string(block.id.type));
        }
    }
}

void validateChunkData(const ChunkData& data, const Voxel::BlockRegistry& registry) {
    validateChunkBlockCount(data.span, data.blocks.size());
    validateChunkBlockIds(data, registry);
}

} // namespace detail

ChunkData serializeChunk(const Voxel::Chunk& chunk) {
    ChunkSpan span;
    span.chunkX = chunk.position().x;
    span.chunkY = chunk.position().y;
    span.chunkZ = chunk.position().z;
    span.sizeX = Voxel::Chunk::SIZE;
    span.sizeY = Voxel::Chunk::SIZE;
    span.sizeZ = Voxel::Chunk::SIZE;
    return serializeChunkSpan(chunk, span);
}

ChunkData serializeChunkSpan(const Voxel::Chunk& chunk, const ChunkSpan& span) {
    const size_t volume = detail::validateChunkSpan(span);

    ChunkData data;
    data.span = span;
    data.blocks.resize(volume);

    for (int z = 0; z < span.sizeZ; ++z) {
        for (int y = 0; y < span.sizeY; ++y) {
            for (int x = 0; x < span.sizeX; ++x) {
                size_t index = static_cast<size_t>(x +
                    z * span.sizeX +
                    y * span.sizeX * span.sizeZ);
                int localX = span.offsetX + x;
                int localY = span.offsetY + y;
                int localZ = span.offsetZ + z;
                data.blocks[index] = chunk.getBlock(localX, localY, localZ);
            }
        }
    }

    return data;
}

void applyChunkData(const ChunkData& data, Voxel::Chunk& chunk, const Voxel::BlockRegistry& registry) {
    const ChunkSpan& span = data.span;
    detail::validateChunkData(data, registry);

    for (int z = 0; z < span.sizeZ; ++z) {
        for (int y = 0; y < span.sizeY; ++y) {
            for (int x = 0; x < span.sizeX; ++x) {
                size_t index = static_cast<size_t>(x +
                    z * span.sizeX +
                    y * span.sizeX * span.sizeZ);
                int localX = span.offsetX + x;
                int localY = span.offsetY + y;
                int localZ = span.offsetZ + z;
                chunk.setBlock(localX, localY, localZ, data.blocks[index], registry);
            }
        }
    }
}

} // namespace Rigel::Persistence
