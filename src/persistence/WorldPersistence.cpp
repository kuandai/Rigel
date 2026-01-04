#include "Rigel/Persistence/WorldPersistence.h"

#include "Rigel/Asset/AssetManager.h"
#include "Rigel/Entity/Entity.h"
#include "Rigel/Entity/EntityFactory.h"
#include "Rigel/Entity/EntityPersistence.h"
#include "Rigel/Entity/EntityRegion.h"
#include "Rigel/Persistence/Backends/CR/CRChunkData.h"
#include "Rigel/Persistence/Backends/CR/CRChunkMapping.h"
#include "Rigel/Persistence/Backends/CR/CRPaths.h"
#include "Rigel/Persistence/PersistenceService.h"
#include "Rigel/Voxel/Chunk.h"
#include "Rigel/Voxel/World.h"

#include <spdlog/spdlog.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace Rigel::Persistence {
namespace {

constexpr const char* kDefaultZoneId = "rigel:default";

int floorDiv(int value, int divisor) {
    int q = value / divisor;
    int r = value % divisor;
    if (r < 0) {
        q -= 1;
    }
    return q;
}

bool parseRegionFilename(const std::string& name, int& rx, int& ry, int& rz) {
    return std::sscanf(name.c_str(), "region_%d_%d_%d.cosmicreach", &rx, &ry, &rz) == 3;
}

bool parseEntityRegionFilename(const std::string& name, int& rx, int& ry, int& rz) {
    return std::sscanf(name.c_str(), "entityRegion_%d_%d_%d.crbin", &rx, &ry, &rz) == 3;
}

} // namespace

std::string mainWorldRootPath(Voxel::WorldId id) {
    return "saves/world_" + std::to_string(id);
}

void loadWorldFromDisk(Voxel::World& world,
                       Asset::AssetManager& assets,
                       PersistenceService& service,
                       PersistenceContext context,
                       uint32_t worldGenVersion) {
    namespace fs = std::filesystem;

    fs::path regionDir = fs::path(Backends::CR::CRPaths::zoneRoot(kDefaultZoneId, context)) / "regions";
    world.clear();
    world.chunkManager().clearDirtyFlags();

    if (fs::exists(regionDir)) {
        for (const auto& entry : fs::directory_iterator(regionDir)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            int rx = 0;
            int ry = 0;
            int rz = 0;
            if (!parseRegionFilename(entry.path().filename().string(), rx, ry, rz)) {
                continue;
            }
            RegionKey key{std::string(kDefaultZoneId), rx, ry, rz};
            ChunkRegionSnapshot region = service.loadRegion(key, context);
            for (const auto& chunk : region.chunks) {
                Backends::CR::decodeChunkSnapshot(
                    chunk,
                    world.chunkManager(),
                    world.blockRegistry(),
                    worldGenVersion);
            }
        }
    }

    fs::path entityDir = fs::path(Backends::CR::CRPaths::zoneRoot(kDefaultZoneId, context)) / "entities";
    if (fs::exists(entityDir)) {
        std::vector<Entity::EntityPersistedChunk> chunks;
        for (const auto& entry : fs::directory_iterator(entityDir)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            int rx = 0;
            int ry = 0;
            int rz = 0;
            if (!parseEntityRegionFilename(entry.path().filename().string(), rx, ry, rz)) {
                continue;
            }
            EntityRegionKey key{std::string(kDefaultZoneId), rx, ry, rz};
            EntityRegionSnapshot region = service.loadEntities(key, context);
            if (region.payload.empty()) {
                continue;
            }
            if (!Entity::decodeEntityRegionPayload(region.payload, chunks)) {
                spdlog::warn("Entity region {} {} {} failed to decode", rx, ry, rz);
                continue;
            }
            for (const auto& chunk : chunks) {
                for (const auto& saved : chunk.entities) {
                    std::unique_ptr<Entity::Entity> entity;
                    if (Entity::EntityFactory::instance().hasType(saved.typeId)) {
                        entity = Entity::EntityFactory::instance().create(saved.typeId);
                    }
                    if (!entity) {
                        entity = std::make_unique<Entity::Entity>(saved.typeId);
                    }
                    entity->setId(saved.id);
                    entity->setPosition(saved.position);
                    entity->setVelocity(saved.velocity);
                    entity->setViewDirection(saved.viewDirection);
                    if (!saved.modelId.empty() && assets.exists(saved.modelId)) {
                        auto model = assets.get<Entity::EntityModelAsset>(saved.modelId);
                        entity->setModel(std::move(model));
                    }
                    world.entities().spawn(std::move(entity));
                }
            }
        }
    }
}

