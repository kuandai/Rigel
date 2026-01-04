#include "Rigel/Application.h"
#include "Rigel/Asset/AssetManager.h"
#include "Rigel/Entity/Entity.h"
#include "Rigel/Entity/EntityFactory.h"
#include "Rigel/Entity/EntityModelLoader.h"
#include "Rigel/Entity/EntityPersistence.h"
#include "Rigel/Persistence/Backends/CR/CRChunkData.h"
#include "Rigel/Persistence/Backends/CR/CRChunkMapping.h"
#include "Rigel/Persistence/Backends/CR/CRFormat.h"
#include "Rigel/Persistence/Backends/CR/CRPaths.h"
#include "Rigel/Persistence/Backends/CR/CRSettings.h"
#include "Rigel/Persistence/Storage.h"
#include "Rigel/Voxel/WorldSet.h"
#include "Rigel/Voxel/WorldConfigProvider.h"
#include <spdlog/spdlog.h>
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include "Rigel/input/InputBindingsLoader.h"
#include "Rigel/input/InputDispatcher.h"
#include "Rigel/input/keypress.h"
#include "Rigel/version.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <filesystem>
#include <limits>
#include <map>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace Rigel {

namespace {

struct RaycastHit {
    glm::ivec3 block{};
    glm::ivec3 normal{};
    float distance = 0.0f;
};

constexpr float kDebugTargetSpan = 6.0f;
constexpr float kDebugDistance = 8.0f;
constexpr float kDebugAlpha = 0.35f;
constexpr int kDebugViewportSize = 130;
constexpr int kDebugViewportMargin = 12;
constexpr int kFrameGraphSamples = 180;
constexpr float kFrameGraphMaxMs = 50.0f;
constexpr float kFrameGraphHeight = 0.28f;
constexpr float kFrameGraphBottom = -0.95f;
constexpr float kMaxFrameTime = 0.05f;
constexpr const char* kDefaultZoneId = "rigel:default";

std::string mainWorldRootPath(Voxel::WorldId id) {
    return "saves/world_" + std::to_string(id);
}

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

void loadWorldFromDisk(Voxel::World& world,
                       Asset::AssetManager& assets,
                       Persistence::PersistenceService& service,
                       Persistence::PersistenceContext context,
                       uint32_t worldGenVersion) {
    namespace fs = std::filesystem;

    fs::path regionDir = fs::path(Persistence::Backends::CR::CRPaths::zoneRoot(kDefaultZoneId, context)) / "regions";
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
            Persistence::RegionKey key{std::string(kDefaultZoneId), rx, ry, rz};
            Persistence::ChunkRegionSnapshot region = service.loadRegion(key, context);
            for (const auto& chunk : region.chunks) {
                Persistence::Backends::CR::decodeChunkSnapshot(
                    chunk,
                    world.chunkManager(),
                    world.blockRegistry(),
                    worldGenVersion);
            }
        }
    }

    fs::path entityDir = fs::path(Persistence::Backends::CR::CRPaths::zoneRoot(kDefaultZoneId, context)) / "entities";
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
            Persistence::EntityRegionKey key{std::string(kDefaultZoneId), rx, ry, rz};
            Persistence::EntityRegionSnapshot region = service.loadEntities(key, context);
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
                     Persistence::PersistenceService& service,
                     Persistence::PersistenceContext context) {
    const auto& registry = world.blockRegistry();

    struct RegionSave {
        Persistence::RegionKey key;
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
            region.key = Persistence::RegionKey{std::string(kDefaultZoneId), rx, ry, rz};
        }
        region.dirtyChunks.push_back(coord);
    });

    for (auto& [coords, regionSave] : regions) {
        Persistence::ChunkRegionSnapshot existing = service.loadRegion(regionSave.key, context);
        using KeyTuple = std::tuple<int32_t, int32_t, int32_t>;
        std::map<KeyTuple, Persistence::ChunkSnapshot> merged;
        for (auto& snapshot : existing.chunks) {
            KeyTuple key{snapshot.key.x, snapshot.key.y, snapshot.key.z};
            merged.emplace(key, std::move(snapshot));
        }

        for (const Voxel::ChunkCoord& coord : regionSave.dirtyChunks) {
            for (int subchunkIndex = 0; subchunkIndex < 8; ++subchunkIndex) {
                Persistence::ChunkKey crKey =
                    Persistence::Backends::CR::toCRChunk({coord.x, coord.y, coord.z, subchunkIndex});
                KeyTuple key{crKey.x, crKey.y, crKey.z};
                merged.erase(key);
            }

            const Voxel::Chunk* chunk = world.chunkManager().getChunk(coord);
            if (!chunk) {
                continue;
            }
            auto snapshots = Persistence::Backends::CR::encodeRigelChunk(
                *chunk,
                registry,
                coord,
                kDefaultZoneId);
            for (auto& snapshot : snapshots) {
                KeyTuple key{snapshot.key.x, snapshot.key.y, snapshot.key.z};
                merged[key] = std::move(snapshot);
            }
        }

        Persistence::ChunkRegionSnapshot out;
        out.key = regionSave.key;
        out.chunks.reserve(merged.size());
        for (auto& entry : merged) {
            out.chunks.push_back(std::move(entry.second));
        }

        service.saveRegion(out, context);
    }

    struct EntityRegionSave {
        Persistence::EntityRegionKey key;
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
            region.key = Persistence::EntityRegionKey{std::string(kDefaultZoneId),
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
    fs::path entityDir = fs::path(Persistence::Backends::CR::CRPaths::zoneRoot(kDefaultZoneId, context)) / "entities";
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
            empty.key = Persistence::EntityRegionKey{std::string(kDefaultZoneId), rx, ry, rz};
            entityRegions.emplace(coord, std::move(empty));
        }
    }

    for (auto& [coord, region] : entityRegions) {
        Persistence::EntityRegionSnapshot snapshot;
        snapshot.key = region.key;
        if (!region.chunks.empty()) {
            snapshot.payload = Entity::encodeEntityRegionPayload(region.chunks);
        }
        service.saveEntities(snapshot, context);
    }

    Persistence::WorldSnapshot worldSnapshot;
    worldSnapshot.metadata.worldId = "world_" + std::to_string(world.id());
    worldSnapshot.metadata.displayName = worldSnapshot.metadata.worldId;
    worldSnapshot.zones.push_back(Persistence::ZoneMetadata{std::string(kDefaultZoneId), std::string(kDefaultZoneId)});
    service.saveWorld(worldSnapshot, Persistence::SaveScope::MetadataOnly, context);
}

float halton(uint32_t index, uint32_t base) {
    float f = 1.0f;
    float result = 0.0f;
    while (index > 0) {
        f /= static_cast<float>(base);
        result += f * static_cast<float>(index % base);
        index /= base;
    }
    return result;
}

constexpr float kCubeVertices[] = {
    // +X
    0.5f, -0.5f, -0.5f,
    0.5f,  0.5f, -0.5f,
    0.5f,  0.5f,  0.5f,
    0.5f, -0.5f, -0.5f,
    0.5f,  0.5f,  0.5f,
    0.5f, -0.5f,  0.5f,
    // -X
    -0.5f, -0.5f,  0.5f,
    -0.5f,  0.5f,  0.5f,
    -0.5f,  0.5f, -0.5f,
    -0.5f, -0.5f,  0.5f,
    -0.5f,  0.5f, -0.5f,
    -0.5f, -0.5f, -0.5f,
    // +Y
    -0.5f,  0.5f, -0.5f,
    0.5f,  0.5f, -0.5f,
    0.5f,  0.5f,  0.5f,
    -0.5f,  0.5f, -0.5f,
    0.5f,  0.5f,  0.5f,
    -0.5f,  0.5f,  0.5f,
    // -Y
    -0.5f, -0.5f,  0.5f,
    0.5f, -0.5f,  0.5f,
    0.5f, -0.5f, -0.5f,
    -0.5f, -0.5f,  0.5f,
    0.5f, -0.5f, -0.5f,
    -0.5f, -0.5f, -0.5f,
    // +Z
    -0.5f, -0.5f,  0.5f,
    -0.5f,  0.5f,  0.5f,
    0.5f,  0.5f,  0.5f,
    -0.5f, -0.5f,  0.5f,
    0.5f,  0.5f,  0.5f,
    0.5f, -0.5f,  0.5f,
    // -Z
    0.5f, -0.5f, -0.5f,
    0.5f,  0.5f, -0.5f,
    -0.5f,  0.5f, -0.5f,
    0.5f, -0.5f, -0.5f,
    -0.5f,  0.5f, -0.5f,
    -0.5f, -0.5f, -0.5f
};


