#include "TestFramework.h"

#include "Rigel/Asset/AssetManager.h"
#include "Rigel/Entity/Entity.h"
#include "Rigel/Entity/EntityRegion.h"
#include "Rigel/Persistence/Backends/Memory/MemoryFormat.h"
#include "Rigel/Persistence/PersistenceService.h"
#include "Rigel/Persistence/Storage.h"
#include "Rigel/Persistence/WorldPersistence.h"
#include "Rigel/Voxel/World.h"
#include "Rigel/Voxel/WorldResources.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace Rigel;

namespace {

constexpr const char* kRootPath = "entity-recovery-world";
constexpr const char* kZoneId = "rigel:default";

struct SharedFiles {
    std::unordered_map<std::string, std::vector<uint8_t>> files;
};

struct MutationControl {
    std::optional<size_t> failAt;
    size_t nextMutation = 0;
    std::vector<std::string> attempted;

    void beforeMutation(const std::string& operation) {
        attempted.push_back(operation);
        const size_t index = nextMutation++;
        if (failAt && index == *failAt) {
            throw std::runtime_error("injected entity persistence interruption");
        }
    }
};

class SharedByteReader final : public Persistence::ByteReader {
public:
    explicit SharedByteReader(std::vector<uint8_t> data)
        : m_data(std::move(data)) {
    }

    uint8_t readU8() override {
        requireAvailable(1);
        return m_data[m_position++];
    }

    uint16_t readU16() override {
        return static_cast<uint16_t>(readU8()) << 8 |
            static_cast<uint16_t>(readU8());
    }

    uint32_t readU32() override {
        return static_cast<uint32_t>(readU8()) << 24 |
            static_cast<uint32_t>(readU8()) << 16 |
            static_cast<uint32_t>(readU8()) << 8 |
            static_cast<uint32_t>(readU8());
    }

    int32_t readI32() override {
        return static_cast<int32_t>(readU32());
    }

    void readBytes(uint8_t* destination, size_t length) override {
        requireAvailable(length);
        if (length > 0) {
            std::memcpy(destination, m_data.data() + m_position, length);
            m_position += length;
        }
    }

    size_t size() const override {
        return m_data.size();
    }

    size_t tell() const override {
        return m_position;
    }

    void seek(size_t offset) override {
        if (offset > m_data.size()) {
            throw std::runtime_error("Test storage seek out of range");
        }
        m_position = offset;
    }

    std::vector<uint8_t> readAt(size_t offset, size_t length) override {
        if (offset > m_data.size() || length > m_data.size() - offset) {
            throw std::runtime_error("Test storage read out of range");
        }
        return std::vector<uint8_t>(
            m_data.begin() + static_cast<std::ptrdiff_t>(offset),
            m_data.begin() + static_cast<std::ptrdiff_t>(offset + length));
    }

private:
    void requireAvailable(size_t length) const {
        if (m_position > m_data.size() ||
            length > m_data.size() - m_position) {
            throw std::runtime_error("Test storage read past end");
        }
    }

    std::vector<uint8_t> m_data;
    size_t m_position = 0;
};

class SharedByteWriter final : public Persistence::ByteWriter {
public:
    explicit SharedByteWriter(std::vector<uint8_t>& data)
        : m_data(data) {
    }

    void writeU8(uint8_t value) override {
        writeBytes(&value, 1);
    }

    void writeU16(uint16_t value) override {
        const uint8_t bytes[] = {
            static_cast<uint8_t>(value >> 8),
            static_cast<uint8_t>(value)};
        writeBytes(bytes, sizeof(bytes));
    }

    void writeU32(uint32_t value) override {
        const uint8_t bytes[] = {
            static_cast<uint8_t>(value >> 24),
            static_cast<uint8_t>(value >> 16),
            static_cast<uint8_t>(value >> 8),
            static_cast<uint8_t>(value)};
        writeBytes(bytes, sizeof(bytes));
    }

    void writeI32(int32_t value) override {
        writeU32(static_cast<uint32_t>(value));
    }

    void writeBytes(const uint8_t* source, size_t length) override {
        if (m_position + length > m_data.size()) {
            m_data.resize(m_position + length);
        }
        if (length > 0) {
            std::memcpy(m_data.data() + m_position, source, length);
            m_position += length;
        }
    }

