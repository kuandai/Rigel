#include "Rigel/Persistence/WorldPersistence.h"

#include "Rigel/Asset/AssetManager.h"
#include "Rigel/Entity/Entity.h"
#include "Rigel/Entity/EntityFactory.h"
#include "Rigel/Entity/EntityPersistence.h"
#include "Rigel/Persistence/Backends/CR/CRFormat.h"
#include "Rigel/Persistence/ChunkSerializer.h"
#include "Rigel/Persistence/Format.h"
#include "Rigel/Persistence/PersistenceService.h"
#include "Rigel/Persistence/Storage.h"
#include "Rigel/Voxel/Chunk.h"
#include "Rigel/Voxel/World.h"
#include "EntityRegionJournal.h"
#include "../entity/EntityPersistenceLimits.h"
#include "backends/cr/CRWorldMetadata.h"

#include <cmath>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
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

std::string describeEntityId(const Entity::EntityId& id) {
    return std::to_string(id.time) + ":" +
        std::to_string(id.random) + ":" +
        std::to_string(id.counter);
}

const char* invalidPositionField(const glm::vec3& position) {
    if (!Entity::detail::isPersistablePositionComponent(position.x)) {
        return "position.x";
    }
    if (!Entity::detail::isPersistablePositionComponent(position.y)) {
        return "position.y";
    }
    if (!Entity::detail::isPersistablePositionComponent(position.z)) {
        return "position.z";
    }
    return nullptr;
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

void loadBootstrapEntities(Voxel::World& world,
                           Asset::AssetManager& assets,
                           PersistenceService& service,
                           PersistenceContext context) {
    auto format = service.openFormat(context);
    requireSupportedDefaultZone(*format, context);

    if (!format->descriptor().capabilities.supportsEntityRegions) {
        return;
    }

    std::string zoneId = kDefaultZoneId;
    detail::replayEntityRegionJournal(*format, context, zoneId);
    std::vector<EntityRegionSnapshot> entityRegions;
    for (const auto& key : format->entityContainer().listRegions(zoneId)) {
        entityRegions.push_back(format->entityContainer().loadRegion(key));
    }
    detail::validateEntityRegionSnapshots(entityRegions);

    for (const auto& region : entityRegions) {
        for (const auto& chunk : region.chunks) {
            for (const auto& saved : chunk.entities) {
                if (world.entities().get(saved.id)) {
                    throw std::runtime_error(
                        "Persistent entity ID " + describeEntityId(saved.id) +
                        " collides with a live entity");
                }
            }
        }
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
                entity->setModelIdentifier(saved.modelId);
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

    struct StagedEntitySave {
        Voxel::ChunkCoord chunk;
        Entity::EntityPersistedEntity entity;
    };
    std::vector<StagedEntitySave> stagedEntities;
    if (format->descriptor().capabilities.supportsEntityRegions) {
        world.entities().forEach([&](const Entity::Entity& entity) {
            if (entity.hasTag(Entity::EntityTags::NoSaveInChunks)) {
                return;
            }
            const glm::vec3& position = entity.position();
            if (const char* field = invalidPositionField(position)) {
                throw std::runtime_error(
                    std::string("Invalid persistent entity ") + field +
                    " for entity ID " + describeEntityId(entity.id()));
            }

            StagedEntitySave staged;
            staged.chunk = Voxel::worldToChunk(
                static_cast<int>(std::floor(position.x)),
                static_cast<int>(std::floor(position.y)),
                static_cast<int>(std::floor(position.z)));
            staged.entity.typeId = entity.typeId();
            staged.entity.id = entity.id();
            staged.entity.position = position;
            staged.entity.velocity = entity.velocity();
            staged.entity.viewDirection = entity.viewDirection();
            staged.entity.modelId = entity.modelIdentifier();
            stagedEntities.push_back(std::move(staged));
        });
    }

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
        for (auto& staged : stagedEntities) {
            const Voxel::ChunkCoord coord = staged.chunk;
            const Entity::PersistenceRegionCoord regionCoord =
                Entity::persistenceRegionForChunk(coord);
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
            region.chunks[it->second].entities.push_back(
                std::move(staged.entity));
        }

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

} // namespace Rigel::Persistence