bool raycastBlock(const Voxel::World& world,
                  const glm::vec3& origin,
                  const glm::vec3& direction,
                  float maxDistance,
                  RaycastHit& outHit) {
    float dirLen = glm::length(direction);
    if (dirLen <= 0.0001f) {
        return false;
    }

    glm::vec3 dir = direction / dirLen;
    glm::ivec3 blockPos(
        static_cast<int>(std::floor(origin.x)),
        static_cast<int>(std::floor(origin.y)),
        static_cast<int>(std::floor(origin.z))
    );

    glm::ivec3 step(0);
    glm::vec3 tMax(0.0f);
    glm::vec3 tDelta(0.0f);

    auto setupAxis = [](float originCoord, float dirCoord, int blockCoord,
                        int& stepOut, float& tMaxOut, float& tDeltaOut) {
        if (dirCoord > 0.0f) {
            stepOut = 1;
            float nextBoundary = static_cast<float>(blockCoord + 1);
            tMaxOut = (nextBoundary - originCoord) / dirCoord;
            tDeltaOut = 1.0f / dirCoord;
        } else if (dirCoord < 0.0f) {
            stepOut = -1;
            float nextBoundary = static_cast<float>(blockCoord);
            tMaxOut = (originCoord - nextBoundary) / -dirCoord;
            tDeltaOut = 1.0f / -dirCoord;
        } else {
            stepOut = 0;
            tMaxOut = std::numeric_limits<float>::infinity();
            tDeltaOut = std::numeric_limits<float>::infinity();
        }
    };

    setupAxis(origin.x, dir.x, blockPos.x, step.x, tMax.x, tDelta.x);
    setupAxis(origin.y, dir.y, blockPos.y, step.y, tMax.y, tDelta.y);
    setupAxis(origin.z, dir.z, blockPos.z, step.z, tMax.z, tDelta.z);

    glm::ivec3 normal(0);
    float t = 0.0f;

    while (t <= maxDistance) {
        if (!world.getBlock(blockPos.x, blockPos.y, blockPos.z).isAir()) {
            outHit.block = blockPos;
            outHit.normal = normal;
            outHit.distance = t;
            return true;
        }

        if (tMax.x < tMax.y) {
            if (tMax.x < tMax.z) {
                blockPos.x += step.x;
                t = tMax.x;
                tMax.x += tDelta.x;
                normal = glm::ivec3(-step.x, 0, 0);
            } else {
                blockPos.z += step.z;
                t = tMax.z;
                tMax.z += tDelta.z;
                normal = glm::ivec3(0, 0, -step.z);
            }
        } else {
            if (tMax.y < tMax.z) {
                blockPos.y += step.y;
                t = tMax.y;
                tMax.y += tDelta.y;
                normal = glm::ivec3(0, -step.y, 0);
            } else {
                blockPos.z += step.z;
                t = tMax.z;
                tMax.z += tDelta.z;
                normal = glm::ivec3(0, 0, -step.z);
            }
        }
    }

    return false;
}

int findFirstAirY(const Voxel::WorldGenerator& generator,
                  const Voxel::WorldGenConfig& config,
                  int worldX,
                  int worldZ) {
    int minY = config.world.minY;
    int maxY = config.world.maxY;

    Voxel::ChunkCoord baseCoord = Voxel::worldToChunk(worldX, 0, worldZ);
    int localX = 0;
    int localZ = 0;
    int localY = 0;
    Voxel::worldToLocal(worldX, 0, worldZ, localX, localY, localZ);

    int minChunkY = Voxel::worldToChunk(0, minY, 0).y;
    int maxChunkY = Voxel::worldToChunk(0, maxY, 0).y;

    for (int cy = maxChunkY; cy >= minChunkY; --cy) {
        Voxel::ChunkCoord coord{baseCoord.x, cy, baseCoord.z};
        Voxel::ChunkBuffer buffer;
        generator.generate(coord, buffer, nullptr);

        for (int ly = Voxel::Chunk::SIZE - 1; ly >= 0; --ly) {
            int worldY = cy * Voxel::Chunk::SIZE + ly;
            if (worldY > maxY || worldY < minY) {
                continue;
            }
            if (!buffer.at(localX, ly, localZ).isAir()) {
                int airY = worldY + 1;
                if (airY <= maxY) {
                    return airY;
                }
                return maxY + 1;
            }
        }
    }

    return maxY;
}

Voxel::ConfigProvider makeWorldConfigProvider(Asset::AssetManager& assets,
                                              Voxel::WorldId worldId) {
    Voxel::ConfigProvider provider;
    provider.addSource(
        std::make_unique<Voxel::EmbeddedConfigSource>(assets, "raw/world_config")
    );
    provider.addSource(
        std::make_unique<Voxel::FileConfigSource>("config/world_generation.yaml")
    );
    provider.addSource(
        std::make_unique<Voxel::FileConfigSource>("world_generation.yaml")
    );
    provider.addSource(
        std::make_unique<Voxel::FileConfigSource>(
            "config/worlds/" + std::to_string(worldId) + "/world_generation.yaml")
    );
    return provider;
}

Voxel::ConfigProvider makeRenderConfigProvider(Asset::AssetManager& assets,
                                               Voxel::WorldId worldId) {
    Voxel::ConfigProvider provider;
    provider.addSource(
        std::make_unique<Voxel::EmbeddedConfigSource>(assets, "raw/render_config")
    );
    provider.addSource(
        std::make_unique<Voxel::FileConfigSource>("config/render.yaml")
    );
    provider.addSource(
        std::make_unique<Voxel::FileConfigSource>("render.yaml")
    );
    provider.addSource(
        std::make_unique<Voxel::FileConfigSource>(
            "config/worlds/" + std::to_string(worldId) + "/render.yaml")
    );
    return provider;
}

} // namespace

struct Application::Impl {
    struct DebugOverlayListener : InputListener {
        Impl* owner = nullptr;

        void onActionReleased(std::string_view action) override {
            if (!owner) {
                return;
            }
            if (action == "debug_overlay") {
                owner->debugOverlayEnabled = !owner->debugOverlayEnabled;
            }
        }
    };

    struct DebugField {
        GLuint vao = 0;
        std::array<GLuint, 4> vbos{};
        Asset::Handle<Asset::ShaderAsset> shader;
        GLint locViewProjection = -1;
        GLint locFieldOrigin = -1;
        GLint locFieldRight = -1;
        GLint locFieldUp = -1;
        GLint locFieldForward = -1;
        GLint locCellSize = -1;
        GLint locColor = -1;
        bool initialized = false;
    };

    struct FrameTimeGraph {
        GLuint vao = 0;
        GLuint vbo = 0;
        Asset::Handle<Asset::ShaderAsset> shader;
        GLint locColor = -1;
        std::vector<float> samples;
        size_t cursor = 0;
        size_t filled = 0;
        bool initialized = false;
    };

    struct EntityDebug {
        GLuint vao = 0;
        GLuint vbo = 0;
        Asset::Handle<Asset::ShaderAsset> shader;
        GLint locViewProjection = -1;
        GLint locFieldOrigin = -1;
        GLint locFieldRight = -1;
        GLint locFieldUp = -1;
        GLint locFieldForward = -1;
        GLint locCellSize = -1;
        GLint locColor = -1;
        bool initialized = false;
    };

    struct TemporalAA {
        GLuint sceneFbo = 0;
        GLuint sceneColor = 0;
        GLuint sceneDepth = 0;
        GLuint resolveFbo = 0;
        std::array<GLuint, 2> history{};
        std::array<GLuint, 2> historyDepth{};
        GLuint quadVao = 0;
        Asset::Handle<Asset::ShaderAsset> shader;
        GLint locCurrentColor = -1;
        GLint locCurrentDepth = -1;
        GLint locHistory = -1;
        GLint locHistoryDepth = -1;
        GLint locCurrentJitter = -1;
        GLint locInvViewProjection = -1;
        GLint locPrevViewProjection = -1;
        GLint locHistoryBlend = -1;
        GLint locHistoryValid = -1;
        GLint locTexelSize = -1;
        int width = 0;
        int height = 0;
        int historyIndex = 0;
        bool initialized = false;
        bool historyValid = false;
        glm::mat4 prevViewProjection{1.0f};
        uint64_t frameIndex = 0;
    };

