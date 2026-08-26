#pragma once

#include "Rigel/Persistence/FormatRegistry.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Rigel::Persistence {

class PersistenceService {
public:
    class PreparedMetadata {
    public:
        PreparedMetadata(PreparedMetadata&&) noexcept = default;
        PreparedMetadata& operator=(PreparedMetadata&&) noexcept = default;
        PreparedMetadata(const PreparedMetadata&) = delete;
        PreparedMetadata& operator=(const PreparedMetadata&) = delete;

    private:
        friend class PersistenceService;

        PreparedMetadata(std::shared_ptr<StorageBackend> storage,
                         std::string path,
                         std::vector<uint8_t> bytes)
            : m_storage(std::move(storage)),
              m_path(std::move(path)),
              m_bytes(std::move(bytes)) {
        }

        std::shared_ptr<StorageBackend> m_storage;
        std::string m_path;
        std::vector<uint8_t> m_bytes;
    };

    explicit PersistenceService(FormatRegistry& registry);

    std::unique_ptr<PersistenceFormat> openFormat(const PersistenceContext& context) const;

    PreparedMetadata prepareWorldMetadataSave(
        const WorldMetadata& metadata,
        PersistenceFormat& format,
        const PersistenceContext& context) const;
    void saveWorldMetadata(const WorldMetadata& metadata, const PersistenceContext& context);
    WorldMetadata loadWorldMetadata(const PersistenceContext& context);

    PreparedMetadata prepareZoneMetadataSave(
        const ZoneMetadata& metadata,
        PersistenceFormat& format,
        const PersistenceContext& context) const;
    void publishMetadataSave(PreparedMetadata prepared) const;
    void saveZoneMetadata(const ZoneMetadata& metadata, const PersistenceContext& context);
    ZoneMetadata loadZoneMetadata(const ZoneKey& key, const PersistenceContext& context);

    void saveRegion(const ChunkRegionSnapshot& region, const PersistenceContext& context);
    ChunkRegionSnapshot loadRegion(const RegionKey& key, const PersistenceContext& context);

    void saveEntities(const EntityRegionSnapshot& region, const PersistenceContext& context);
    EntityRegionSnapshot loadEntities(const EntityRegionKey& key, const PersistenceContext& context);

private:
    FormatRegistry& m_registry;

    std::unique_ptr<PersistenceFormat> resolve(const PersistenceContext& context) const;
    void handleUnsupportedFeature(const PersistenceContext& context, const std::string& message) const;
};

} // namespace Rigel::Persistence