void saveWorldToDisk(const Voxel::World& world,
                     PersistenceService& service,
                     PersistenceContext context) {
    const auto& registry = world.blockRegistry();

    struct RegionSave {
        RegionKey key;
        std::vector<Voxel::ChunkCoord> dirtyChunks;
    };

    std::map<std::tuple<int, int, int>, RegionSave> regions;
    world.chunkManager().forEachChunk([&](Voxel::ChunkCoord coord, const Voxel::Chunk& chunk) {
        if (!chunk.isPersistDirty()) {
            return;
        }
        int rx = floorDiv(coord.x, 16);
        int ry = floorDiv(coord.y, 16);
        int rz = floorDiv(coord.z, 16);
        auto keyTuple = std::make_tuple(rx, ry, rz);
        auto& region = regions[keyTuple];
        if (region.dirtyChunks.empty()) {
            region.key = RegionKey{std::string(kDefaultZoneId), rx, ry, rz};
        }
        region.dirtyChunks.push_back(coord);
    });

    for (auto& [coords, regionSave] : regions) {
        ChunkRegionSnapshot existing = service.loadRegion(regionSave.key, context);
        using KeyTuple = std::tuple<int32_t, int32_t, int32_t>;
        std::map<KeyTuple, ChunkSnapshot> merged;
        for (auto& snapshot : existing.chunks) {
            KeyTuple key{snapshot.key.x, snapshot.key.y, snapshot.key.z};
            merged.emplace(key, std::move(snapshot));
        }

        for (const Voxel::ChunkCoord& coord : regionSave.dirtyChunks) {
            for (int subchunkIndex = 0; subchunkIndex < 8; ++subchunkIndex) {
                ChunkKey crKey =
                    Backends::CR::toCRChunk({coord.x, coord.y, coord.z, subchunkIndex});
                KeyTuple key{crKey.x, crKey.y, crKey.z};
                merged.erase(key);
            }

            const Voxel::Chunk* chunk = world.chunkManager().getChunk(coord);
            if (!chunk) {
                continue;
            }
            auto snapshots = Backends::CR::encodeRigelChunk(
                *chunk,
                registry,
                coord,
                kDefaultZoneId);
            for (auto& snapshot : snapshots) {
                KeyTuple key{snapshot.key.x, snapshot.key.y, snapshot.key.z};
                merged[key] = std::move(snapshot);
            }
        }

        ChunkRegionSnapshot out;
        out.key = regionSave.key;
        out.chunks.reserve(merged.size());
        for (auto& entry : merged) {
            out.chunks.push_back(std::move(entry.second));
        }

        service.saveRegion(out, context);
    }

    struct EntityRegionSave {
        EntityRegionKey key;
        std::unordered_map<Voxel::ChunkCoord, size_t, Voxel::ChunkCoordHash> chunkIndex;
        std::vector<Entity::EntityPersistedChunk> chunks;
    };

    std::unordered_map<Entity::EntityRegionCoord,
                       EntityRegionSave,
                       Entity::EntityRegionCoordHash> entityRegions;

    world.entities().forEach([&](const Entity::Entity& entity) {
        if (entity.hasTag(Entity::EntityTags::NoSaveInChunks)) {
            return;
        }
        const glm::vec3& pos = entity.position();
        Voxel::ChunkCoord coord = Voxel::worldToChunk(
            static_cast<int>(std::floor(pos.x)),
            static_cast<int>(std::floor(pos.y)),
            static_cast<int>(std::floor(pos.z))
        );
        Entity::EntityRegionCoord regionCoord = Entity::chunkToRegion(coord);
        auto& region = entityRegions[regionCoord];
        if (region.chunks.empty() && region.chunkIndex.empty()) {
            region.key = EntityRegionKey{std::string(kDefaultZoneId),
                                                      regionCoord.x,
                                                      regionCoord.y,
                                                      regionCoord.z};
        }
        auto it = region.chunkIndex.find(coord);
        if (it == region.chunkIndex.end()) {
            Entity::EntityPersistedChunk chunk;
            chunk.coord = coord;
            region.chunks.push_back(std::move(chunk));
            size_t index = region.chunks.size() - 1;
            region.chunkIndex.emplace(coord, index);
            it = region.chunkIndex.find(coord);
        }
        Entity::EntityPersistedEntity saved;
        saved.typeId = entity.typeId();
        saved.id = entity.id();
        saved.position = entity.position();
        saved.velocity = entity.velocity();
        saved.viewDirection = entity.viewDirection();
        if (entity.model()) {
            saved.modelId = entity.model().id();
        }
        region.chunks[it->second].entities.push_back(std::move(saved));
    });

    namespace fs = std::filesystem;
    fs::path entityDir = fs::path(Backends::CR::CRPaths::zoneRoot(kDefaultZoneId, context)) / "entities";
    if (fs::exists(entityDir)) {
        for (const auto& entry : fs::directory_iterator(entityDir)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            int rx = 0;
            int ry = 0;
            int rz = 0;
            if (!parseEntityRegionFilename(entry.path().filename().string(), rx, ry, rz)) {
                continue;
            }
            Entity::EntityRegionCoord coord{rx, ry, rz};
            if (entityRegions.find(coord) != entityRegions.end()) {
                continue;
            }
            EntityRegionSave empty;
            empty.key = EntityRegionKey{std::string(kDefaultZoneId), rx, ry, rz};
            entityRegions.emplace(coord, std::move(empty));
        }
    }

    for (auto& [coord, region] : entityRegions) {
        EntityRegionSnapshot snapshot;
        snapshot.key = region.key;
        if (!region.chunks.empty()) {
            snapshot.payload = Entity::encodeEntityRegionPayload(region.chunks);
        }
        service.saveEntities(snapshot, context);
    }

    WorldSnapshot worldSnapshot;
    worldSnapshot.metadata.worldId = "world_" + std::to_string(world.id());
    worldSnapshot.metadata.displayName = worldSnapshot.metadata.worldId;
    worldSnapshot.zones.push_back(ZoneMetadata{std::string(kDefaultZoneId), std::string(kDefaultZoneId)});
    service.saveWorld(worldSnapshot, SaveScope::MetadataOnly, context);
}

} // namespace Rigel::Persistence