    GLFWwindow* window = nullptr;
    Asset::AssetManager assets;
    Voxel::WorldSet worldSet;
    Voxel::WorldId activeWorldId = Voxel::WorldSet::defaultWorldId();
    Voxel::World* world = nullptr;
    Voxel::WorldView* worldView = nullptr;
    bool worldReady = false;
    glm::vec3 cameraPos = glm::vec3(48.0f, 32.0f, 48.0f);
    glm::vec3 cameraTarget = glm::vec3(8.0f, 0.0f, 8.0f);
    glm::vec3 cameraForward = glm::vec3(0.0f, 0.0f, -1.0f);
    glm::vec3 cameraRight = glm::vec3(1.0f, 0.0f, 0.0f);
    glm::vec3 cameraUp = glm::vec3(0.0f, 1.0f, 0.0f);
    float cameraYaw = -135.0f;
    float cameraPitch = -20.0f;
    float moveSpeed = 10.0f;
    float mouseSensitivity = 0.12f;
    bool cursorCaptured = true;
    bool firstMouse = true;
    double lastMouseX = 0.0;
    double lastMouseY = 0.0;
    double lastTime = 0.0;
    bool windowFocused = true;
    bool pendingTimeReset = false;
    bool benchmarkEnabled = false;
    double benchmarkStartTime = 0.0;
    Voxel::ChunkBenchmarkStats benchmark;
    Voxel::BlockID placeBlock = Voxel::BlockRegistry::airId();
    bool lastLeftDown = false;
    bool lastRightDown = false;
    DebugField debugField;
    FrameTimeGraph frameGraph;
    EntityDebug entityDebug;
    TemporalAA taa;
    std::vector<Voxel::ChunkStreamer::DebugChunkState> debugStates;
    float debugDistance = kDebugDistance;
    bool debugOverlayEnabled = true;
    std::shared_ptr<InputBindings> inputBindings;
    InputDispatcher inputDispatcher;
    DebugOverlayListener debugOverlayListener;

    void setCursorCaptured(bool captured) {
        cursorCaptured = captured;
        if (!window) {
            return;
        }

        glfwSetInputMode(window, GLFW_CURSOR,
                         captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
        if (glfwRawMouseMotionSupported()) {
            glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION,
                             captured ? GLFW_TRUE : GLFW_FALSE);
        }
        firstMouse = true;
    }

    void updateCamera(float dt) {
        if (dt <= 0.0f) {
            return;
        }

        cameraPitch = std::clamp(cameraPitch, -89.0f, 89.0f);

        float yawRad = glm::radians(cameraYaw);
        float pitchRad = glm::radians(cameraPitch);
        glm::vec3 forward{
            std::cos(yawRad) * std::cos(pitchRad),
            std::sin(pitchRad),
            std::sin(yawRad) * std::cos(pitchRad)
        };
        forward = glm::normalize(forward);

        glm::vec3 worldUp(0.0f, 1.0f, 0.0f);
        glm::vec3 right = glm::normalize(glm::cross(forward, worldUp));
        glm::vec3 up = glm::normalize(glm::cross(right, forward));

        glm::vec3 move(0.0f);
        if (inputDispatcher.isActionPressed("move_forward")) {
            move += forward;
        }
        if (inputDispatcher.isActionPressed("move_backward")) {
            move -= forward;
        }
        if (inputDispatcher.isActionPressed("move_right")) {
            move += right;
        }
        if (inputDispatcher.isActionPressed("move_left")) {
            move -= right;
        }
        if (inputDispatcher.isActionPressed("move_up")) {
            move += worldUp;
        }
        if (inputDispatcher.isActionPressed("move_down")) {
            move -= worldUp;
        }

        float speed = moveSpeed;
        if (inputDispatcher.isActionPressed("sprint")) {
            speed *= 2.0f;
        }

        if (glm::length(move) > 0.0f) {
            cameraPos += glm::normalize(move) * speed * dt;
        }

        cameraForward = forward;
        cameraRight = right;
        cameraUp = up;
        cameraTarget = cameraPos + forward;
    }

    void initDebugField() {
        try {
            debugField.shader = assets.get<Asset::ShaderAsset>("shaders/chunk_debug");
        } catch (const std::exception& e) {
            spdlog::warn("Debug chunk shader unavailable: {}", e.what());
            return;
        }

        glGenVertexArrays(1, &debugField.vao);
        glGenBuffers(static_cast<GLsizei>(debugField.vbos.size()), debugField.vbos.data());

        glBindVertexArray(debugField.vao);

        glBindVertexArray(0);

        debugField.locViewProjection = debugField.shader->uniform("u_viewProjection");
        debugField.locFieldOrigin = debugField.shader->uniform("u_fieldOrigin");
        debugField.locFieldRight = debugField.shader->uniform("u_fieldRight");
        debugField.locFieldUp = debugField.shader->uniform("u_fieldUp");
        debugField.locFieldForward = debugField.shader->uniform("u_fieldForward");
        debugField.locCellSize = debugField.shader->uniform("u_cellSize");
        debugField.locColor = debugField.shader->uniform("u_color");

        debugField.initialized = true;
    }

    void initFrameGraph() {
        try {
            frameGraph.shader = assets.get<Asset::ShaderAsset>("shaders/frame_graph");
        } catch (const std::exception& e) {
            spdlog::warn("Frame graph shader unavailable: {}", e.what());
            return;
        }

        glGenVertexArrays(1, &frameGraph.vao);
        glGenBuffers(1, &frameGraph.vbo);
        frameGraph.samples.assign(kFrameGraphSamples, 0.0f);
        frameGraph.initialized = true;
        frameGraph.locColor = frameGraph.shader->uniform("u_color");
    }

    void initEntityDebug() {
        try {
            entityDebug.shader = assets.get<Asset::ShaderAsset>("shaders/entity_debug");
        } catch (const std::exception& e) {
            spdlog::warn("Entity debug shader unavailable: {}", e.what());
            return;
        }

        glGenVertexArrays(1, &entityDebug.vao);
        glGenBuffers(1, &entityDebug.vbo);
        glBindVertexArray(entityDebug.vao);
        glBindVertexArray(0);

        entityDebug.locViewProjection = entityDebug.shader->uniform("u_viewProjection");
        entityDebug.locFieldOrigin = entityDebug.shader->uniform("u_fieldOrigin");
        entityDebug.locFieldRight = entityDebug.shader->uniform("u_fieldRight");
        entityDebug.locFieldUp = entityDebug.shader->uniform("u_fieldUp");
        entityDebug.locFieldForward = entityDebug.shader->uniform("u_fieldForward");
        entityDebug.locCellSize = entityDebug.shader->uniform("u_cellSize");
        entityDebug.locColor = entityDebug.shader->uniform("u_color");
        entityDebug.initialized = true;
    }

    void releaseTaaTargets() {
        if (taa.sceneFbo != 0) {
            glDeleteFramebuffers(1, &taa.sceneFbo);
            taa.sceneFbo = 0;
        }
        if (taa.resolveFbo != 0) {
            glDeleteFramebuffers(1, &taa.resolveFbo);
            taa.resolveFbo = 0;
        }
        if (taa.sceneColor != 0) {
            glDeleteTextures(1, &taa.sceneColor);
            taa.sceneColor = 0;
        }
        if (taa.sceneDepth != 0) {
            glDeleteTextures(1, &taa.sceneDepth);
            taa.sceneDepth = 0;
        }
        for (GLuint& tex : taa.history) {
            if (tex != 0) {
                glDeleteTextures(1, &tex);
                tex = 0;
            }
        }
        for (GLuint& tex : taa.historyDepth) {
            if (tex != 0) {
                glDeleteTextures(1, &tex);
                tex = 0;
            }
        }
        taa.width = 0;
        taa.height = 0;
        taa.historyValid = false;
    }