    size_t size() const override {
        return m_data.size();
    }

    size_t tell() const override {
        return m_position;
    }

    void seek(size_t offset) override {
        if (offset > m_data.size()) {
            m_data.resize(offset);
        }
        m_position = offset;
    }

    void writeAt(size_t offset,
                 const uint8_t* source,
                 size_t length) override {
        const size_t originalPosition = m_position;
        seek(offset);
        writeBytes(source, length);
        m_position = originalPosition;
    }

    void flush() override {
    }

private:
    std::vector<uint8_t>& m_data;
    size_t m_position = 0;
};

bool isEntityMutationPath(const std::string& path) {
    return path.ends_with("/entity-regions.journal") ||
        path.find("/entities/entityRegion_") != std::string::npos;
}

class SharedWriteSession final : public Persistence::AtomicWriteSession {
public:
    SharedWriteSession(std::shared_ptr<SharedFiles> files,
                       std::shared_ptr<MutationControl> control,
                       std::string path)
        : m_files(std::move(files)),
          m_control(std::move(control)),
          m_path(std::move(path)),
          m_writer(m_buffer) {
    }

    Persistence::ByteWriter& writer() override {
        return m_writer;
    }

    void commit() override {
        if (m_control && isEntityMutationPath(m_path)) {
            m_control->beforeMutation("write " + m_path);
        }
        m_files->files[m_path] = m_buffer;
    }

    void abort() override {
    }

private:
    std::shared_ptr<SharedFiles> m_files;
    std::shared_ptr<MutationControl> m_control;
    std::string m_path;
    std::vector<uint8_t> m_buffer;
    SharedByteWriter m_writer;
};

class SharedStorage final : public Persistence::StorageBackend {
public:
    SharedStorage(std::shared_ptr<SharedFiles> files,
                  std::shared_ptr<MutationControl> control = {})
        : m_files(std::move(files)), m_control(std::move(control)) {
    }

    std::unique_ptr<Persistence::ByteReader> openRead(
        const std::string& path) override {
        const auto found = m_files->files.find(path);
        if (found == m_files->files.end()) {
            throw std::runtime_error("Missing test storage file: " + path);
        }
        return std::make_unique<SharedByteReader>(found->second);
    }

    std::unique_ptr<Persistence::AtomicWriteSession> openWrite(
        const std::string& path,
        Persistence::AtomicWriteOptions) override {
        return std::make_unique<SharedWriteSession>(
            m_files, m_control, path);
    }

    bool exists(const std::string& path) override {
        if (m_files->files.contains(path)) {
            return true;
        }
        return std::any_of(
            m_files->files.begin(), m_files->files.end(),
            [&](const auto& entry) {
                return entry.first.starts_with(path + "/");
            });
    }

    std::vector<std::string> list(const std::string& path) override {
        std::vector<std::string> entries;
        for (const auto& [candidate, _] : m_files->files) {
            if (candidate.starts_with(path + "/")) {
                entries.push_back(candidate);
            }
        }
        return entries;
    }

    void mkdirs(const std::string&) override {
    }

    void remove(const std::string& path) override {
        if (m_control && isEntityMutationPath(path)) {
            m_control->beforeMutation("remove " + path);
        }
        m_files->files.erase(path);
    }

private:
    std::shared_ptr<SharedFiles> m_files;
    std::shared_ptr<MutationControl> m_control;
};

struct EntityRecord {
    Entity::EntityId id;
    std::string typeId;
    glm::vec3 position{0.0f};
    glm::vec3 velocity{0.0f};
    glm::vec3 viewDirection{0.0f, 0.0f, -1.0f};

