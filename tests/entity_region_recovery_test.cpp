#include "TestFramework.h"

#include "Rigel/Asset/AssetManager.h"
#include "Rigel/Entity/Entity.h"
#include "Rigel/Entity/EntityPersistence.h"
#include "Rigel/Persistence/Backends/CR/CRFormat.h"
#include "Rigel/Persistence/Backends/Memory/MemoryFormat.h"
#include "Rigel/Persistence/PersistenceService.h"
#include "Rigel/Persistence/Storage.h"
#include "Rigel/Persistence/WorldPersistence.h"
#include "Rigel/Voxel/World.h"
#include "Rigel/Voxel/WorldResources.h"

#include <algorithm>
#include <cstring>
#include <functional>
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
constexpr const char* kJournalPath =
    "entity-recovery-world/entity-regions.journal";

struct SharedFiles {
    std::unordered_map<std::string, std::vector<uint8_t>> files;
};

enum class FailureTiming {
    BeforeMutation,
    AfterMutation,
};

struct MutationControl {
    std::optional<size_t> failAt;
    FailureTiming failureTiming = FailureTiming::BeforeMutation;
    size_t nextMutation = 0;
    std::vector<std::string> attempted;
    std::optional<size_t> activeMutation;

    void beforeMutation(const std::string& operation) {
        attempted.push_back(operation);
        const size_t index = nextMutation++;
        activeMutation = index;
        if (failAt && index == *failAt &&
            failureTiming == FailureTiming::BeforeMutation) {
            throw std::runtime_error("injected entity persistence interruption");
        }
    }

    void afterMutation() {
        if (failAt && activeMutation == failAt &&
            failureTiming == FailureTiming::AfterMutation) {
            throw std::runtime_error("injected entity persistence interruption");
        }
        activeMutation.reset();
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
        if (m_control && isEntityMutationPath(m_path)) {
            m_control->afterMutation();
        }
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
        if (m_control && isEntityMutationPath(path)) {
            m_control->afterMutation();
        }
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
    const std::shared_ptr<MutationControl>& control = {},
    std::string preferredFormat = "memory") {
    Persistence::PersistenceContext context;
    context.rootPath = kRootPath;
    context.preferredFormat = std::move(preferredFormat);
    context.storage = std::make_shared<SharedStorage>(files, control);
    return context;
}

void registerMemoryFormat(Persistence::FormatRegistry& formats) {
    formats.registerFormat(
        Persistence::Backends::Memory::descriptor(),
        Persistence::Backends::Memory::factory(),
        Persistence::Backends::Memory::probe());
}

void registerFormats(Persistence::FormatRegistry& formats) {
    registerMemoryFormat(formats);
    formats.registerFormat(
        Persistence::Backends::CR::descriptor(),
        Persistence::Backends::CR::factory(),
        Persistence::Backends::CR::probe());
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
                 const std::shared_ptr<MutationControl>& control = {},
                 std::string preferredFormat = "memory") {
    Persistence::FormatRegistry formats;
    registerFormats(formats);
    Persistence::PersistenceService service(formats);
    Voxel::WorldResources resources;
    Voxel::World world(resources);
    world.setId(1);
    populateWorld(world, records);
    auto context = makeContext(
        files, control, std::move(preferredFormat));
    context.providers = world.persistenceProvidersHandle();
    Persistence::saveWorldToDisk(world, service, context);
}

std::string saveFailureAfterIdMutation(
    const std::shared_ptr<SharedFiles>& files,
    const std::vector<EntityRecord>& records,
    const std::string& preferredFormat,
    const std::function<void(Entity::Entity&)>& mutate,
    const std::shared_ptr<MutationControl>& control) {
    Persistence::FormatRegistry formats;
    registerFormats(formats);
    Persistence::PersistenceService service(formats);
    Voxel::WorldResources resources;
    Voxel::World world(resources);
    world.setId(1);
    populateWorld(world, records);
    world.entities().forEach(mutate);
    auto context = makeContext(files, control, preferredFormat);
    context.providers = world.persistenceProvidersHandle();
    try {
        Persistence::saveWorldToDisk(world, service, context);
    } catch (const std::exception& error) {
        return error.what();
    }
    throw Test::TestFailure("Expected entity save validation to fail");
}

std::string saveDirtyChunkFailure(
    const std::shared_ptr<SharedFiles>& files,
    const std::string& preferredFormat,
    const std::shared_ptr<MutationControl>& control) {
    Persistence::FormatRegistry formats;
    registerFormats(formats);
    Persistence::PersistenceService service(formats);
    Voxel::WorldResources resources;
    Voxel::World world(resources);
    world.setId(1);
    world.chunkManager().getOrCreateChunk({0, 0, 0}).markPersistDirty();
    auto context = makeContext(files, control, preferredFormat);
    context.providers = world.persistenceProvidersHandle();
    try {
        Persistence::saveWorldToDisk(world, service, context);
    } catch (const std::exception& error) {
        return error.what();
    }
    throw Test::TestFailure("Expected world save to reject recovery journal");
}

struct LoadedState {
    std::vector<EntityRecord> records;
    std::vector<Persistence::EntityRegionKey> regions;
};

LoadedState loadRecords(const std::shared_ptr<SharedFiles>& files,
                        std::string preferredFormat = "memory") {
    Persistence::FormatRegistry formats;
    registerFormats(formats);
    Persistence::PersistenceService service(formats);
    Voxel::WorldResources resources;
    Voxel::World world(resources);
    world.setId(1);
    Asset::AssetManager assets;
    auto context = makeContext(
        files, {}, std::move(preferredFormat));
    context.providers = world.persistenceProvidersHandle();
    Persistence::loadBootstrapEntities(
        world, assets, service, context);

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
        const Entity::PersistenceRegionCoord region =
            Entity::persistenceRegionForChunk(chunk);
        coords.emplace(region.x, region.y, region.z);
    }