    void initTaa() {
        if (taa.initialized) {
            return;
        }
        try {
            taa.shader = assets.get<Asset::ShaderAsset>("shaders/taa_resolve");
        } catch (const std::exception& e) {
            spdlog::warn("TAA shader unavailable: {}", e.what());
            return;
        }

        glGenVertexArrays(1, &taa.quadVao);
        glBindVertexArray(taa.quadVao);
        glBindVertexArray(0);

        taa.locCurrentColor = taa.shader->uniform("u_currentColor");
        taa.locCurrentDepth = taa.shader->uniform("u_currentDepth");
        taa.locHistory = taa.shader->uniform("u_historyColor");
        taa.locHistoryDepth = taa.shader->uniform("u_historyDepth");
        taa.locCurrentJitter = taa.shader->uniform("u_currentJitter");
        taa.locInvViewProjection = taa.shader->uniform("u_invViewProjection");
        taa.locPrevViewProjection = taa.shader->uniform("u_prevViewProjection");
        taa.locHistoryBlend = taa.shader->uniform("u_historyBlend");
        taa.locHistoryValid = taa.shader->uniform("u_historyValid");
        taa.locTexelSize = taa.shader->uniform("u_texelSize");

        taa.initialized = true;
    }

    void ensureTaaTargets(int width, int height) {
        initTaa();
        if (!taa.initialized || width <= 0 || height <= 0) {
            return;
        }
        if (taa.width == width && taa.height == height && taa.sceneFbo != 0) {
            return;
        }

        releaseTaaTargets();

        taa.width = width;
        taa.height = height;
        taa.historyValid = false;

        glGenTextures(1, &taa.sceneColor);
        glBindTexture(GL_TEXTURE_2D, taa.sceneColor);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0,
                     GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glGenTextures(1, &taa.sceneDepth);
        glBindTexture(GL_TEXTURE_2D, taa.sceneDepth);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, width, height, 0,
                     GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);

        glGenTextures(2, taa.history.data());
        for (GLuint& tex : taa.history) {
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0,
                         GL_RGBA, GL_FLOAT, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        glGenTextures(2, taa.historyDepth.data());
        for (GLuint& tex : taa.historyDepth) {
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, width, height, 0,
                         GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
        }

        glGenFramebuffers(1, &taa.sceneFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, taa.sceneFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, taa.sceneColor, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                               GL_TEXTURE_2D, taa.sceneDepth, 0);
        GLenum drawBuffer = GL_COLOR_ATTACHMENT0;
        glDrawBuffers(1, &drawBuffer);
        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            spdlog::warn("TAA scene FBO incomplete: status=0x{:X}", status);
        }

