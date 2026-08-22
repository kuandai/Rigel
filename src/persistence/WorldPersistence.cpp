#include "Rigel/Persistence/WorldPersistence.h"

#include "Rigel/Asset/AssetManager.h"
#include "Rigel/Entity/Entity.h"
#include "Rigel/Entity/EntityFactory.h"
#include "Rigel/Entity/EntityPersistence.h"
#include "Rigel/Entity/EntityRegion.h"
#include "Rigel/Persistence/Backends/CR/CRFormat.h"
#include "Rigel/Persistence/ChunkSerializer.h"
#include "Rigel/Persistence/Format.h"
#include "Rigel/Persistence/PersistenceService.h"
#include "Rigel/Persistence/Storage.h"
#include "Rigel/Voxel/Chunk.h"
#include "Rigel/Voxel/World.h"
#include "ChunkValidation.h"
#include "EntityRegionJournal.h"
#include "backends/cr/CRWorldMetadata.h"

#include <cmath>
#include <exception>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Rigel::Persistence {
namespace {

constexpr const char* kDefaultZoneId = "rigel:default";

void requireSupportedDefaultZone(const PersistenceFormat& format,
                                 const PersistenceContext& context) {
    if (format.descriptor().id == Backends::CR::descriptor().id) {
        Backends::CR::requireSupportedDefaultZone(context, kDefaultZoneId);
    }
}

void saveChunkRegions(const Voxel::World& world,
                      PersistenceFormat& format,
                      const std::vector<Voxel::ChunkCoord>& dirtyChunks) {
    const auto& layout = format.regionLayout();

    struct RegionSave {
        RegionKey key;
        std::vector<Voxel::ChunkCoord> chunks;
    };

    std::map<std::tuple<int, int, int>, RegionSave> regions;
    for (const Voxel::ChunkCoord& coord : dirtyChunks) {
        RegionKey regionKey = layout.regionForChunk(kDefaultZoneId, coord);
        auto keyTuple = std::make_tuple(regionKey.x, regionKey.y, regionKey.z);
        auto& region = regions[keyTuple];
        if (region.chunks.empty()) {
            region.key = regionKey;
        }
        region.chunks.push_back(coord);
    }

    std::set<std::tuple<int, int, int>> existingRegions;
    for (const auto& key : format.chunkContainer().listRegions(kDefaultZoneId)) {
        existingRegions.insert(std::make_tuple(key.x, key.y, key.z));
    }

    for (auto& [coords, regionSave] : regions) {
        ChunkRegionSnapshot existing;
        if (existingRegions.contains(coords)) {
            existing = format.chunkContainer().loadRegion(regionSave.key);
        } else {
            existing.key = regionSave.key;
        }

        using KeyTuple = std::tuple<int32_t, int32_t, int32_t>;
        std::map<KeyTuple, ChunkSnapshot> merged;
        for (auto& snapshot : existing.chunks) {
            KeyTuple key{snapshot.key.x, snapshot.key.y, snapshot.key.z};
            merged.emplace(key, std::move(snapshot));
        }

        for (const Voxel::ChunkCoord& coord : regionSave.chunks) {
            const Voxel::Chunk* chunk = world.chunkManager().getChunk(coord);
            if (!chunk) {
                continue;
            }
            for (const auto& storageKey :
                 layout.storageKeysForChunk(kDefaultZoneId, coord)) {
                KeyTuple key{storageKey.x, storageKey.y, storageKey.z};

                ChunkSpan span = layout.spanForStorageKey(storageKey);
                ChunkData data = serializeChunkSpan(*chunk, span);
                ChunkSnapshot snapshot;
                snapshot.key = storageKey;
                snapshot.data = std::move(data);
                auto existingSnapshot = merged.find(key);
                if (existingSnapshot != merged.end()) {
                    snapshot.opaquePayload = std::move(existingSnapshot->second.opaquePayload);
                }
                merged[key] = std::move(snapshot);
            }
        }

        ChunkRegionSnapshot out;
        out.key = regionSave.key;
        out.chunks.reserve(merged.size());
        for (auto& entry : merged) {
            out.chunks.push_back(std::move(entry.second));
        }
        format.chunkContainer().saveRegion(out);
    }
}

} // namespace

