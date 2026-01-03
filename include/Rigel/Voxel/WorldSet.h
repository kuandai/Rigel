#pragma once

#include "World.h"
#include "WorldId.h"
#include "WorldResources.h"
#include "WorldView.h"

#include <Rigel/Asset/AssetManager.h>
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
    WorldView& createView(WorldId id, Asset::AssetManager& assets);

    bool hasWorld(WorldId id) const;
    World& world(WorldId id);
    const World& world(WorldId id) const;

    WorldView* findView(WorldId id);
    const WorldView* findView(WorldId id) const;
    WorldView& view(WorldId id);

    void removeWorld(WorldId id);
    void clear();

    Persistence::FormatRegistry& persistenceFormats() { return m_persistenceFormats; }
    const Persistence::FormatRegistry& persistenceFormats() const { return m_persistenceFormats; }
    Persistence::PersistenceService& persistenceService() { return m_persistenceService; }
    const Persistence::PersistenceService& persistenceService() const { return m_persistenceService; }

    void setPersistenceRoot(std::string rootPath) { m_persistenceRoot = std::move(rootPath); }
    void setPersistenceStorage(std::shared_ptr<Persistence::StorageBackend> storage) { m_persistenceStorage = std::move(storage); }
    void setPersistencePolicies(Persistence::PersistencePolicies policies) { m_persistencePolicies = std::move(policies); }
    void setPersistencePreferredFormat(std::string formatId) { m_persistencePreferredFormat = std::move(formatId); }

    Persistence::PersistenceContext persistenceContext(WorldId id) const;

    static constexpr WorldId defaultWorldId() { return kDefaultWorldId; }

private:
    struct WorldEntry {
        World world;
        std::unique_ptr<WorldView> view;
    };

    std::unordered_map<WorldId, std::unique_ptr<WorldEntry>> m_worlds;
    WorldResources m_resources;

    Persistence::FormatRegistry m_persistenceFormats;
    Persistence::PersistenceService m_persistenceService;
    std::string m_persistenceRoot;
    std::string m_persistencePreferredFormat;
    Persistence::PersistencePolicies m_persistencePolicies{};
    std::shared_ptr<Persistence::StorageBackend> m_persistenceStorage;
};

} // namespace Rigel::Voxel