    bool operator==(const EntityRecord&) const = default;
};

bool entityIdLess(const Entity::EntityId& lhs, const Entity::EntityId& rhs) {
    return std::tie(lhs.time, lhs.random, lhs.counter) <
        std::tie(rhs.time, rhs.random, rhs.counter);
}

void sortRecords(std::vector<EntityRecord>& records) {
    std::sort(
        records.begin(), records.end(),
        [](const EntityRecord& lhs, const EntityRecord& rhs) {
            return entityIdLess(lhs.id, rhs.id);
        });
}

Persistence::PersistenceContext makeContext(
    const std::shared_ptr<SharedFiles>& files,
    const std::shared_ptr<MutationControl>& control = {}) {
    Persistence::PersistenceContext context;
    context.rootPath = kRootPath;
    context.preferredFormat = "memory";
    context.storage = std::make_shared<SharedStorage>(files, control);
    return context;
}

void registerMemoryFormat(Persistence::FormatRegistry& formats) {
    formats.registerFormat(
        Persistence::Backends::Memory::descriptor(),
        Persistence::Backends::Memory::factory(),
        Persistence::Backends::Memory::probe());
}

void populateWorld(Voxel::World& world,
                   const std::vector<EntityRecord>& records) {
    for (const auto& record : records) {
        auto entity = std::make_unique<Entity::Entity>(record.typeId);
        entity->setId(record.id);
        entity->setPosition(record.position);
        entity->setVelocity(record.velocity);
        entity->setViewDirection(record.viewDirection);
        CHECK_EQ(world.entities().spawn(std::move(entity)), record.id);
    }
}

void saveRecords(const std::shared_ptr<SharedFiles>& files,
                 const std::vector<EntityRecord>& records,
                 const std::shared_ptr<MutationControl>& control = {}) {
    Persistence::FormatRegistry formats;
    registerMemoryFormat(formats);
    Persistence::PersistenceService service(formats);
    Voxel::WorldResources resources;
    Voxel::World world(resources);
    world.setId(1);
    populateWorld(world, records);
    auto context = makeContext(files, control);
    context.providers = world.persistenceProvidersHandle();
    Persistence::saveWorldToDisk(world, service, context);
}

struct LoadedState {
    std::vector<EntityRecord> records;
    std::vector<Persistence::EntityRegionKey> regions;
};

LoadedState loadRecords(const std::shared_ptr<SharedFiles>& files) {
    Persistence::FormatRegistry formats;
    registerMemoryFormat(formats);
    Persistence::PersistenceService service(formats);
    Voxel::WorldResources resources;
    Voxel::World world(resources);
    world.setId(1);
    Asset::AssetManager assets;
    auto context = makeContext(files);
    context.providers = world.persistenceProvidersHandle();
    Persistence::loadWorldFromDisk(
        world, assets, service, context, 0,
        Persistence::LoadScope::EntitiesOnly);

    LoadedState loaded;
    world.entities().forEach([&](const Entity::Entity& entity) {
        loaded.records.push_back(EntityRecord{
            entity.id(),
            entity.typeId(),
            entity.position(),
            entity.velocity(),
            entity.viewDirection()});
    });
    sortRecords(loaded.records);

    auto format = service.openFormat(context);
    loaded.regions = format->entityContainer().listRegions(kZoneId);
    std::sort(
        loaded.regions.begin(), loaded.regions.end(),
        [](const auto& lhs, const auto& rhs) {
            return std::tie(lhs.zoneId, lhs.x, lhs.y, lhs.z) <
                std::tie(rhs.zoneId, rhs.x, rhs.y, rhs.z);
        });
    return loaded;
}

std::vector<Persistence::EntityRegionKey> expectedRegions(
    const std::vector<EntityRecord>& records) {
    std::set<std::tuple<int32_t, int32_t, int32_t>> coords;
    for (const auto& record : records) {
        const Voxel::ChunkCoord chunk = Voxel::worldToChunk(
            static_cast<int>(record.position.x),
            static_cast<int>(record.position.y),
            static_cast<int>(record.position.z));
        const Entity::EntityRegionCoord region = Entity::chunkToRegion(chunk);
        coords.emplace(region.x, region.y, region.z);
    }

    std::vector<Persistence::EntityRegionKey> regions;
    for (const auto& [x, y, z] : coords) {
        regions.push_back(Persistence::EntityRegionKey{kZoneId, x, y, z});
    }
    return regions;
}

bool journalExists(const std::shared_ptr<SharedFiles>& files) {
    return files->files.contains(
        std::string(kRootPath) + "/entity-regions.journal");
}

void checkExactState(const std::shared_ptr<SharedFiles>& files,
                     std::vector<EntityRecord> expected) {
    sortRecords(expected);
    const LoadedState loaded = loadRecords(files);
    if (loaded.records.size() != expected.size()) {
        throw Test::TestFailure(
            "Recovered entity count " +
            std::to_string(loaded.records.size()) + " did not match " +
            std::to_string(expected.size()));
    }
    for (size_t i = 0; i < expected.size(); ++i) {
        CHECK_EQ(loaded.records[i].id, expected[i].id);
        CHECK_EQ(loaded.records[i].typeId, expected[i].typeId);
        CHECK_EQ(loaded.records[i].position, expected[i].position);
        CHECK_EQ(loaded.records[i].velocity, expected[i].velocity);
        CHECK_EQ(loaded.records[i].viewDirection, expected[i].viewDirection);
    }
    CHECK_EQ(loaded.regions, expectedRegions(expected));
    CHECK(!journalExists(files));
}

void exerciseInterruptedSave(const std::vector<EntityRecord>& prior,
                             const std::vector<EntityRecord>& desired) {
    constexpr size_t mutationCount = 4;
    for (size_t failAt = 0; failAt < mutationCount; ++failAt) {
        auto files = std::make_shared<SharedFiles>();
        saveRecords(files, prior);

        auto control = std::make_shared<MutationControl>();
        control->failAt = failAt;
        CHECK_THROWS(saveRecords(files, desired, control));
        CHECK_EQ(control->attempted.size(), failAt + 1);

        if (failAt == 0) {
            CHECK(!journalExists(files));
            checkExactState(files, prior);
        } else {
            CHECK(journalExists(files));
            checkExactState(files, desired);
        }
    }
}

Persistence::EntityPersistedEntity persistedEntity(
    const EntityRecord& record) {
    Persistence::EntityPersistedEntity entity;
    entity.id = record.id;
    entity.typeId = record.typeId;
    entity.position = record.position;
    entity.velocity = record.velocity;
    entity.viewDirection = record.viewDirection;
    return entity;
}

Persistence::EntityRegionSnapshot regionSnapshot(
    int32_t regionX,
    const std::vector<EntityRecord>& records) {
    Persistence::EntityRegionSnapshot region;
    region.key = Persistence::EntityRegionKey{kZoneId, regionX, 0, 0};
    Persistence::EntityPersistedChunk chunk;
    chunk.coord = Voxel::ChunkCoord{regionX * Entity::EntityRegionChunkSpan, 0, 0};
    for (const auto& record : records) {
        chunk.entities.push_back(persistedEntity(record));
    }
    region.chunks.push_back(std::move(chunk));
    return region;
}

void saveRawRegions(
    const std::shared_ptr<SharedFiles>& files,
    const std::vector<Persistence::EntityRegionSnapshot>& regions) {
    Persistence::FormatRegistry formats;
    registerMemoryFormat(formats);
    Persistence::PersistenceService service(formats);
    auto context = makeContext(files);
    for (const auto& region : regions) {
        service.saveEntities(region, context);
    }
}

std::string loadFailure(
    const std::shared_ptr<SharedFiles>& files,
    Voxel::World& world,
    Asset::AssetManager& assets) {
    Persistence::FormatRegistry formats;
    registerMemoryFormat(formats);
    Persistence::PersistenceService service(formats);
    auto context = makeContext(files);
    context.providers = world.persistenceProvidersHandle();
    try {
        Persistence::loadWorldFromDisk(
            world, assets, service, context, 0,
            Persistence::LoadScope::EntitiesOnly);
    } catch (const std::exception& error) {
        return error.what();
    }
    throw Test::TestFailure("Expected persisted entity validation to fail");
}

} // namespace

