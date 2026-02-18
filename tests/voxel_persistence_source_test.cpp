#include "TestFramework.h"

#include "Rigel/Persistence/Backends/Memory/MemoryFormat.h"
#include "Rigel/Persistence/ChunkSerializer.h"
#include "Rigel/Persistence/PersistenceService.h"
#include "Rigel/Persistence/Storage.h"
#include "Rigel/Voxel/VoxelLod/GeneratorSource.h"
#include "Rigel/Voxel/VoxelLod/LoadedChunkSource.h"
#include "Rigel/Voxel/VoxelLod/PersistenceSource.h"
#include "Rigel/Voxel/VoxelLod/VoxelSourceChain.h"

#include <filesystem>
#include <random>

using namespace Rigel;
using namespace Rigel::Voxel;

namespace {

Persistence::PersistenceContext makeContext(const std::filesystem::path& root) {
    Persistence::PersistenceContext context;
    context.rootPath = root.string();
    context.preferredFormat = "memory";
    context.storage = std::make_shared<Persistence::FilesystemBackend>();
    return context;
}

Persistence::PersistenceService makeService(Persistence::FormatRegistry& registry) {
    registry.registerFormat(
        Persistence::Backends::Memory::descriptor(),
        Persistence::Backends::Memory::factory(),
        Persistence::Backends::Memory::probe());
    return Persistence::PersistenceService(registry);
}

Chunk makeChunk(ChunkCoord coord, bool randomPattern, BlockID a, BlockID b, BlockID c) {
    Chunk chunk(coord);
    std::mt19937 rng(1337);
    std::uniform_int_distribution<int> dist(0, 2);
    for (int z = 0; z < Chunk::SIZE; ++z) {
        for (int y = 0; y < Chunk::SIZE; ++y) {
            for (int x = 0; x < Chunk::SIZE; ++x) {
                BlockID id = a;
                if (randomPattern) {
                    const int value = dist(rng);
                    id = (value == 0) ? a : (value == 1 ? b : c);
                } else {
                    const int value = (x * 3 + y * 5 + z * 7) % 3;
                    id = (value == 0) ? a : (value == 1 ? b : c);
                }
                chunk.setBlock(x, y, z, BlockState{id});
            }
        }
    }
    return chunk;
}

void saveChunkToMemoryFormat(Persistence::PersistenceService& service,
                             const Persistence::PersistenceContext& context,
                             const std::string& zoneId,
                             const Chunk& chunk) {
    auto format = service.openFormat(context);
    Persistence::RegionLayout& layout = format->regionLayout();
    const auto regionKey = layout.regionForChunk(zoneId, chunk.position());
    const auto storageKeys = layout.storageKeysForChunk(zoneId, chunk.position());

    Persistence::ChunkRegionSnapshot region;
    region.key = regionKey;
    region.chunks.reserve(storageKeys.size());
    for (const auto& storageKey : storageKeys) {
        Persistence::ChunkSnapshot snapshot;
        snapshot.key = storageKey;
        snapshot.data = Persistence::serializeChunkSpan(chunk, layout.spanForStorageKey(storageKey));
        region.chunks.push_back(std::move(snapshot));
    }

    service.saveRegion(region, context);
}

void verifyBrickMatchesChunk(std::span<const VoxelId> sampled, const Chunk& chunk) {
    for (int z = 0; z < Chunk::SIZE; ++z) {
        for (int y = 0; y < Chunk::SIZE; ++y) {
            for (int x = 0; x < Chunk::SIZE; ++x) {
                const size_t index = static_cast<size_t>(x) +
                    static_cast<size_t>(y) * static_cast<size_t>(Chunk::SIZE) +
                    static_cast<size_t>(z) * static_cast<size_t>(Chunk::SIZE) *
                    static_cast<size_t>(Chunk::SIZE);
                CHECK_EQ(sampled[index], toVoxelId(chunk.getBlock(x, y, z).id));
            }
        }
    }
}

} // namespace

TEST_CASE(VoxelPersistenceSource_DeterministicBrickMatchesSavedChunk) {
    Persistence::FormatRegistry registry;
    auto service = makeService(registry);

    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("rigel_voxel_persist_source_" + std::to_string(now));
    std::filesystem::create_directories(root);

    const auto context = makeContext(root);
    const std::string zoneId = "rigel:test_zone";
    const ChunkCoord coord{1, -2, 3};

    Chunk chunk = makeChunk(coord, false, BlockID{1}, BlockID{2}, BlockID{3});
    saveChunkToMemoryFormat(service, context, zoneId, chunk);

    PersistenceSource source(&service, context, zoneId);
    source.setCacheLimits(8, 64);

    BrickSampleDesc desc;
    desc.worldMinVoxel = glm::ivec3(coord.x * Chunk::SIZE, coord.y * Chunk::SIZE, coord.z * Chunk::SIZE);
    desc.brickDimsVoxels = glm::ivec3(Chunk::SIZE);
    desc.stepVoxels = 1;
    std::vector<VoxelId> sampled(desc.outVoxelCount(), kVoxelAir);

    CHECK_EQ(source.sampleBrick(desc, sampled), BrickSampleStatus::Hit);
    verifyBrickMatchesChunk(sampled, chunk);
    std::filesystem::remove_all(root);
}

