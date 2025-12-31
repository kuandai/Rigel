#pragma once

#include "World.h"
#include "WorldId.h"
#include "WorldResources.h"
#include "WorldView.h"

#include <Rigel/Asset/AssetManager.h>

#include <memory>
#include <unordered_map>

namespace Rigel::Voxel {

class WorldSet {
public:
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

    static constexpr WorldId defaultWorldId() { return kDefaultWorldId; }

private:
    struct WorldEntry {
        World world;
        std::unique_ptr<WorldView> view;
    };

    std::unordered_map<WorldId, std::unique_ptr<WorldEntry>> m_worlds;
    WorldResources m_resources;
};

} // namespace Rigel::Voxel