TEST_CASE(Persistence_EntityRegionMoveRecoversAtEveryMutation) {
    const Entity::EntityId id{1, 2, 3};
    const EntityRecord prior{id, "rigel:mover", glm::vec3(1.0f, 2.0f, 3.0f)};
    EntityRecord desired = prior;
    desired.position = glm::vec3(513.0f, 4.0f, 5.0f);

    exerciseInterruptedSave({prior}, {desired});
}

TEST_CASE(Persistence_EntityRegionSwapRecoversAtEveryMutation) {
    const EntityRecord priorA{
        Entity::EntityId{10, 11, 12},
        "rigel:swapper_a",
        glm::vec3(1.0f, 2.0f, 3.0f)};
    const EntityRecord priorB{
        Entity::EntityId{20, 21, 22},
        "rigel:swapper_b",
        glm::vec3(513.0f, 4.0f, 5.0f)};
    EntityRecord desiredA = priorA;
    desiredA.position = glm::vec3(514.0f, 6.0f, 7.0f);
    EntityRecord desiredB = priorB;
    desiredB.position = glm::vec3(2.0f, 8.0f, 9.0f);

    exerciseInterruptedSave({priorA, priorB}, {desiredA, desiredB});
}

TEST_CASE(Persistence_EntityDespawnRecoversAtEveryMutation) {
    const EntityRecord priorA{
        Entity::EntityId{30, 31, 32},
        "rigel:despawn_a",
        glm::vec3(1.0f, 2.0f, 3.0f)};
    const EntityRecord priorB{
        Entity::EntityId{40, 41, 42},
        "rigel:despawn_b",
        glm::vec3(513.0f, 4.0f, 5.0f)};

    exerciseInterruptedSave({priorA, priorB}, {});
}

