#pragma once

#include "World.h"
#include "WorldId.h"
#include "WorldResources.h"

#include <Rigel/Persistence/PersistenceService.h>

#include <memory>
#include <unordered_map>

namespace Rigel::Voxel {

namespace Persistence = ::Rigel::Persistence;

class WorldSet {
public:
    WorldSet();
    ~WorldSet();

    WorldResources& resources() { return m_resources; }
    const WorldResources& resources() const { return m_resources; }

    void initializeResources(Asset::AssetManager& assets);

    World& createWorld(WorldId id);

    bool hasWorld(WorldId id) const;
    World& world(WorldId id);
    const World& world(WorldId id) const;

    /**
     * @brief Destroy every world during application teardown.
     *
     * @pre Render views and asynchronous chunk loaders have already released
     * their references to these worlds.
     */
    void clear();

    Persistence::FormatRegistry& persistenceFormats() { return m_persistenceFormats; }
    const Persistence::FormatRegistry& persistenceFormats() const { return m_persistenceFormats; }
    Persistence::PersistenceService& persistenceService() { return m_persistenceService; }
    const Persistence::PersistenceService& persistenceService() const { return m_persistenceService; }

    void setPersistenceRoot(std::string rootPath) { m_persistenceRoot = std::move(rootPath); }
    void setPersistenceStorage(std::shared_ptr<Persistence::StorageBackend> storage) { m_persistenceStorage = std::move(storage); }
    void setPersistencePreferredFormat(std::string formatId) { m_persistencePreferredFormat = std::move(formatId); }
    void setPersistenceActiveFormat(WorldId id, std::string formatId);

    Persistence::PersistenceContext persistenceContext(WorldId id) const;

    static constexpr WorldId defaultWorldId() { return kDefaultWorldId; }

private:
    struct WorldEntry {
        World world;
        std::string persistenceFormat;
    };

    std::unordered_map<WorldId, std::unique_ptr<WorldEntry>> m_worlds;
    WorldResources m_resources;

    Persistence::FormatRegistry m_persistenceFormats;
    Persistence::PersistenceService m_persistenceService;
    std::string m_persistenceRoot;
    std::string m_persistencePreferredFormat;
    std::shared_ptr<Persistence::StorageBackend> m_persistenceStorage;
};

} // namespace Rigel::Voxel
