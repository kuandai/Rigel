#include "Rigel/Voxel/WorldSet.h"

#include <stdexcept>

namespace Rigel::Voxel {

WorldSet::WorldSet()
    : m_persistenceService(m_persistenceFormats) {
}

WorldSet::~WorldSet() = default;

void WorldSet::initializeResources(Asset::AssetManager& assets) {
    m_resources.initialize(assets);
}

World& WorldSet::createWorld(WorldId id) {
    auto [it, inserted] = m_worlds.emplace(id, nullptr);
    if (inserted || !it->second) {
        it->second = std::make_unique<WorldEntry>();
        it->second->world.setId(id);
        it->second->world.initialize(m_resources);
    }
    return it->second->world;
}

WorldView& WorldSet::createView(WorldId id, Asset::AssetManager& assets) {
    World& world = createWorld(id);
    WorldEntry& entry = *m_worlds.at(id);
    if (!entry.view) {
        entry.view = std::make_unique<WorldView>(world, m_resources);
        entry.view->initialize(assets);
        if (world.generator()) {
            entry.view->setGenerator(world.generator());
        }
    }
    return *entry.view;
}

bool WorldSet::hasWorld(WorldId id) const {
    return m_worlds.find(id) != m_worlds.end();
}

World& WorldSet::world(WorldId id) {
    auto it = m_worlds.find(id);
    if (it == m_worlds.end() || !it->second) {
        throw std::out_of_range("World not found");
    }
    return it->second->world;
}

const World& WorldSet::world(WorldId id) const {
    auto it = m_worlds.find(id);
    if (it == m_worlds.end() || !it->second) {
        throw std::out_of_range("World not found");
    }
    return it->second->world;
}

WorldView* WorldSet::findView(WorldId id) {
    auto it = m_worlds.find(id);
    if (it == m_worlds.end() || !it->second) {
        return nullptr;
    }
    return it->second->view.get();
}

const WorldView* WorldSet::findView(WorldId id) const {
    auto it = m_worlds.find(id);
    if (it == m_worlds.end() || !it->second) {
        return nullptr;
    }
    return it->second->view.get();
}

WorldView& WorldSet::view(WorldId id) {
    auto* view = findView(id);
    if (!view) {
        throw std::out_of_range("World view not found");
    }
    return *view;
}

void WorldSet::removeWorld(WorldId id) {
    m_worlds.erase(id);
}

void WorldSet::clear() {
    m_worlds.clear();
}

Persistence::PersistenceContext WorldSet::persistenceContext(WorldId id) const {
    const World& target = world(id);
    Persistence::PersistenceContext ctx;
    ctx.rootPath = m_persistenceRoot;
    ctx.preferredFormat = m_persistencePreferredFormat;
    ctx.zoneId = m_persistenceZoneId;
    ctx.policies = m_persistencePolicies;
    ctx.storage = m_persistenceStorage;
    ctx.providers = target.persistenceProvidersHandle();
    return ctx;
}

} // namespace Rigel::Voxel