    std::vector<Persistence::EntityRegionKey> regions;
    for (const auto& [x, y, z] : coords) {
        regions.push_back(Persistence::EntityRegionKey{kZoneId, x, y, z});
    }
    return regions;
}

bool journalExists(const std::shared_ptr<SharedFiles>& files) {
    return files->files.contains(kJournalPath);
}

bool hasEntityRegionFile(const std::shared_ptr<SharedFiles>& files) {
    return std::any_of(
        files->files.begin(), files->files.end(),
        [](const auto& entry) {
            return entry.first.find("/entities/entityRegion_") !=
                std::string::npos;
        });
}

void appendU16(std::vector<uint8_t>& bytes, uint16_t value) {
    bytes.push_back(static_cast<uint8_t>(value >> 8));
    bytes.push_back(static_cast<uint8_t>(value));
}

void appendU32(std::vector<uint8_t>& bytes, uint32_t value) {
    bytes.push_back(static_cast<uint8_t>(value >> 24));
    bytes.push_back(static_cast<uint8_t>(value >> 16));
    bytes.push_back(static_cast<uint8_t>(value >> 8));
    bytes.push_back(static_cast<uint8_t>(value));
}

void appendString(std::vector<uint8_t>& bytes, const std::string& value) {
    appendU32(bytes, static_cast<uint32_t>(value.size()));
    bytes.insert(bytes.end(), value.begin(), value.end());
}

std::vector<uint8_t> journalHeader(uint32_t desiredCount,
                                   uint32_t obsoleteCount,
                                   uint32_t formatVersion = 1) {
    std::vector<uint8_t> bytes;
    appendU32(bytes, 0x5247454A);
    appendU16(bytes, 2);
    appendU16(bytes, 0);
    appendString(bytes, "memory");
    appendU32(bytes, formatVersion);
    appendU32(bytes, desiredCount);
    appendU32(bytes, obsoleteCount);
    return bytes;
}

