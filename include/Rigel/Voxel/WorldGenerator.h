#pragma once

#include "Block.h"
#include "BlockRegistry.h"
#include "Chunk.h"
#include "ChunkCoord.h"
#include "DensityFunction.h"
#include "GeneratorDefinition.h"

#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace Rigel::Voxel {

class BlockGalleryChunkGenerator;

struct ChunkBuffer {
    std::array<BlockState, Chunk::VOLUME> blocks{};

    BlockState& at(int x, int y, int z) {
        return blocks[x + y * Chunk::SIZE + z * Chunk::SIZE * Chunk::SIZE];
    }
    const BlockState& at(int x, int y, int z) const {
        return blocks[x + y * Chunk::SIZE + z * Chunk::SIZE * Chunk::SIZE];
    }
};

struct ClimateSample {
    float temperature = 0.0f;
    float humidity = 0.0f;
    float continentalness = 0.0f;
};

struct BiomeSample {
    int primary = -1;
    int secondary = -1;
    float blend = 0.0f;
};

struct WorldGenContext {
    ChunkCoord coord;
    const GeneratorDefinitionData* definition = nullptr;
    BlockID solidBlock = BlockRegistry::airId();
    BlockID waterBlock = BlockRegistry::airId();
    std::array<int, Chunk::SIZE * Chunk::SIZE> heightMap{};
    std::array<ClimateSample, Chunk::SIZE * Chunk::SIZE> climate{};
    std::array<BiomeSample, Chunk::SIZE * Chunk::SIZE> biomes{};
    const std::atomic_bool* cancel = nullptr;

    bool shouldCancel() const {
        return cancel && cancel->load(std::memory_order_relaxed);
    }

};

class WorldGenStage {
public:
    virtual ~WorldGenStage() = default;
    virtual const char* name() const = 0;
    virtual void apply(WorldGenContext& ctx, ChunkBuffer& buffer) const = 0;
};

class WorldGenerator {
public:
    WorldGenerator(const BlockRegistry& registry,
                   GeneratorDefinitionData definition,
                   uint32_t seed,
                   uint32_t semanticsVersion = kGeneratorSemanticsVersion,
                   std::shared_ptr<const BlockGalleryChunkGenerator>
                       blockGallery = {});

    const GeneratorDefinitionData& definition() const { return m_definition; }
    uint32_t seed() const { return m_seed; }
    uint32_t semanticsVersion() const { return m_semanticsVersion; }
    bool matchesGenerationInputs(
        const GeneratorDefinitionData& definition,
        uint32_t seed,
        uint32_t semanticsVersion) const;
    bool shouldPersistGeneratedChunk(ChunkCoord coord) const;

    void generate(ChunkCoord coord, ChunkBuffer& out,
                  const std::atomic_bool* cancel = nullptr) const;

private:
    const BlockRegistry& m_registry;
    const GeneratorDefinitionData m_definition;
    const uint32_t m_seed;
    const uint32_t m_semanticsVersion;
    const BlockID m_solidBlock;
    const BlockID m_waterBlock;
    const DensityGraph m_densityGraph;
    const std::vector<std::unique_ptr<const WorldGenStage>> m_stages;
    const std::shared_ptr<const BlockGalleryChunkGenerator> m_blockGallery;
};

} // namespace Rigel::Voxel