std::string mainWorldRootPath(Voxel::WorldId id) {
    return "saves/world_" + std::to_string(id);
}

void loadWorldFromDisk(Voxel::World& world,
                       Asset::AssetManager& assets,
                       PersistenceService& service,
                       PersistenceContext context,
                       uint32_t worldGenVersion,
                       LoadScope scope) {
    auto format = service.openFormat(context);
    requireSupportedDefaultZone(*format, context);

    std::vector<ChunkRegionSnapshot> chunkRegions;
    if (includesChunks(scope)) {
        for (const auto& key :
             format->chunkContainer().listRegions(kDefaultZoneId)) {
            ChunkRegionSnapshot region =
                format->chunkContainer().loadRegion(key);
            for (const auto& snapshot : region.chunks) {
                detail::validateChunkData(
                    snapshot.data, world.blockRegistry());
            }
            chunkRegions.push_back(std::move(region));
        }
    }

    std::string zoneId = kDefaultZoneId;
    std::vector<EntityRegionSnapshot> entityRegions;
    if (includesEntities(scope) &&
        format->descriptor().capabilities.supportsEntityRegions) {
        detail::replayEntityRegionJournal(*format, context, zoneId);
        for (const auto& key : format->entityContainer().listRegions(zoneId)) {
            entityRegions.push_back(format->entityContainer().loadRegion(key));
        }
        detail::validateEntityRegionSnapshots(entityRegions);
    }

    world.clear();
    world.chunkManager().clearDirtyFlags();

    std::unordered_set<Voxel::ChunkCoord, Voxel::ChunkCoordHash> touchedChunks;

    if (includesChunks(scope)) {
        for (const auto& region : chunkRegions) {
            for (const auto& snapshot : region.chunks) {
                const ChunkSpan& span = snapshot.data.span;
                Voxel::ChunkCoord coord{span.chunkX, span.chunkY, span.chunkZ};
                Voxel::Chunk& chunk = world.chunkManager().getOrCreateChunk(coord);
                chunk.setWorldGenVersion(worldGenVersion);
                applyChunkData(snapshot.data, chunk, world.blockRegistry());
                touchedChunks.insert(coord);
            }
        }

        for (const auto& coord : touchedChunks) {
            Voxel::Chunk* chunk = world.chunkManager().getChunk(coord);
            if (!chunk) {
                continue;
            }
            chunk->clearDirty();
            chunk->clearPersistDirty();
        }
    }

    if (!includesEntities(scope)) {
        return;
    }

    if (!format->descriptor().capabilities.supportsEntityRegions) {
        return;
    }

    struct StagedEntity {
        Entity::EntityId id;
        std::unique_ptr<Entity::Entity> entity;
    };
    std::vector<StagedEntity> stagedEntities;
    for (const auto& region : entityRegions) {
        for (const auto& chunk : region.chunks) {
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
                stagedEntities.push_back(StagedEntity{
                    saved.id, std::move(entity)});
            }
        }
    }
    for (auto& staged : stagedEntities) {
        const Entity::EntityId spawnedId =
            world.entities().spawn(std::move(staged.entity));
        if (spawnedId != staged.id) {
            throw std::runtime_error(
                "Failed to spawn validated persistent entity");
        }
    }
}