void appendKey(std::vector<uint8_t>& bytes,
               const std::string& zoneId,
               uint32_t x = 0,
               uint32_t y = 0,
               uint32_t z = 0) {
    appendString(bytes, zoneId);
    appendU32(bytes, x);
    appendU32(bytes, y);
    appendU32(bytes, z);
}

void installJournal(const std::shared_ptr<SharedFiles>& files,
                    std::vector<uint8_t> bytes) {
    files->files[kJournalPath] = std::move(bytes);
}

void checkExactState(const std::shared_ptr<SharedFiles>& files,
                     std::vector<EntityRecord> expected,
                     const std::string& preferredFormat = "memory") {
    sortRecords(expected);
    const LoadedState loaded = loadRecords(files, preferredFormat);
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
    const std::vector<std::string> formats = {"memory", "cr"};
    const std::vector<FailureTiming> timings = {
        FailureTiming::BeforeMutation,
        FailureTiming::AfterMutation};
    for (const auto& preferredFormat : formats) {
        for (FailureTiming timing : timings) {
            for (size_t failAt = 0; failAt < mutationCount; ++failAt) {
                auto files = std::make_shared<SharedFiles>();
                saveRecords(files, prior, {}, preferredFormat);

                auto control = std::make_shared<MutationControl>();
                control->failAt = failAt;
                control->failureTiming = timing;
                CHECK_THROWS(saveRecords(
                    files, desired, control, preferredFormat));
                CHECK_EQ(control->attempted.size(), failAt + 1);

                if (failAt == 0 &&
                    timing == FailureTiming::BeforeMutation) {
                    CHECK(!journalExists(files));
                    checkExactState(files, prior, preferredFormat);
                } else {
                    checkExactState(files, desired, preferredFormat);
                }
            }
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
    chunk.coord = Voxel::ChunkCoord{regionX * 16, 0, 0};
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

void bootstrapEntities(
    const std::shared_ptr<SharedFiles>& files,
    Voxel::World& world,
    Asset::AssetManager& assets,
    std::string preferredFormat = "memory",
    const std::shared_ptr<MutationControl>& control = {}) {
    Persistence::FormatRegistry formats;
    registerFormats(formats);
    Persistence::PersistenceService service(formats);
    auto context = makeContext(
        files, control, std::move(preferredFormat));
    context.providers = world.persistenceProvidersHandle();
    Persistence::loadBootstrapEntities(
        world, assets, service, context);
}

std::string loadFailure(
    const std::shared_ptr<SharedFiles>& files,
    Voxel::World& world,
    Asset::AssetManager& assets,
    std::string preferredFormat = "memory",
    const std::shared_ptr<MutationControl>& control = {}) {
    try {
        bootstrapEntities(
            files, world, assets, std::move(preferredFormat), control);
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

TEST_CASE(Persistence_EntityPendingJournalReplaysBeforeSubsequentSave) {
    const EntityRecord prior{
        Entity::EntityId{90, 91, 92},
        "rigel:prior",
        glm::vec3(1.0f, 2.0f, 3.0f)};
    const EntityRecord pending{
        prior.id,
        "rigel:pending",
        glm::vec3(513.0f, 4.0f, 5.0f)};
    const EntityRecord final{
        prior.id,
        "rigel:final",
        glm::vec3(1025.0f, 6.0f, 7.0f)};

    for (const std::string& preferredFormat : {"memory", "cr"}) {
        auto files = std::make_shared<SharedFiles>();
        saveRecords(files, {prior}, {}, preferredFormat);
        auto publishControl = std::make_shared<MutationControl>();
        publishControl->failAt = 1;
        CHECK_THROWS(saveRecords(
            files, {pending}, publishControl, preferredFormat));
        CHECK(journalExists(files));

        auto replayControl = std::make_shared<MutationControl>();
        saveRecords(files, {final}, replayControl, preferredFormat);

        CHECK_EQ(replayControl->attempted.size(), static_cast<size_t>(7));
        CHECK(replayControl->attempted[0].starts_with("write "));
        CHECK(replayControl->attempted[0].find("entity-regions.journal") ==
              std::string::npos);
        CHECK(replayControl->attempted[1].starts_with("remove "));
        CHECK(replayControl->attempted[1].find("entity-regions.journal") ==
              std::string::npos);
        CHECK_EQ(
            replayControl->attempted[2],
            std::string("remove ") + kJournalPath);
        CHECK_EQ(
            replayControl->attempted[3],
            std::string("write ") + kJournalPath);
        CHECK(replayControl->attempted[5].starts_with("remove "));
        CHECK(replayControl->attempted[5].find("entity-regions.journal") ==
              std::string::npos);
        CHECK_EQ(
            replayControl->attempted[6],
            std::string("remove ") + kJournalPath);
        checkExactState(files, {final}, preferredFormat);
    }
}

TEST_CASE(Persistence_PristineWorldSkipsEntityJournalPublication) {
    for (const std::string& preferredFormat : {"memory", "cr"}) {
        auto files = std::make_shared<SharedFiles>();
        auto control = std::make_shared<MutationControl>();

        saveRecords(files, {}, control, preferredFormat);

        CHECK(control->attempted.empty());
        CHECK(!journalExists(files));
        CHECK(!hasEntityRegionFile(files));
    }
}

TEST_CASE(Persistence_EntitySaveRejectsNullMutatedIdBeforePublication) {
    const EntityRecord record{
        Entity::EntityId{100, 101, 102},
        "rigel:null_save",
        glm::vec3(513.0f, 0.0f, 0.0f)};
    for (const std::string& preferredFormat : {"memory", "cr"}) {
        auto files = std::make_shared<SharedFiles>();
        auto control = std::make_shared<MutationControl>();

        const std::string error = saveFailureAfterIdMutation(
            files, {record}, preferredFormat,
            [](Entity::Entity& entity) {
                entity.setId(Entity::EntityId::Null());
            },
            control);

        CHECK(error.find("Null persistent entity ID 0:0:0") !=
              std::string::npos);
        CHECK(error.find("rigel:default/(1, 0, 0)") !=
              std::string::npos);
        CHECK(control->attempted.empty());
        CHECK(!journalExists(files));
        CHECK(!hasEntityRegionFile(files));
    }
}

TEST_CASE(Persistence_EntitySaveRejectsDuplicateMutatedIdsBeforePublication) {
    const Entity::EntityId duplicate{110, 111, 112};
    const EntityRecord first{
        Entity::EntityId{120, 121, 122},
        "rigel:duplicate_save_a",
        glm::vec3(1.0f, 0.0f, 0.0f)};
    const EntityRecord second{
        Entity::EntityId{130, 131, 132},
        "rigel:duplicate_save_b",
        glm::vec3(513.0f, 0.0f, 0.0f)};
    for (const std::string& preferredFormat : {"memory", "cr"}) {
        auto files = std::make_shared<SharedFiles>();
        auto control = std::make_shared<MutationControl>();

        const std::string error = saveFailureAfterIdMutation(
            files, {first, second}, preferredFormat,
            [&](Entity::Entity& entity) {
                entity.setId(duplicate);
            },
            control);

        CHECK(error.find("Duplicate persistent entity ID 110:111:112") !=
              std::string::npos);
        CHECK(error.find("rigel:default/(0, 0, 0)") !=
              std::string::npos);
        CHECK(error.find("rigel:default/(1, 0, 0)") !=
              std::string::npos);
        CHECK(control->attempted.empty());
        CHECK(!journalExists(files));
        CHECK(!hasEntityRegionFile(files));
    }
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
    const EntityRecord live{
        Entity::EntityId{40, 41, 42},
        "rigel:live",
        glm::vec3(32.0f, 2.0f, 3.0f)};
    populateWorld(world, {live});
    const Voxel::ChunkCoord liveChunk{9, 0, 0};
    Voxel::Chunk& chunk = world.chunkManager().getOrCreateChunk(liveChunk);
    chunk.setWorldGenVersion(17);
    chunk.clearDirty();
    Asset::AssetManager assets;
    const std::string error = loadFailure(files, world, assets);

    CHECK_EQ(world.entities().size(), static_cast<size_t>(1));
    CHECK(world.entities().get(live.id) != nullptr);
    CHECK(world.chunkManager().getChunk(liveChunk) != nullptr);
    CHECK_EQ(
        world.chunkManager().getChunk(liveChunk)->worldGenVersion(),
        static_cast<uint32_t>(17));
    CHECK(error.find("0:0:0") != std::string::npos);
    CHECK(error.find("rigel:default/(1, 0, 0)") != std::string::npos);
}

TEST_CASE(Persistence_EntityBootstrapPreservesLiveWorldState) {
    auto files = std::make_shared<SharedFiles>();
    const EntityRecord saved{
        Entity::EntityId{150, 151, 152},
        "rigel:saved",
        glm::vec3(513.0f, 4.0f, 5.0f)};
    saveRawRegions(files, {regionSnapshot(1, {saved})});

    Voxel::WorldResources resources;
    Voxel::World world(resources);
    const EntityRecord live{
        Entity::EntityId{140, 141, 142},
        "rigel:live",
        glm::vec3(1.0f, 2.0f, 3.0f)};
    populateWorld(world, {live});
    const Voxel::ChunkCoord liveChunk{4, 0, 0};
    Voxel::Chunk& chunk = world.chunkManager().getOrCreateChunk(liveChunk);
    chunk.setWorldGenVersion(23);
    chunk.clearDirty();
    Asset::AssetManager assets;

    bootstrapEntities(files, world, assets);

    CHECK_EQ(world.entities().size(), static_cast<size_t>(2));
    CHECK(world.entities().get(live.id) != nullptr);
    CHECK(world.entities().get(saved.id) != nullptr);
    CHECK(world.chunkManager().getChunk(liveChunk) != nullptr);
    CHECK_EQ(
        world.chunkManager().getChunk(liveChunk)->worldGenVersion(),
        static_cast<uint32_t>(23));
}

TEST_CASE(Persistence_EntityBootstrapRejectsLiveIdCollisionBeforeSpawning) {
    auto files = std::make_shared<SharedFiles>();
    const EntityRecord unique{
        Entity::EntityId{160, 161, 162},
        "rigel:unique",
        glm::vec3(1.0f, 2.0f, 3.0f)};
    const EntityRecord colliding{
        Entity::EntityId{170, 171, 172},
        "rigel:colliding",
        glm::vec3(513.0f, 4.0f, 5.0f)};
    saveRawRegions(files, {
        regionSnapshot(0, {unique}),
        regionSnapshot(1, {colliding})});

    Voxel::WorldResources resources;
    Voxel::World world(resources);
    const EntityRecord live{
        colliding.id,
        "rigel:live",
        glm::vec3(32.0f, 2.0f, 3.0f)};
    populateWorld(world, {live});
    const Voxel::ChunkCoord liveChunk{7, 0, 0};
    world.chunkManager().getOrCreateChunk(liveChunk).setWorldGenVersion(29);
    Asset::AssetManager assets;

    const std::string error = loadFailure(files, world, assets);

    CHECK(error.find("170:171:172") != std::string::npos);
    CHECK(error.find("collides with a live entity") != std::string::npos);
    CHECK_EQ(world.entities().size(), static_cast<size_t>(1));
    CHECK(world.entities().get(live.id) != nullptr);
    CHECK(world.entities().get(unique.id) == nullptr);
    CHECK(world.chunkManager().getChunk(liveChunk) != nullptr);
    CHECK_EQ(
        world.chunkManager().getChunk(liveChunk)->worldGenVersion(),
        static_cast<uint32_t>(29));
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

TEST_CASE(Persistence_EntityJournalRejectsUnboundedRegionCounts) {
    const std::vector<std::pair<uint32_t, uint32_t>> declarations = {
        {UINT32_MAX, 0},
        {0, UINT32_MAX}};

    for (const auto& [desiredCount, obsoleteCount] : declarations) {
        auto files = std::make_shared<SharedFiles>();
        installJournal(files, journalHeader(desiredCount, obsoleteCount));
        Voxel::WorldResources resources;
        Voxel::World world(resources);
        Asset::AssetManager assets;

        const std::string error = loadFailure(files, world, assets);

        CHECK(error.find("region count exceeds limit") != std::string::npos);
        CHECK(journalExists(files));
        CHECK_EQ(world.entities().size(), static_cast<size_t>(0));
    }
}

TEST_CASE(Persistence_EntityJournalRejectsCountsBeyondRemainingData) {
    auto files = std::make_shared<SharedFiles>();
    installJournal(files, journalHeader(1, 0));
    Voxel::WorldResources resources;
    Voxel::World world(resources);
    Asset::AssetManager assets;

    const std::string error = loadFailure(files, world, assets);

    CHECK(error.find("counts exceed remaining data") != std::string::npos);
    CHECK(journalExists(files));
    CHECK_EQ(world.entities().size(), static_cast<size_t>(0));
}

TEST_CASE(Persistence_EntityJournalRejectsUnboundedPayloadSize) {
    constexpr uint32_t kMaxEntityRegionBytes = 64 * 1024 * 1024;
    for (uint32_t payloadSize : {kMaxEntityRegionBytes + 1, UINT32_MAX}) {
        auto files = std::make_shared<SharedFiles>();
        std::vector<uint8_t> journal = journalHeader(1, 0);
        appendKey(journal, "");
        appendU32(journal, payloadSize);
        journal.resize(journal.size() + 12);
        installJournal(files, std::move(journal));
        Voxel::WorldResources resources;
        Voxel::World world(resources);
        const EntityRecord live{
            Entity::EntityId{90, 91, 92},
            "rigel:live",
            glm::vec3(1.0f, 2.0f, 3.0f)};
        populateWorld(world, {live});
        Asset::AssetManager assets;

        const std::string error = loadFailure(files, world, assets);

        CHECK(error.find("payload exceeds size limit") != std::string::npos);
        CHECK(journalExists(files));
        CHECK_EQ(world.entities().size(), static_cast<size_t>(1));
        world.entities().forEach([&](const Entity::Entity& entity) {
            CHECK_EQ(entity.id(), live.id);
            CHECK_EQ(entity.typeId(), live.typeId);
            CHECK_EQ(entity.position(), live.position);
        });
    }
}

TEST_CASE(Persistence_EntityJournalRejectsUnboundedPayloadCounts) {
    std::vector<std::vector<uint8_t>> payloads;

    std::vector<uint8_t> chunks = {
        0x52, 0x47, 0x45, 0x31,
        0x00, 0x01,
        0x00, 0x00};
    appendU32(chunks, UINT32_MAX);
    payloads.push_back(std::move(chunks));

    std::vector<uint8_t> entities = {
        0x52, 0x47, 0x45, 0x31,
        0x00, 0x01,
        0x00, 0x00};
    appendU32(entities, 1);
    appendU32(entities, 0);
    appendU32(entities, 0);
    appendU32(entities, 0);
    appendU32(entities, UINT32_MAX);
    payloads.push_back(std::move(entities));

    for (const auto& payload : payloads) {
        auto files = std::make_shared<SharedFiles>();
        std::vector<uint8_t> journal = journalHeader(1, 0);
        appendKey(journal, "");
        appendU32(journal, static_cast<uint32_t>(payload.size()));
        journal.insert(journal.end(), payload.begin(), payload.end());
        installJournal(files, std::move(journal));
        Voxel::WorldResources resources;
        Voxel::World world(resources);
        Asset::AssetManager assets;

        const std::string error = loadFailure(files, world, assets);

        CHECK(error.find("Invalid entity payload for journal region") !=
              std::string::npos);
        CHECK(journalExists(files));
        CHECK_EQ(world.entities().size(), static_cast<size_t>(0));
    }
}

TEST_CASE(Persistence_EntityJournalRejectsUnsupportedVersionBeforeMutation) {
    auto files = std::make_shared<SharedFiles>();
    std::vector<uint8_t> journal = journalHeader(0, 0);
    journal[5] = 1;
    installJournal(files, std::move(journal));
    auto control = std::make_shared<MutationControl>();
    Voxel::WorldResources resources;
    Voxel::World world(resources);
    Asset::AssetManager assets;

    const std::string error = loadFailure(
        files, world, assets, "memory", control);

    CHECK(error.find("Unsupported entity region journal version") !=
          std::string::npos);
    CHECK(control->attempted.empty());
    CHECK(journalExists(files));
}

TEST_CASE(Persistence_EntityJournalRejectsUnexpectedZoneBeforeMutation) {
    auto files = std::make_shared<SharedFiles>();
    std::vector<uint8_t> journal = journalHeader(0, 1);
    appendKey(journal, "../../foreign");
    installJournal(files, std::move(journal));
    auto control = std::make_shared<MutationControl>();
    Voxel::WorldResources resources;
    Voxel::World world(resources);
    Asset::AssetManager assets;

    const std::string error = loadFailure(
        files, world, assets, "memory", control);

    CHECK(error.find("belongs to unexpected zone") != std::string::npos);
    CHECK(control->attempted.empty());
    CHECK(journalExists(files));
}

TEST_CASE(Persistence_EntityJournalRejectsDifferentPersistenceFormat) {
    auto files = std::make_shared<SharedFiles>();
    const EntityRecord desired{
        Entity::EntityId{80, 81, 82},
        "rigel:format_guard",
        glm::vec3(1.0f, 2.0f, 3.0f)};
    auto publishControl = std::make_shared<MutationControl>();
    publishControl->failAt = 1;
    CHECK_THROWS(saveRecords(files, {desired}, publishControl));
    CHECK(journalExists(files));

    auto replayControl = std::make_shared<MutationControl>();
    Voxel::WorldResources resources;
    Voxel::World world(resources);
    Asset::AssetManager assets;
    const std::string error = loadFailure(
        files, world, assets, "cr", replayControl);

    CHECK(error.find("persistence format mismatch") != std::string::npos);
    CHECK(replayControl->attempted.empty());
    CHECK(journalExists(files));
    CHECK(std::none_of(
        files->files.begin(), files->files.end(),
        [](const auto& entry) {
            return entry.first.ends_with(".crbin");
        }));

    auto saveControl = std::make_shared<MutationControl>();
    const std::string saveError = saveDirtyChunkFailure(
        files, "cr", saveControl);
    CHECK(saveError.find("persistence format mismatch") != std::string::npos);
    CHECK(saveControl->attempted.empty());
    CHECK(std::none_of(
        files->files.begin(), files->files.end(),
        [](const auto& entry) {
            return entry.first.ends_with(".cosmicreach");
        }));
    CHECK(journalExists(files));
    checkExactState(files, {desired});
}

TEST_CASE(Persistence_EntityJournalRejectsDifferentPersistenceFormatVersion) {
    auto files = std::make_shared<SharedFiles>();
    installJournal(files, journalHeader(0, 0, 2));
    auto control = std::make_shared<MutationControl>();
    Voxel::WorldResources resources;
    Voxel::World world(resources);
    Asset::AssetManager assets;

    const std::string error = loadFailure(
        files, world, assets, "memory", control);

    CHECK(error.find("persistence format mismatch") != std::string::npos);
    CHECK(control->attempted.empty());
    CHECK(journalExists(files));
}
