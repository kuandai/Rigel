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
#include "../entity/EntityPersistenceDetail.h"
#include "../entity/EntityPersistenceLimits.h"
#include "backends/cr/CRWorldMetadata.h"

#include <cmath>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace Rigel::Persistence {
namespace {

constexpr const char* kDefaultZoneId = "rigel:default";

BootstrappedWorldGeneration requirePublishedWorldGeneration(
    const Voxel::World& world,
    PersistenceService& service,
    const PersistenceContext& context) {
    return loadPublishedWorldGeneration(
        service, world.blockRegistry(), context);
}

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

    for (auto& [coords, regionSave] : regions) {
        (void)coords;
        ChunkRegionSnapshot existing;
        if (format.chunkContainer().regionExists(regionSave.key)) {
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
    detail::EntityRegionJournalUsage usage =
        detail::beginEntityRegionJournalUsage(format->descriptor());
    format->entityContainer().forEachRegion(zoneId, [&](const auto& key) {
        detail::EntityRegionJournalUsage candidateUsage = usage;
        size_t regionPayloadBytes = detail::accountDesiredEntityRegion(
            candidateUsage, key);

        EntityRegionSnapshot candidate =
            format->entityContainer().loadRegion(key);
        size_t regionChunks = 0;
        for (const auto& chunk : candidate.chunks) {
            detail::accountEntityRegionChunk(
                candidateUsage, regionPayloadBytes, regionChunks);
            ++regionChunks;
            size_t chunkEntities = 0;
            for (const auto& entity : chunk.entities) {
                detail::accountEntityRegionEntity(
                    candidateUsage,
                    regionPayloadBytes,
                    chunkEntities,
                    Entity::detail::measurePersistedEntityBytes(
                        entity.typeId, entity.modelId));
                ++chunkEntities;
            }
        }

        entityRegions.push_back(std::move(candidate));
        usage = candidateUsage;
        return true;
    });
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
                     const WorldSettings& settings,
                     PersistenceService& service,
                     PersistenceContext context) {
    const BootstrappedWorldGeneration published =
        requirePublishedWorldGeneration(world, service, context);
    if (settings != published.generation.settings) {
        throw std::runtime_error(
            "World save settings do not match the published world identity");
    }
    context.preferredFormat = published.persistenceFormat;
    context.discoverExistingFormat = false;

    auto format = service.openFormat(context);
    requireSupportedDefaultZone(*format, context);

    std::string zoneId = kDefaultZoneId;
    const bool zoneMetadataExists = context.storage->exists(
        format->zoneMetadataCodec().metadataPath(ZoneKey{zoneId}, context));
    std::optional<PersistenceService::PreparedMetadata>
        preparedZoneMetadata;
    if (!zoneMetadataExists) {
        preparedZoneMetadata = service.prepareZoneMetadataSave(
            ZoneMetadata{zoneId, zoneId}, *format, context);
    }

    detail::EntityRegionJournalPlan entityJournalPlan;
    std::vector<EntityRegionSnapshot> desiredEntityRegions;
    if (format->descriptor().capabilities.supportsEntityRegions) {
        detail::replayEntityRegionJournal(*format, context, zoneId);

        struct EntityRegionSave {
            std::unordered_map<Voxel::ChunkCoord,
                               size_t,
                               Voxel::ChunkCoordHash> chunkIndex;
            std::vector<Entity::EntityPersistedChunk> chunks;
            size_t payloadBytes = 0;
        };

        std::map<std::tuple<int32_t, int32_t, int32_t>, EntityRegionSave>
            entityRegions;
        detail::EntityRegionJournalUsage journalUsage =
            detail::beginEntityRegionJournalUsage(format->descriptor());

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

            const Voxel::ChunkCoord chunkCoord = Voxel::worldToChunk(
                static_cast<int>(std::floor(position.x)),
                static_cast<int>(std::floor(position.y)),
                static_cast<int>(std::floor(position.z)));
            const Entity::PersistenceRegionCoord regionCoord =
                Entity::persistenceRegionForChunk(chunkCoord);
            const auto regionKey = std::make_tuple(
                regionCoord.x, regionCoord.y, regionCoord.z);
            auto regionIt = entityRegions.find(regionKey);
            const bool isNewRegion = regionIt == entityRegions.end();

            bool isNewChunk = true;
            size_t existingRegionChunks = 0;
            size_t existingChunkEntities = 0;
            if (!isNewRegion) {
                existingRegionChunks = regionIt->second.chunks.size();
                const auto chunkIt =
                    regionIt->second.chunkIndex.find(chunkCoord);
                isNewChunk = chunkIt == regionIt->second.chunkIndex.end();
                if (!isNewChunk) {
                    existingChunkEntities =
                        regionIt->second.chunks[chunkIt->second]
                            .entities.size();
                }
            }

            const size_t encodedEntityBytes =
                Entity::detail::measurePersistedEntityBytes(
                    entity.typeId(), entity.modelIdentifier());
            detail::EntityRegionJournalUsage nextUsage = journalUsage;
            size_t nextRegionPayloadBytes = isNewRegion
                ? detail::accountDesiredEntityRegion(
                      nextUsage,
                      EntityRegionKey{
                          zoneId,
                          regionCoord.x,
                          regionCoord.y,
                          regionCoord.z})
                : regionIt->second.payloadBytes;
            if (isNewChunk) {
                detail::accountEntityRegionChunk(
                    nextUsage,
                    nextRegionPayloadBytes,
                    existingRegionChunks);
            }
            detail::accountEntityRegionEntity(
                nextUsage,
                nextRegionPayloadBytes,
                existingChunkEntities,
                encodedEntityBytes);

            if (isNewRegion) {
                regionIt = entityRegions.emplace(
                    regionKey, EntityRegionSave{}).first;
            }
            auto& region = regionIt->second;
            auto chunkIt = region.chunkIndex.find(chunkCoord);
            if (isNewChunk) {
                Entity::EntityPersistedChunk chunk;
                chunk.coord = chunkCoord;
                region.chunks.push_back(std::move(chunk));
                const size_t index = region.chunks.size() - 1;
                chunkIt = region.chunkIndex.emplace(
                    chunkCoord, index).first;
            }

            Entity::EntityPersistedEntity persisted;
            persisted.typeId = entity.typeId();
            persisted.id = entity.id();
            persisted.position = position;
            persisted.velocity = entity.velocity();
            persisted.viewDirection = entity.viewDirection();
            persisted.modelId = entity.modelIdentifier();
            region.chunks[chunkIt->second].entities.push_back(
                std::move(persisted));
            region.payloadBytes = nextRegionPayloadBytes;
            journalUsage = nextUsage;
        });

        desiredEntityRegions.reserve(entityRegions.size());
        for (auto& [coord, region] : entityRegions) {
            EntityRegionSnapshot snapshot;
            snapshot.key = EntityRegionKey{
                zoneId,
                std::get<0>(coord),
                std::get<1>(coord),
                std::get<2>(coord)};
            snapshot.chunks = std::move(region.chunks);
            desiredEntityRegions.push_back(std::move(snapshot));
        }
        entityJournalPlan = detail::prepareEntityRegionJournal(
            *format,
            zoneId,
            std::move(desiredEntityRegions));
    }

    std::vector<Voxel::ChunkCoord> dirtyChunks;
    world.chunkManager().forEachChunk(
        [&](Voxel::ChunkCoord coord, const Voxel::Chunk& chunk) {
            if (chunk.isPersistDirty()) {
                dirtyChunks.push_back(coord);
            }
        });
    saveChunkRegions(world, *format, dirtyChunks);

    if (format->descriptor().capabilities.supportsEntityRegions) {
        detail::publishAndApplyEntityRegionJournal(
            *format, context, entityJournalPlan);
    }

    if (preparedZoneMetadata) {
        service.publishMetadataSave(std::move(*preparedZoneMetadata));
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

    const BootstrappedWorldGeneration published =
        requirePublishedWorldGeneration(world, service, context);
    context.preferredFormat = published.persistenceFormat;
    context.discoverExistingFormat = false;
    auto format = service.openFormat(context);
    requireSupportedDefaultZone(*format, context);
    saveChunkRegions(world, *format, {coord});
}

} // namespace Rigel::Persistence