void saveWorldToDisk(const Voxel::World& world,
                     PersistenceService& service,
                     PersistenceContext context) {
    auto format = service.openFormat(context);
    requireSupportedDefaultZone(*format, context);
    std::string zoneId = kDefaultZoneId;
    if (format->descriptor().capabilities.supportsEntityRegions) {
        detail::replayEntityRegionJournal(*format, context, zoneId);
    }
    const bool worldMetadataExists = context.storage->exists(
        format->worldMetadataCodec().metadataPath(context));
    const bool zoneMetadataExists = context.storage->exists(
        format->zoneMetadataCodec().metadataPath(ZoneKey{zoneId}, context));
    std::vector<Voxel::ChunkCoord> dirtyChunks;
    world.chunkManager().forEachChunk([&](Voxel::ChunkCoord coord, const Voxel::Chunk& chunk) {
        if (chunk.isPersistDirty()) {
            dirtyChunks.push_back(coord);
        }
    });
    saveChunkRegions(world, *format, dirtyChunks);

    struct EntityRegionSave {
        std::unordered_map<Voxel::ChunkCoord, size_t, Voxel::ChunkCoordHash> chunkIndex;
        std::vector<Entity::EntityPersistedChunk> chunks;
    };

    std::map<std::tuple<int32_t, int32_t, int32_t>, EntityRegionSave>
        entityRegions;

    if (format->descriptor().capabilities.supportsEntityRegions) {
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
            auto& region = entityRegions[std::make_tuple(
                regionCoord.x, regionCoord.y, regionCoord.z)];
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

        std::vector<EntityRegionSnapshot> desiredRegions;
        desiredRegions.reserve(entityRegions.size());
        for (auto& [coord, region] : entityRegions) {
            EntityRegionSnapshot snapshot;
            snapshot.key = EntityRegionKey{
                zoneId,
                std::get<0>(coord),
                std::get<1>(coord),
                std::get<2>(coord)};
            snapshot.chunks = std::move(region.chunks);
            desiredRegions.push_back(std::move(snapshot));
        }
        detail::saveEntityRegionsRecoverably(
            *format, context, zoneId, std::move(desiredRegions));
    }

    WorldSnapshot worldSnapshot;
    worldSnapshot.metadata.worldId = "world_" + std::to_string(world.id());
    worldSnapshot.metadata.displayName = worldSnapshot.metadata.worldId;
    if (!worldMetadataExists) {
        service.saveWorld(worldSnapshot, context);
    }
    if (!zoneMetadataExists) {
        service.saveZoneMetadata(ZoneMetadata{zoneId, zoneId}, context);
    }
}

void saveChunkToDisk(const Voxel::World& world,
                     PersistenceService& service,
                     PersistenceContext context,
                     const Voxel::ChunkCoord& coord) {
    const Voxel::Chunk* chunk = world.chunkManager().getChunk(coord);
    if (!chunk || !chunk->isPersistDirty()) {
        return;
    }

    auto format = service.openFormat(context);
    requireSupportedDefaultZone(*format, context);
    saveChunkRegions(world, *format, {coord});
}

bool loadChunkFromDisk(Voxel::World& world,
                       PersistenceService& service,
                       PersistenceContext context,
                       const Voxel::ChunkCoord& coord,
                       uint32_t worldGenVersion) {
    auto format = service.openFormat(context);
    requireSupportedDefaultZone(*format, context);
    const auto& layout = format->regionLayout();
    std::string zoneId = kDefaultZoneId;

    RegionKey regionKey = layout.regionForChunk(zoneId, coord);
    ChunkRegionSnapshot region;
    try {
        region = format->chunkContainer().loadRegion(regionKey);
    } catch (const std::exception&) {
        return false;
    }

    if (region.chunks.empty()) {
        return false;
    }

    std::vector<const ChunkSnapshot*> matchingSnapshots;
    for (const auto& snapshot : region.chunks) {
        const ChunkSpan& span = snapshot.data.span;
        if (span.chunkX != coord.x || span.chunkY != coord.y || span.chunkZ != coord.z) {
            continue;
        }
        detail::validateChunkData(snapshot.data, world.blockRegistry());
        matchingSnapshots.push_back(&snapshot);
    }

    if (matchingSnapshots.empty()) {
        return false;
    }

    Voxel::Chunk& chunk = world.chunkManager().getOrCreateChunk(coord);
    chunk.setWorldGenVersion(worldGenVersion);
    for (const ChunkSnapshot* snapshot : matchingSnapshots) {
        applyChunkData(snapshot->data, chunk, world.blockRegistry());
    }
    chunk.clearDirty();
    chunk.clearPersistDirty();

    return true;
}

} // namespace Rigel::Persistence