TEST_CASE(VoxelPersistenceSource_RandomBrickMatchesSavedChunk) {
    Persistence::FormatRegistry registry;
    auto service = makeService(registry);

    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("rigel_voxel_persist_source_rand_" + std::to_string(now));
    std::filesystem::create_directories(root);

    const auto context = makeContext(root);
    const std::string zoneId = "rigel:test_zone";
    const ChunkCoord coord{-4, 1, -1};

    Chunk chunk = makeChunk(coord, true, BlockID{2}, BlockID{4}, BlockID{6});
    saveChunkToMemoryFormat(service, context, zoneId, chunk);

    PersistenceSource source(&service, context, zoneId);
    source.setCacheLimits(8, 64);

    BrickSampleDesc desc;
    desc.worldMinVoxel = glm::ivec3(coord.x * Chunk::SIZE, coord.y * Chunk::SIZE, coord.z * Chunk::SIZE);
    desc.brickDimsVoxels = glm::ivec3(Chunk::SIZE);
    desc.stepVoxels = 1;
    std::vector<VoxelId> sampled(desc.outVoxelCount(), kVoxelAir);

    CHECK_EQ(source.sampleBrick(desc, sampled), BrickSampleStatus::Hit);
    verifyBrickMatchesChunk(sampled, chunk);
    std::filesystem::remove_all(root);
}

TEST_CASE(VoxelPersistenceSource_MissingChunkReturnsMiss) {
    Persistence::FormatRegistry registry;
    auto service = makeService(registry);

    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("rigel_voxel_persist_source_miss_" + std::to_string(now));
    std::filesystem::create_directories(root);

    const auto context = makeContext(root);
    PersistenceSource source(&service, context, "rigel:test_zone");
    source.setCacheLimits(4, 16);

    BrickSampleDesc desc;
    desc.worldMinVoxel = glm::ivec3(0);
    desc.brickDimsVoxels = glm::ivec3(Chunk::SIZE);
    desc.stepVoxels = 1;
    std::vector<VoxelId> sampled(desc.outVoxelCount(), kVoxelAir);

    CHECK_EQ(source.sampleBrick(desc, sampled), BrickSampleStatus::Miss);
    std::filesystem::remove_all(root);
}

TEST_CASE(VoxelPersistenceSource_LoadedSourceOverridesPersistedData) {
    Persistence::FormatRegistry registry;
    auto service = makeService(registry);

    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("rigel_voxel_persist_source_override_" + std::to_string(now));
    std::filesystem::create_directories(root);

    const auto context = makeContext(root);
    const std::string zoneId = "rigel:test_zone";
    const ChunkCoord coord{0, 0, 0};

    Chunk persisted(coord);
    persisted.fill(BlockState{BlockID{5}});
    saveChunkToMemoryFormat(service, context, zoneId, persisted);

    Chunk loaded(coord);
    loaded.fill(BlockState{BlockID{9}});
    std::array<BlockState, Chunk::VOLUME> loadedBlocks{};
    loaded.copyBlocks(loadedBlocks);
    std::vector<LoadedChunkSource::ChunkSnapshot> snapshots;
    snapshots.push_back(LoadedChunkSource::ChunkSnapshot{coord, loadedBlocks});
    LoadedChunkSource loadedSource(std::move(snapshots));

    PersistenceSource persistenceSource(&service, context, zoneId);
    GeneratorSource generator([](ChunkCoord,
                                 std::array<BlockState, Chunk::VOLUME>& out,
                                 const std::atomic_bool*) {
        out.fill(BlockState{BlockID{11}});
    });

    VoxelSourceChain chain;
    chain.setLoaded(&loadedSource);
    chain.setPersistence(&persistenceSource);
    chain.setGenerator(&generator);

    BrickSampleDesc desc;
    desc.worldMinVoxel = glm::ivec3(0);
    desc.brickDimsVoxels = glm::ivec3(Chunk::SIZE);
    desc.stepVoxels = 1;
    std::vector<VoxelId> sampled(desc.outVoxelCount(), kVoxelAir);

    CHECK_EQ(chain.sampleBrick(desc, sampled), BrickSampleStatus::Hit);
    for (VoxelId id : sampled) {
        CHECK_EQ(id, toVoxelId(BlockID{9}));
    }
    std::filesystem::remove_all(root);
}