        glGenFramebuffers(1, &taa.resolveFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    glm::vec2 nextJitter(int width, int height, float scale) {
        if (width <= 0 || height <= 0) {
            return glm::vec2(0.0f);
        }
        ++taa.frameIndex;
        float jx = halton(static_cast<uint32_t>(taa.frameIndex), 2) - 0.5f;
        float jy = halton(static_cast<uint32_t>(taa.frameIndex), 3) - 0.5f;
        float offsetX = jx * scale * 2.0f / static_cast<float>(width);
        float offsetY = jy * scale * 2.0f / static_cast<float>(height);
        return glm::vec2(offsetX, offsetY);
    }

    bool resolveTaa(const glm::mat4& invViewProjection,
                    const glm::mat4& viewProjection,
                    const glm::vec2& jitterUv,
                    float blend) {
        if (!taa.initialized || taa.resolveFbo == 0 || taa.sceneColor == 0) {
            return false;
        }

        int readIndex = taa.historyIndex;
        int writeIndex = (taa.historyIndex + 1) % 2;

        glBindFramebuffer(GL_FRAMEBUFFER, taa.resolveFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, taa.history[writeIndex], 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                               GL_TEXTURE_2D, taa.historyDepth[writeIndex], 0);
        GLenum drawBuffer = GL_COLOR_ATTACHMENT0;
        glDrawBuffers(1, &drawBuffer);

        taa.shader->bind();
        if (taa.locCurrentColor >= 0) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, taa.sceneColor);
            glUniform1i(taa.locCurrentColor, 0);
        }
        if (taa.locCurrentDepth >= 0) {
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, taa.sceneDepth);
            glUniform1i(taa.locCurrentDepth, 1);
        }
        if (taa.locHistory >= 0) {
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, taa.history[readIndex]);
            glUniform1i(taa.locHistory, 2);
        }
        if (taa.locHistoryDepth >= 0) {
            glActiveTexture(GL_TEXTURE3);
            glBindTexture(GL_TEXTURE_2D, taa.historyDepth[readIndex]);
            glUniform1i(taa.locHistoryDepth, 3);
        }
        if (taa.locCurrentJitter >= 0) {
            glUniform2fv(taa.locCurrentJitter, 1, glm::value_ptr(jitterUv));
        }
        if (taa.locInvViewProjection >= 0) {
            glUniformMatrix4fv(taa.locInvViewProjection, 1, GL_FALSE,
                               glm::value_ptr(invViewProjection));
        }
        if (taa.locPrevViewProjection >= 0) {
            glUniformMatrix4fv(taa.locPrevViewProjection, 1, GL_FALSE,
                               glm::value_ptr(taa.prevViewProjection));
        }
        if (taa.locHistoryBlend >= 0) {
            glUniform1f(taa.locHistoryBlend, blend);
        }
        if (taa.locHistoryValid >= 0) {
            glUniform1i(taa.locHistoryValid, taa.historyValid ? 1 : 0);
        }
        if (taa.locTexelSize >= 0) {
            glm::vec2 texelSize(1.0f / static_cast<float>(taa.width),
                                1.0f / static_cast<float>(taa.height));
            glUniform2fv(taa.locTexelSize, 1, glm::value_ptr(texelSize));
        }

        glDisable(GL_BLEND);
        glDisable(GL_CULL_FACE);
        glDepthMask(GL_FALSE);
        glDisable(GL_DEPTH_TEST);

        glBindVertexArray(taa.quadVao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindVertexArray(0);
        glUseProgram(0);

        glBindFramebuffer(GL_READ_FRAMEBUFFER, taa.resolveFbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glBlitFramebuffer(0, 0, taa.width, taa.height,
                          0, 0, taa.width, taa.height,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, taa.sceneFbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glBlitFramebuffer(0, 0, taa.width, taa.height,
                          0, 0, taa.width, taa.height,
                          GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, taa.sceneFbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, taa.resolveFbo);
        glBlitFramebuffer(0, 0, taa.width, taa.height,
                          0, 0, taa.width, taa.height,
                          GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_TRUE);
        glEnable(GL_CULL_FACE);
        glDisable(GL_BLEND);
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

        taa.historyValid = true;
        taa.historyIndex = writeIndex;
        taa.prevViewProjection = viewProjection;

        return true;
    }

    void recordFrameTime(float seconds) {
        if (!frameGraph.initialized || seconds <= 0.0f) {
            return;
        }
        float ms = seconds * 1000.0f;
        frameGraph.samples[frameGraph.cursor] = ms;
        frameGraph.cursor = (frameGraph.cursor + 1) % frameGraph.samples.size();
        frameGraph.filled = std::min(frameGraph.samples.size(), frameGraph.filled + 1);
    }

    void renderFrameGraph() {
        if (!debugOverlayEnabled || !frameGraph.initialized || frameGraph.filled == 0) {
            return;
        }

        const size_t sampleCount = frameGraph.samples.size();
        const float barWidth = 2.0f / static_cast<float>(sampleCount);
        const float baseY = kFrameGraphBottom;
        const float topSpan = kFrameGraphHeight;

        std::vector<glm::vec2> vertices;
        vertices.reserve(frameGraph.filled * 6);

        for (size_t i = 0; i < frameGraph.filled; ++i) {
            size_t sampleIndex = (frameGraph.cursor + sampleCount - 1 - i) % sampleCount;
            float ms = frameGraph.samples[sampleIndex];
            float t = std::min(ms, kFrameGraphMaxMs) / kFrameGraphMaxMs;
            float height = t * topSpan;

            float x1 = 1.0f - static_cast<float>(i) * barWidth;
            float x0 = x1 - barWidth;
            float y0 = baseY;
            float y1 = baseY + height;

            vertices.emplace_back(x0, y0);
            vertices.emplace_back(x1, y0);
            vertices.emplace_back(x1, y1);

            vertices.emplace_back(x0, y0);
            vertices.emplace_back(x1, y1);
            vertices.emplace_back(x0, y1);
        }

        frameGraph.shader->bind();
        if (frameGraph.locColor >= 0) {
            glm::vec4 color(0.2f, 0.9f, 0.9f, 0.85f);
            glUniform4fv(frameGraph.locColor, 1, glm::value_ptr(color));
        }

        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        glBindVertexArray(frameGraph.vao);
        glBindBuffer(GL_ARRAY_BUFFER, frameGraph.vbo);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(vertices.size() * sizeof(glm::vec2)),
                     vertices.data(),
                     GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(glm::vec2), nullptr);
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(vertices.size()));

        glDisable(GL_BLEND);
        glDepthMask(GL_TRUE);
        glEnable(GL_DEPTH_TEST);
        glBindVertexArray(0);
        glUseProgram(0);
    }

    void renderDebugField(const glm::vec3& viewForward,
                          int viewportWidth,
                          int viewportHeight) {
        if (!debugOverlayEnabled || !debugField.initialized || !worldView) {
            return;
        }

        worldView->getChunkDebugStates(debugStates);
        if (debugStates.empty()) {
            return;
        }

        int radius = std::max(0, worldView->viewDistanceChunks());
        int diameter = radius * 2 + 1;
        if (diameter <= 0) {
            return;
        }

        float cellSize = kDebugTargetSpan / static_cast<float>(diameter);

        auto centerCoord = Voxel::worldToChunk(
            static_cast<int>(std::floor(cameraPos.x)),
            static_cast<int>(std::floor(cameraPos.y)),
            static_cast<int>(std::floor(cameraPos.z))
        );

        std::unordered_map<Voxel::ChunkCoord,
                           Voxel::ChunkStreamer::DebugState,
                           Voxel::ChunkCoordHash> stateMap;
        stateMap.reserve(debugStates.size());
        std::array<std::unordered_set<Voxel::ChunkCoord, Voxel::ChunkCoordHash>, 4> occupancy;
        for (auto& set : occupancy) {
            set.reserve(debugStates.size());
        }

        for (const auto& entry : debugStates) {
            int dx = entry.coord.x - centerCoord.x;
            int dy = entry.coord.y - centerCoord.y;
            int dz = entry.coord.z - centerCoord.z;

            if (std::abs(dx) > radius || std::abs(dy) > radius || std::abs(dz) > radius) {
                continue;
            }
            Voxel::ChunkCoord offset{dx, dy, dz};
            stateMap[offset] = entry.state;
            switch (entry.state) {
                case Voxel::ChunkStreamer::DebugState::QueuedGen:
                    occupancy[0].insert(offset);
                    break;
                case Voxel::ChunkStreamer::DebugState::ReadyData:
                    occupancy[1].insert(offset);
                    break;
                case Voxel::ChunkStreamer::DebugState::QueuedMesh:
                    occupancy[2].insert(offset);
                    break;
                case Voxel::ChunkStreamer::DebugState::ReadyMesh:
                    occupancy[3].insert(offset);
                    break;
            }
        }

        if (stateMap.empty()) {
            return;
        }

        std::array<std::vector<glm::vec3>, 4> meshVertices;
        std::array<glm::vec4, 4> colors = {
            glm::vec4(1.0f, 0.2f, 0.2f, kDebugAlpha),
            glm::vec4(1.0f, 0.9f, 0.2f, kDebugAlpha),
            glm::vec4(0.2f, 0.8f, 1.0f, kDebugAlpha),
            glm::vec4(0.2f, 1.0f, 0.3f, kDebugAlpha)
        };
        std::array<std::array<int, 3>, 6> offsets = {{
            { 1, 0, 0},
            {-1, 0, 0},
            { 0, 1, 0},
            { 0,-1, 0},
            { 0, 0, 1},
            { 0, 0,-1}
        }};

        for (const auto& [coord, state] : stateMap) {
            int stateIdx = 0;
            switch (state) {
                case Voxel::ChunkStreamer::DebugState::QueuedGen:
                    stateIdx = 0;
                    break;
                case Voxel::ChunkStreamer::DebugState::ReadyData:
                    stateIdx = 1;
                    break;
                case Voxel::ChunkStreamer::DebugState::QueuedMesh:
                    stateIdx = 2;
                    break;
                case Voxel::ChunkStreamer::DebugState::ReadyMesh:
                    stateIdx = 3;
                    break;
            }

            for (int face = 0; face < 6; ++face) {
                Voxel::ChunkCoord neighbor{
                    coord.x + offsets[face][0],
                    coord.y + offsets[face][1],
                    coord.z + offsets[face][2]
                };
                if (occupancy[stateIdx].find(neighbor) != occupancy[stateIdx].end()) {
                    continue;
                }

                size_t base = static_cast<size_t>(face) * 18;
                for (int v = 0; v < 6; ++v) {
                    float x = kCubeVertices[base + v * 3 + 0] + static_cast<float>(coord.x);
                    float y = kCubeVertices[base + v * 3 + 1] + static_cast<float>(coord.y);
                    float z = kCubeVertices[base + v * 3 + 2] + static_cast<float>(coord.z);
                    meshVertices[stateIdx].push_back(glm::vec3(x, y, z));
                }
            }
        }

        GLint previousViewport[4] = {};
        glGetIntegerv(GL_VIEWPORT, previousViewport);

        int viewportSize = std::min(kDebugViewportSize, std::min(viewportWidth, viewportHeight));
        int marginX = std::min(kDebugViewportMargin, std::max(0, viewportWidth - viewportSize));
        int marginY = std::min(kDebugViewportMargin, std::max(0, viewportHeight - viewportSize));
        int viewportX = marginX;
        int viewportY = std::max(0, viewportHeight - viewportSize - marginY);
        glViewport(viewportX, viewportY, viewportSize, viewportSize);

        float renderDistance = worldView->renderConfig().renderDistance;
        float farPlane = std::max(500.0f, renderDistance + static_cast<float>(Voxel::Chunk::SIZE));
        glm::mat4 debugProjection = glm::perspective(glm::radians(60.0f), 1.0f, 0.1f, farPlane);
        glm::mat4 debugView = glm::lookAt(
            cameraPos,
            cameraTarget,
            glm::vec3(0.0f, 1.0f, 0.0f)
        );
        glm::mat4 debugViewProjection = debugProjection * debugView;

        debugField.shader->bind();
        if (debugField.locViewProjection >= 0) {
            glUniformMatrix4fv(debugField.locViewProjection, 1, GL_FALSE, glm::value_ptr(debugViewProjection));
        }
        glm::vec3 fieldOrigin = cameraPos + viewForward * debugDistance;
        if (debugField.locFieldOrigin >= 0) {
            glUniform3fv(debugField.locFieldOrigin, 1, glm::value_ptr(fieldOrigin));
        }
        if (debugField.locFieldRight >= 0) {
            glUniform3fv(debugField.locFieldRight, 1, glm::value_ptr(glm::vec3(1.0f, 0.0f, 0.0f)));
        }
        if (debugField.locFieldUp >= 0) {
            glUniform3fv(debugField.locFieldUp, 1, glm::value_ptr(glm::vec3(0.0f, 1.0f, 0.0f)));
        }
        if (debugField.locFieldForward >= 0) {
            glUniform3fv(debugField.locFieldForward, 1, glm::value_ptr(glm::vec3(0.0f, 0.0f, 1.0f)));
        }
        if (debugField.locCellSize >= 0) {
            glUniform1f(debugField.locCellSize, cellSize);
        }

        glDisable(GL_CULL_FACE);
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        glBindVertexArray(debugField.vao);
        for (size_t i = 0; i < meshVertices.size(); ++i) {
            if (meshVertices[i].empty()) {
                continue;
            }

            if (debugField.locColor >= 0) {
                glUniform4fv(debugField.locColor, 1, glm::value_ptr(colors[i]));
            }

            glBindBuffer(GL_ARRAY_BUFFER, debugField.vbos[i]);
            glBufferData(GL_ARRAY_BUFFER,
                         static_cast<GLsizeiptr>(meshVertices[i].size() * sizeof(glm::vec3)),
                         meshVertices[i].data(),
                         GL_DYNAMIC_DRAW);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), nullptr);

            glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(meshVertices[i].size()));
        }

        glDisable(GL_BLEND);
        glDepthMask(GL_TRUE);
        glEnable(GL_DEPTH_TEST);
        glBindVertexArray(0);
        glUseProgram(0);
        glViewport(previousViewport[0],
                   previousViewport[1],
                   previousViewport[2],
                   previousViewport[3]);
    }

    void renderEntityDebugBoxes(const glm::mat4& view, const glm::mat4& projection) {
        if (!debugOverlayEnabled || !entityDebug.initialized || !world || world->entities().size() == 0) {
            return;
        }

        constexpr size_t kCubeVertexCount = sizeof(kCubeVertices) / (sizeof(float) * 3);
        std::vector<glm::vec3> vertices;
        vertices.reserve(world->entities().size() * kCubeVertexCount);

        world->entities().forEach([&](Entity::Entity& entity) {
            const Entity::Aabb& bounds = entity.worldBounds();
            glm::vec3 center = (bounds.min + bounds.max) * 0.5f;
            glm::vec3 size = bounds.max - bounds.min;

            for (size_t i = 0; i < kCubeVertexCount; ++i) {
                size_t base = i * 3;
                glm::vec3 local(kCubeVertices[base + 0],
                                kCubeVertices[base + 1],
                                kCubeVertices[base + 2]);
                vertices.push_back(center + local * size);
            }
        });

        if (vertices.empty()) {
            return;
        }

        glm::mat4 viewProjection = projection * view;

        entityDebug.shader->bind();
        if (entityDebug.locViewProjection >= 0) {
            glUniformMatrix4fv(entityDebug.locViewProjection, 1, GL_FALSE,
                               glm::value_ptr(viewProjection));
        }
        if (entityDebug.locFieldOrigin >= 0) {
            glUniform3fv(entityDebug.locFieldOrigin, 1, glm::value_ptr(glm::vec3(0.0f)));
        }
        if (entityDebug.locFieldRight >= 0) {
            glUniform3fv(entityDebug.locFieldRight, 1, glm::value_ptr(glm::vec3(1.0f, 0.0f, 0.0f)));
        }
        if (entityDebug.locFieldUp >= 0) {
            glUniform3fv(entityDebug.locFieldUp, 1, glm::value_ptr(glm::vec3(0.0f, 1.0f, 0.0f)));
        }
        if (entityDebug.locFieldForward >= 0) {
            glUniform3fv(entityDebug.locFieldForward, 1, glm::value_ptr(glm::vec3(0.0f, 0.0f, 1.0f)));
        }
        if (entityDebug.locCellSize >= 0) {
            glUniform1f(entityDebug.locCellSize, 1.0f);
        }
        if (entityDebug.locColor >= 0) {
            glm::vec4 color(1.0f, 0.1f, 0.1f, 0.9f);
            glUniform4fv(entityDebug.locColor, 1, glm::value_ptr(color));
        }

        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glDisable(GL_CULL_FACE);
        glDisable(GL_BLEND);
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);

        glBindVertexArray(entityDebug.vao);
        glBindBuffer(GL_ARRAY_BUFFER, entityDebug.vbo);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(vertices.size() * sizeof(glm::vec3)),
                     vertices.data(),
                     GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), nullptr);
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(vertices.size()));

        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        glDepthMask(GL_TRUE);
        glEnable(GL_CULL_FACE);
        glBindVertexArray(0);
        glUseProgram(0);
    }

};