TEST_CASE(Persistence_EntityLoadRejectsNullIdBeforeSpawning) {
    auto files = std::make_shared<SharedFiles>();
    const EntityRecord valid{
        Entity::EntityId{50, 51, 52},
        "rigel:valid",
        glm::vec3(1.0f)};
    const EntityRecord invalid{
        Entity::EntityId::Null(),
        "rigel:null",
        glm::vec3(513.0f, 0.0f, 0.0f)};
    saveRawRegions(files, {
        regionSnapshot(0, {valid}),
        regionSnapshot(1, {invalid})});

    Voxel::WorldResources resources;
    Voxel::World world(resources);
    Asset::AssetManager assets;
    const std::string error = loadFailure(files, world, assets);

    CHECK_EQ(world.entities().size(), static_cast<size_t>(0));
    CHECK(error.find("0:0:0") != std::string::npos);
    CHECK(error.find("rigel:default/(1, 0, 0)") != std::string::npos);
}

TEST_CASE(Persistence_EntityLoadRejectsDuplicateIdWithinRegion) {
    auto files = std::make_shared<SharedFiles>();
    const Entity::EntityId duplicateId{60, 61, 62};
    const EntityRecord first{
        duplicateId, "rigel:first", glm::vec3(1.0f)};
    const EntityRecord second{
        duplicateId, "rigel:second", glm::vec3(2.0f)};
    saveRawRegions(files, {regionSnapshot(0, {first, second})});

    Voxel::WorldResources resources;
    Voxel::World world(resources);
    Asset::AssetManager assets;
    const std::string error = loadFailure(files, world, assets);

    CHECK_EQ(world.entities().size(), static_cast<size_t>(0));
    CHECK(error.find("60:61:62") != std::string::npos);
    CHECK(error.find("rigel:default/(0, 0, 0)") != std::string::npos);
}

TEST_CASE(Persistence_EntityLoadRejectsDuplicateIdAcrossRegions) {
    auto files = std::make_shared<SharedFiles>();
    const Entity::EntityId duplicateId{70, 71, 72};
    const EntityRecord first{
        duplicateId, "rigel:first", glm::vec3(1.0f)};
    const EntityRecord second{
        duplicateId, "rigel:second", glm::vec3(513.0f, 0.0f, 0.0f)};
    saveRawRegions(files, {
        regionSnapshot(0, {first}),
        regionSnapshot(1, {second})});

    Voxel::WorldResources resources;
    Voxel::World world(resources);
    Asset::AssetManager assets;
    const std::string error = loadFailure(files, world, assets);

    CHECK_EQ(world.entities().size(), static_cast<size_t>(0));
    CHECK(error.find("70:71:72") != std::string::npos);
    CHECK(error.find("rigel:default/(0, 0, 0)") != std::string::npos);
    CHECK(error.find("rigel:default/(1, 0, 0)") != std::string::npos);
}