Application::Application() : m_impl(std::make_unique<Impl>()) {
    m_impl->debugOverlayListener.owner = m_impl.get();
    #ifdef DEBUG
    spdlog::info("Rigel v{} Developer Preview", RIGEL_VERSION);
    #else
    spdlog::info("Rigel v{}", RIGEL_VERSION);
    #endif

    // Initialize GLFW
    if (!glfwInit()) {
        spdlog::error("GLFW initialization failed");
        throw std::runtime_error("GLFW initialization failed");
    }
    spdlog::info("GLFW initialized successfully");

    // Create a simple GLFW window
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_DEPTH_BITS, 24);

    m_impl->window = glfwCreateWindow(800, 600, "Rigel", nullptr, nullptr);
    if (!m_impl->window) {
        spdlog::error("Failed to create GLFW window");
        glfwTerminate();
        throw std::runtime_error("Failed to create GLFW window");
    }

    glfwMakeContextCurrent(m_impl->window);

    // Initialize GLEW
    if (glewInit() != GLEW_OK) {
        spdlog::error("GLEW initialization failed");
        glfwDestroyWindow(m_impl->window);
        glfwTerminate();
        throw std::runtime_error("GLEW initialization failed");
    }
    spdlog::info("GLEW initialized successfully");

    // Print OpenGL version
    spdlog::info("OpenGL Version: {}", (char*)glGetString(GL_VERSION));

    // Set initial viewport
    glViewport(0, 0, 800, 600);

    // Set Callbacks
    glfwSetFramebufferSizeCallback(m_impl->window, [](GLFWwindow* window, int width, int height)-> void {
        glViewport(0, 0, width, height);
    });
    glfwSetKeyCallback(m_impl->window, Rigel::keyCallback);
    glfwSetWindowUserPointer(m_impl->window, m_impl.get());
    m_impl->setCursorCaptured(true);
    glfwSetCursorPosCallback(m_impl->window, [](GLFWwindow* window, double xpos, double ypos) {
        auto* impl = static_cast<Application::Impl*>(glfwGetWindowUserPointer(window));
        if (!impl) {
            return;
        }

        if (!impl->cursorCaptured) {
            return;
        }

        if (impl->firstMouse) {
            impl->lastMouseX = xpos;
            impl->lastMouseY = ypos;
            impl->firstMouse = false;
            return;
        }

        double xoffset = xpos - impl->lastMouseX;
        double yoffset = ypos - impl->lastMouseY;
        impl->lastMouseX = xpos;
        impl->lastMouseY = ypos;

        impl->cameraYaw += static_cast<float>(xoffset) * impl->mouseSensitivity;
        impl->cameraPitch -= static_cast<float>(yoffset) * impl->mouseSensitivity;
        impl->cameraPitch = std::clamp(impl->cameraPitch, -89.0f, 89.0f);
    });
    glfwSetWindowFocusCallback(m_impl->window, [](GLFWwindow* window, int focused) {
        auto* impl = static_cast<Application::Impl*>(glfwGetWindowUserPointer(window));
        if (!impl) {
            return;
        }

        impl->windowFocused = focused != 0;
        impl->pendingTimeReset = true;
        if (focused) {
            impl->setCursorCaptured(true);
        } else {
            impl->firstMouse = true;
        }
    });
    const char* benchEnv = std::getenv("RIGEL_CHUNK_BENCH");
    if (benchEnv && benchEnv[0] != '\0' && benchEnv[0] != '0') {
        m_impl->benchmarkEnabled = true;
        spdlog::info("Chunk benchmark enabled");
    }

    try {
        m_impl->assets.loadManifest("manifest.yaml");
        m_impl->assets.registerLoader("input", std::make_unique<InputBindingsLoader>());
        m_impl->assets.registerLoader("entity_models", std::make_unique<Entity::EntityModelLoader>());
        m_impl->assets.registerLoader("entity_anims", std::make_unique<Entity::EntityAnimationSetLoader>());
        m_impl->worldSet.persistenceFormats().registerFormat(
            Persistence::Backends::CR::descriptor(),
            Persistence::Backends::CR::factory(),
            Persistence::Backends::CR::probe());
        m_impl->worldSet.setPersistenceStorage(std::make_shared<Persistence::FilesystemBackend>());
        m_impl->worldSet.setPersistenceRoot(mainWorldRootPath(m_impl->activeWorldId));
        m_impl->worldSet.setPersistencePreferredFormat("cr");
        m_impl->worldSet.initializeResources(m_impl->assets);

        if (m_impl->assets.exists("input/default")) {
            auto bindingsHandle = m_impl->assets.get<InputBindings>("input/default");
            m_impl->inputBindings = bindingsHandle.shared();
        }
        if (!m_impl->inputBindings) {
            m_impl->inputBindings = std::make_shared<InputBindings>();
        }
        if (!m_impl->inputBindings->hasAction("debug_overlay")) {
            m_impl->inputBindings->bind("debug_overlay", GLFW_KEY_F1);
        }
        if (!m_impl->inputBindings->hasAction("move_forward")) {
            m_impl->inputBindings->bind("move_forward", GLFW_KEY_W);
        }
        if (!m_impl->inputBindings->hasAction("move_backward")) {
            m_impl->inputBindings->bind("move_backward", GLFW_KEY_S);
        }
        if (!m_impl->inputBindings->hasAction("move_left")) {
            m_impl->inputBindings->bind("move_left", GLFW_KEY_A);
        }
        if (!m_impl->inputBindings->hasAction("move_right")) {
            m_impl->inputBindings->bind("move_right", GLFW_KEY_D);
        }
        if (!m_impl->inputBindings->hasAction("move_up")) {
            m_impl->inputBindings->bind("move_up", GLFW_KEY_SPACE);
        }
        if (!m_impl->inputBindings->hasAction("move_down")) {
            m_impl->inputBindings->bind("move_down", GLFW_KEY_LEFT_CONTROL);
        }
        if (!m_impl->inputBindings->hasAction("sprint")) {
            m_impl->inputBindings->bind("sprint", GLFW_KEY_LEFT_SHIFT);
        }
        if (!m_impl->inputBindings->hasAction("exit")) {
            m_impl->inputBindings->bind("exit", GLFW_KEY_ESCAPE);
        }
        if (!m_impl->inputBindings->hasAction("demo_spawn_entity")) {
            m_impl->inputBindings->bind("demo_spawn_entity", GLFW_KEY_F2);
        }
        m_impl->inputDispatcher.setBindings(m_impl->inputBindings);
        m_impl->inputDispatcher.addListener(&m_impl->debugOverlayListener);

        Voxel::ConfigProvider configProvider =
            makeWorldConfigProvider(m_impl->assets, m_impl->activeWorldId);
        Voxel::WorldGenConfig config = configProvider.loadConfig();
        if (config.solidBlock.empty()) {
            config.solidBlock = "base:stone_shale";
        }
        if (config.surfaceBlock.empty()) {
            config.surfaceBlock = "base:grass";
        }

        m_impl->world = &m_impl->worldSet.createWorld(m_impl->activeWorldId);
        m_impl->worldView = &m_impl->worldSet.createView(m_impl->activeWorldId, m_impl->assets);

        auto crSettings = std::make_shared<Persistence::Backends::CR::CRPersistenceSettings>();
        crSettings->enableLz4 = config.persistence.cr.lz4;
        m_impl->world->persistenceProviders().add(Persistence::Backends::CR::kCRSettingsProviderId, crSettings);

        auto generator =
            std::make_shared<Voxel::WorldGenerator>(m_impl->worldSet.resources().registry());
        generator->setConfig(config);
        m_impl->world->setGenerator(generator);
        m_impl->worldView->setGenerator(generator);

        loadWorldFromDisk(*m_impl->world,
                          m_impl->assets,
                          m_impl->worldSet.persistenceService(),
                          m_impl->worldSet.persistenceContext(m_impl->activeWorldId),
                          generator->config().world.version);

        Voxel::ConfigProvider renderConfigProvider =
            makeRenderConfigProvider(m_impl->assets, m_impl->activeWorldId);
        m_impl->worldView->renderConfig() = renderConfigProvider.loadRenderConfig();
        m_impl->worldView->setStreamConfig(config.stream);
        if (m_impl->benchmarkEnabled) {
            m_impl->worldView->setBenchmark(&m_impl->benchmark);
        }

        auto placeId = m_impl->world->blockRegistry().findByIdentifier(config.solidBlock);
        if (!placeId) {
            placeId = m_impl->world->blockRegistry().findByIdentifier("base:stone_shale");
        }
        if (placeId) {
            m_impl->placeBlock = *placeId;
        } else if (m_impl->world->blockRegistry().size() > 1) {
            m_impl->placeBlock = Voxel::BlockID{1};
        }

        int spawnX = static_cast<int>(std::floor(m_impl->cameraPos.x));
        int spawnZ = static_cast<int>(std::floor(m_impl->cameraPos.z));
        int spawnY = findFirstAirY(*generator, config, spawnX, spawnZ);
        m_impl->cameraPos.y = static_cast<float>(spawnY) + 0.5f;

        m_impl->initDebugField();
        m_impl->initFrameGraph();
        m_impl->initEntityDebug();
        m_impl->initTaa();
        m_impl->worldReady = true;
    } catch (const std::exception& e) {
        spdlog::error("Voxel bootstrap failed: {}", e.what());
    }
}

Application::~Application() {
    if (m_impl && m_impl->worldReady && m_impl->world) {
        try {
            saveWorldToDisk(*m_impl->world,
                            m_impl->worldSet.persistenceService(),
                            m_impl->worldSet.persistenceContext(m_impl->activeWorldId));
        } catch (const std::exception& e) {
            spdlog::error("World save failed: {}", e.what());
        }
    }

    if (m_impl && m_impl->window) {
        glfwMakeContextCurrent(m_impl->window);

        if (m_impl->debugField.initialized) {
            if (m_impl->debugField.vao != 0) {
                glDeleteVertexArrays(1, &m_impl->debugField.vao);
                m_impl->debugField.vao = 0;
            }
            for (GLuint& vbo : m_impl->debugField.vbos) {
                if (vbo != 0) {
                    glDeleteBuffers(1, &vbo);
                    vbo = 0;
                }
            }
            m_impl->debugField.initialized = false;
        }
        if (m_impl->frameGraph.initialized) {
            if (m_impl->frameGraph.vao != 0) {
                glDeleteVertexArrays(1, &m_impl->frameGraph.vao);
                m_impl->frameGraph.vao = 0;
            }
            if (m_impl->frameGraph.vbo != 0) {
                glDeleteBuffers(1, &m_impl->frameGraph.vbo);
                m_impl->frameGraph.vbo = 0;
            }
            m_impl->frameGraph.initialized = false;
        }
        if (m_impl->entityDebug.initialized) {
            if (m_impl->entityDebug.vao != 0) {
                glDeleteVertexArrays(1, &m_impl->entityDebug.vao);
                m_impl->entityDebug.vao = 0;
            }
            if (m_impl->entityDebug.vbo != 0) {
                glDeleteBuffers(1, &m_impl->entityDebug.vbo);
                m_impl->entityDebug.vbo = 0;
            }
            m_impl->entityDebug.initialized = false;
        }
        if (m_impl->taa.quadVao != 0) {
            glDeleteVertexArrays(1, &m_impl->taa.quadVao);
            m_impl->taa.quadVao = 0;
        }
        m_impl->releaseTaaTargets();
        m_impl->taa.initialized = false;

        if (m_impl->worldView) {
            m_impl->worldView->clear();
            m_impl->worldView->releaseRenderResources();
        }
        if (m_impl->world) {
            m_impl->world->clear();
        }
        m_impl->worldSet.resources().releaseRenderResources();
        m_impl->worldSet.clear();
        m_impl->worldView = nullptr;
        m_impl->world = nullptr;
        m_impl->assets.clearCache();

        glfwDestroyWindow(m_impl->window);
        m_impl->window = nullptr;
    }
    glfwTerminate();
    spdlog::info("Application terminated successfully");
}

void Application::run() {
    m_impl->lastTime = glfwGetTime();
    if (m_impl->benchmarkEnabled) {
        m_impl->benchmarkStartTime = m_impl->lastTime;
    }

    // Render loop
    while (!glfwWindowShouldClose(m_impl->window)) {
        double now = glfwGetTime();
        float deltaTime = static_cast<float>(now - m_impl->lastTime);
        m_impl->lastTime = now;

        // Frame setup
        glClearColor(0.2f, 0.3f, 0.3f, 1.0f);

        // Flush event queue
        glfwPollEvents();
        if (m_impl->pendingTimeReset) {
            m_impl->lastTime = glfwGetTime();
            deltaTime = 0.0f;
            m_impl->pendingTimeReset = false;
        }
        if (deltaTime > kMaxFrameTime) {
            deltaTime = kMaxFrameTime;
        }
        m_impl->recordFrameTime(deltaTime);
        Rigel::keyupdate();
        m_impl->inputDispatcher.update();

        if (m_impl->worldReady && m_impl->world && m_impl->worldView) {
            if (m_impl->cursorCaptured &&
                glfwGetInputMode(m_impl->window, GLFW_CURSOR) != GLFW_CURSOR_DISABLED) {
                m_impl->setCursorCaptured(true);
            }
            m_impl->updateCamera(deltaTime);

            if (m_impl->inputDispatcher.isActionJustPressed("demo_spawn_entity")) {
                auto entity = std::make_unique<Entity::Entity>("rigel:demo_entity");
                glm::vec3 spawnPos = m_impl->cameraPos + m_impl->cameraForward * 2.0f;
                spawnPos.y += 0.5f;
                entity->setPosition(spawnPos);
                if (m_impl->assets.exists("entity_models/model_drone_interceptor")) {
                    auto model = m_impl->assets.get<Entity::EntityModelAsset>("entity_models/model_drone_interceptor");
                    entity->setModel(std::move(model));
                }
                Entity::EntityId id = m_impl->world->entities().spawn(std::move(entity));
                if (!id.isNull()) {
                    spdlog::info("Spawned demo entity {}:{}:{} at {:.2f}, {:.2f}, {:.2f}",
                                 id.time, id.random, id.counter,
                                 spawnPos.x, spawnPos.y, spawnPos.z);
                } else {
                    spdlog::warn("Failed to spawn demo entity");
                }
            }

            bool leftDown = glfwGetMouseButton(m_impl->window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
            bool rightDown = glfwGetMouseButton(m_impl->window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
            if (m_impl->cursorCaptured) {
                auto rebuildEditedChunk = [&](const glm::ivec3& worldPos) {
                    Voxel::ChunkCoord coord = Voxel::worldToChunk(worldPos.x, worldPos.y, worldPos.z);
                    int lx = 0;
                    int ly = 0;
                    int lz = 0;
                    Voxel::worldToLocal(worldPos.x, worldPos.y, worldPos.z, lx, ly, lz);
                    m_impl->worldView->rebuildChunkMesh(coord);
                    if (lx == 0) {
                        m_impl->worldView->rebuildChunkMesh(coord.offset(-1, 0, 0));
                    } else if (lx == Voxel::Chunk::SIZE - 1) {
                        m_impl->worldView->rebuildChunkMesh(coord.offset(1, 0, 0));
                    }
                    if (ly == 0) {
                        m_impl->worldView->rebuildChunkMesh(coord.offset(0, -1, 0));
                    } else if (ly == Voxel::Chunk::SIZE - 1) {
                        m_impl->worldView->rebuildChunkMesh(coord.offset(0, 1, 0));
                    }
                    if (lz == 0) {
                        m_impl->worldView->rebuildChunkMesh(coord.offset(0, 0, -1));
                    } else if (lz == Voxel::Chunk::SIZE - 1) {
                        m_impl->worldView->rebuildChunkMesh(coord.offset(0, 0, 1));
                    }
                };

                const float interactDistance = 8.0f;
                RaycastHit hit;
                if (leftDown && !m_impl->lastLeftDown) {
                    if (raycastBlock(*m_impl->world, m_impl->cameraPos, m_impl->cameraForward,
                                     interactDistance, hit)) {
                        m_impl->world->setBlock(hit.block.x, hit.block.y, hit.block.z, Voxel::BlockState{});
                        rebuildEditedChunk(hit.block);
                    }
                }
                if (rightDown && !m_impl->lastRightDown) {
                    if (raycastBlock(*m_impl->world, m_impl->cameraPos, m_impl->cameraForward,
                                     interactDistance, hit)) {
                        glm::ivec3 placePos = hit.block + hit.normal;
                        if (hit.normal != glm::ivec3(0) &&
                            m_impl->placeBlock != Voxel::BlockRegistry::airId() &&
                            m_impl->world->getBlock(placePos.x, placePos.y, placePos.z).isAir()) {
                            Voxel::BlockState state;
                            state.id = m_impl->placeBlock;
                            m_impl->world->setBlock(placePos.x, placePos.y, placePos.z, state);
                            rebuildEditedChunk(placePos);
                        }
                    }
                }
            }
            m_impl->lastLeftDown = leftDown;
            m_impl->lastRightDown = rightDown;

            int width = 0;
            int height = 0;
            glfwGetFramebufferSize(m_impl->window, &width, &height);
            float aspect = (height > 0) ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;

            float renderDistance = m_impl->worldView->renderConfig().renderDistance;
            float nearPlane = 0.1f;
            float farPlane = std::max(500.0f, renderDistance + static_cast<float>(Voxel::Chunk::SIZE));
            glm::mat4 projection = glm::perspective(glm::radians(60.0f), aspect, nearPlane, farPlane);
            glm::mat4 projectionNoJitter = projection;
            glm::mat4 view = glm::lookAt(
                m_impl->cameraPos,
                m_impl->cameraTarget,
                glm::vec3(0.0f, 1.0f, 0.0f)
            );

            bool useTaa = m_impl->worldView->renderConfig().taa.enabled;
            if (useTaa) {
                m_impl->ensureTaaTargets(width, height);
                useTaa = m_impl->taa.initialized && m_impl->taa.sceneFbo != 0;
            } else {
                m_impl->taa.historyValid = false;
            }

            glm::vec2 jitter(0.0f);
            if (useTaa) {
                jitter = m_impl->nextJitter(width, height,
                                            m_impl->worldView->renderConfig().taa.jitterScale);
                projection[2][0] += jitter.x;
                projection[2][1] += jitter.y;
            }

            glBindFramebuffer(GL_FRAMEBUFFER, useTaa ? m_impl->taa.sceneFbo : 0);
            glViewport(0, 0, width, height);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

            m_impl->world->tickEntities(deltaTime);
            m_impl->worldView->updateStreaming(m_impl->cameraPos);
            m_impl->worldView->updateMeshes();
            m_impl->worldView->render(view, projection, m_impl->cameraPos, nearPlane, farPlane, deltaTime);

            if (useTaa) {
                m_impl->renderEntityDebugBoxes(view, projection);
            }

            if (useTaa) {
                glm::mat4 viewProjection = projection * view;
                glm::mat4 viewProjectionNoJitter = projectionNoJitter * view;
                glm::mat4 invViewProjection = glm::inverse(viewProjectionNoJitter);
                glm::vec2 jitterUv = jitter * 0.5f;
                m_impl->resolveTaa(invViewProjection,
                                   viewProjectionNoJitter,
                                   jitterUv,
                                   m_impl->worldView->renderConfig().taa.blend);
                glViewport(0, 0, width, height);
            }

            if (!useTaa) {
                m_impl->renderEntityDebugBoxes(view, projectionNoJitter);
            }
            m_impl->renderDebugField(m_impl->cameraForward,
                                     width,
                                     height);
            m_impl->renderFrameGraph();
        } else {
            int width = 0;
            int height = 0;
            glfwGetFramebufferSize(m_impl->window, &width, &height);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glViewport(0, 0, width, height);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        }

        glfwSwapBuffers(m_impl->window);

        // Exit on ESC
        if (m_impl->inputDispatcher.isActionPressed("exit")) {
            glfwSetWindowShouldClose(m_impl->window, true);
        }
    }

    if (m_impl->benchmarkEnabled) {
        double endTime = glfwGetTime();
        double elapsed = endTime - m_impl->benchmarkStartTime;
        const auto& stats = m_impl->benchmark;
        double genRate = (elapsed > 0.0)
            ? static_cast<double>(stats.generatedChunks) / elapsed
            : 0.0;
        double processedRate = (elapsed > 0.0)
            ? static_cast<double>(stats.processedChunks()) / elapsed
            : 0.0;
        double meshedRate = (elapsed > 0.0)
            ? static_cast<double>(stats.meshedChunks) / elapsed
            : 0.0;
        spdlog::info(
            "Chunk benchmark (lifetime): generated {} ({:.1f}/s), processed {} ({:.1f}/s), "
            "meshed {} ({:.1f}/s), empty {}, wall {:.2f}s "
            "[gen {:.2f}s, mesh {:.2f}s, empty {:.2f}s]",
            stats.generatedChunks,
            genRate,
            stats.processedChunks(),
            processedRate,
            stats.meshedChunks,
            meshedRate,
            stats.emptyChunks,
            elapsed,
            stats.generationSeconds,
            stats.meshSeconds,
            stats.emptyMeshSeconds
        );
    }
}

} // namespace Rigel
