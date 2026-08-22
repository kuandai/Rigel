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
#include "../src/entity/EntityPersistenceDetail.h"
#include "../src/entity/EntityPersistenceLimits.h"
#include "../src/persistence/DurableDirectory.h"
#include "../src/persistence/EntityRegionJournal.h"
#include "../src/persistence/EntityRegionJournalLimits.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
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
constexpr float kPositivePositionOverflow = 0x1p31f;
constexpr float kNegativePositionBoundary = -0x1p31f;

struct SharedFiles {
    std::unordered_map<std::string, std::vector<uint8_t>> files;
    std::unordered_map<std::string, std::vector<uint8_t>> durableFiles;
    std::unordered_map<std::string, size_t> reportedReadSizes;
    std::unordered_map<std::string, std::vector<uint8_t>> unsynchronizedRemovals;
    std::set<std::string> directories;
    std::set<std::string> durableDirectories;
    std::vector<std::string> durabilityOperations;
};

enum class FailureTiming {
    BeforeMutation,
    AfterMutation,
};

struct MutationControl {
    std::optional<size_t> failAt;
    FailureTiming failureTiming = FailureTiming::BeforeMutation;
    bool observeAllPaths = false;
    size_t nextMutation = 0;
    std::vector<std::string> attempted;
    std::optional<size_t> activeMutation;
    std::optional<std::string> failDirectoryChild;
    bool directoryFailureInjected = false;

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
    explicit SharedByteReader(std::vector<uint8_t> data,
                              std::optional<size_t> reportedSize = {})
        : m_data(std::move(data)),
          m_reportedSize(reportedSize.value_or(m_data.size())) {
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
        return m_reportedSize;
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
    size_t m_reportedSize = 0;
    size_t m_position = 0;
};

void checkSharedReaderFailure(Persistence::ByteReader& reader,
                              size_t length) {
    const size_t position = reader.tell();
    const std::array<uint8_t, 4> expected{91, 92, 93, 94};
    auto destination = expected;
    try {
        reader.readBytes(destination.data(), length);
    } catch (const std::runtime_error& error) {
        CHECK_EQ(std::string(error.what()), "Test storage read past end");
        CHECK_EQ(reader.tell(), position);
        CHECK_EQ(destination, expected);
        return;
    } catch (const std::exception& error) {
        throw Test::TestFailure(
            std::string("Unexpected sequential read exception: ") + error.what());
    }
    throw Test::TestFailure("Expected sequential read to fail");
}

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

bool observesMutationPath(const std::shared_ptr<MutationControl>& control,
                          const std::string& path) {
    return control &&
        (control->observeAllPaths || isEntityMutationPath(path));
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
        if (observesMutationPath(m_control, m_path)) {
            m_control->beforeMutation("write " + m_path);
        }
        m_files->files[m_path] = m_buffer;
        m_files->reportedReadSizes.erase(m_path);
        if (observesMutationPath(m_control, m_path)) {
            m_control->afterMutation();
        }
        m_files->durableFiles[m_path] = m_buffer;
        m_files->unsynchronizedRemovals.erase(m_path);
        m_files->durabilityOperations.push_back("write " + m_path);
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
        const auto reportedSize = m_files->reportedReadSizes.find(path);
        return std::make_unique<SharedByteReader>(
            found->second,
            reportedSize == m_files->reportedReadSizes.end()
                ? std::optional<size_t>{}
                : std::optional<size_t>{reportedSize->second});
    }

    std::unique_ptr<Persistence::AtomicWriteSession> openWrite(
        const std::string& path) override {
        mkdirs(std::filesystem::path(path).parent_path().generic_string());
        return std::make_unique<SharedWriteSession>(
            m_files, m_control, path);
    }

    bool exists(const std::string& path) override {
        if (m_files->files.contains(path) ||
            m_files->directories.contains(path)) {
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

    void mkdirs(const std::string& path) override {
        std::string currentChild;
        Persistence::detail::createDirectoriesDurably(
            std::filesystem::path(path),
            [&](const std::filesystem::path& directoryPath) {
                currentChild = directoryPath.generic_string();
                const bool created =
                    m_files->directories.insert(currentChild).second;
                m_files->durabilityOperations.push_back(
                    std::string("mkdir ") +
                    (created ? "new " : "existing ") + currentChild);
            },
            [&](const std::filesystem::path& parentPath) {
                m_files->durabilityOperations.push_back(
                    "sync " + parentPath.generic_string() + " for " +
                    currentChild);
                if (m_control && m_control->failDirectoryChild == currentChild &&
                    !m_control->directoryFailureInjected) {
                    m_control->directoryFailureInjected = true;
                    throw std::runtime_error(
                        "injected directory synchronization interruption");
                }
                m_files->durableDirectories.insert(currentChild);
            });
    }

    void remove(const std::string& path) override {
        if (observesMutationPath(m_control, path)) {
            m_control->beforeMutation("remove " + path);
        }
        std::optional<std::vector<uint8_t>> removedContents;
        const auto found = m_files->files.find(path);
        if (found != m_files->files.end()) {
            removedContents = found->second;
            m_files->files.erase(found);
        }
        try {
            if (observesMutationPath(m_control, path)) {
                m_control->afterMutation();
            }
        } catch (...) {
            if (removedContents) {
                m_files->unsynchronizedRemovals.try_emplace(
                    path, std::move(*removedContents));
            }
            throw;
        }
        m_files->durableFiles.erase(path);
        m_files->reportedReadSizes.erase(path);
        m_files->unsynchronizedRemovals.erase(path);
        m_files->durabilityOperations.push_back("remove " + path);
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

std::string saveFailureAfterEntityMutation(
    const std::shared_ptr<SharedFiles>& files,
    const std::vector<EntityRecord>& records,
    const std::string& preferredFormat,
    const std::function<void(Entity::Entity&)>& mutate,
    const std::shared_ptr<MutationControl>& control,
    bool addDirtyChunk = false) {
    Persistence::FormatRegistry formats;
    registerFormats(formats);
    Persistence::PersistenceService service(formats);
    Voxel::WorldResources resources;
    Voxel::World world(resources);
    world.setId(1);
    populateWorld(world, records);
    world.entities().forEach(mutate);
    if (addDirtyChunk) {
        world.chunkManager().getOrCreateChunk({0, 0, 0}).markPersistDirty();
    }
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

LoadedState loadRecords(
    const std::shared_ptr<SharedFiles>& files,
    std::string preferredFormat = "memory",
    const std::shared_ptr<MutationControl>& control = {}) {
    Persistence::FormatRegistry formats;
    registerFormats(formats);
    Persistence::PersistenceService service(formats);
    Voxel::WorldResources resources;
    Voxel::World world(resources);
    world.setId(1);
    Asset::AssetManager assets;
    auto context = makeContext(
        files, control, std::move(preferredFormat));
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
            static_cast<int>(std::floor(record.position.x)),
            static_cast<int>(std::floor(record.position.y)),
            static_cast<int>(std::floor(record.position.z)));
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

size_t operationIndex(const std::vector<std::string>& operations,
                      const std::string& expected) {
    const auto found = std::find(operations.begin(), operations.end(), expected);
    if (found == operations.end()) {
        throw Test::TestFailure("Missing durability operation: " + expected);
    }
    return static_cast<size_t>(std::distance(operations.begin(), found));
}

size_t operationIndexContaining(const std::vector<std::string>& operations,
                                const std::string& prefix,
                                const std::string& contained) {
    const auto found = std::find_if(
        operations.begin(), operations.end(),
        [&](const std::string& operation) {
            return operation.starts_with(prefix) &&
                operation.find(contained) != std::string::npos;
        });
    if (found == operations.end()) {
        throw Test::TestFailure(
            "Missing durability operation containing: " + contained);
    }
    return static_cast<size_t>(std::distance(operations.begin(), found));
}

size_t operationCount(const std::vector<std::string>& operations,
                      const std::string& prefix) {
    return static_cast<size_t>(std::count_if(
        operations.begin(), operations.end(),
        [&](const std::string& operation) {
            return operation.starts_with(prefix);
        }));
}

const std::vector<std::string>& entityDirectoryComponents() {
    static const std::vector<std::string> components = {
        kRootPath,
        std::string(kRootPath) + "/zones",
        std::string(kRootPath) + "/zones/rigel",
        std::string(kRootPath) + "/zones/rigel/default",
        std::string(kRootPath) + "/zones/rigel/default/entities"};
    return components;
}

void simulatePowerLoss(
    const std::shared_ptr<SharedFiles>& files,
    bool retainUnsynchronizedDirectories = false) {
    files->files = files->durableFiles;
    files->unsynchronizedRemovals.clear();

    if (retainUnsynchronizedDirectories) {
        return;
    }

    std::vector<std::string> disappeared;
    for (const auto& directory : files->directories) {
        if (!files->durableDirectories.contains(directory)) {
            disappeared.push_back(directory);
        }
    }
    for (const auto& directory : disappeared) {
        const std::string prefix = directory + "/";
        for (auto it = files->files.begin(); it != files->files.end();) {
            if (it->first.starts_with(prefix)) {
                it = files->files.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = files->durableFiles.begin();
             it != files->durableFiles.end();) {
            if (it->first.starts_with(prefix)) {
                it = files->durableFiles.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = files->directories.begin();
             it != files->directories.end();) {
            if (*it == directory || it->starts_with(prefix)) {
                files->durableDirectories.erase(*it);
                it = files->directories.erase(it);
            } else {
                ++it;
            }
        }
    }
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

std::vector<uint8_t> entityPayloadHeader(uint32_t chunkCount) {
    std::vector<uint8_t> bytes;
    appendU32(bytes, 0x52474531);
    appendU16(bytes, 1);
    appendU16(bytes, 0);
    appendU32(bytes, chunkCount);
    return bytes;
}

void appendEntityChunk(std::vector<uint8_t>& bytes,
                       uint32_t entityCount,
                       uint32_t x = 0,
                       uint32_t y = 0,
                       uint32_t z = 0) {
    appendU32(bytes, x);
    appendU32(bytes, y);
    appendU32(bytes, z);
    appendU32(bytes, entityCount);
}

void appendDesiredPayload(std::vector<uint8_t>& journal,
                          const std::vector<uint8_t>& payload,
                          uint32_t regionX = 0) {
    appendKey(journal, "", regionX);
    appendU32(journal, static_cast<uint32_t>(payload.size()));
    journal.insert(journal.end(), payload.begin(), payload.end());
}

void installJournal(const std::shared_ptr<SharedFiles>& files,
                    std::vector<uint8_t> bytes) {
    files->files[kJournalPath] = bytes;
    files->durableFiles[kJournalPath] = std::move(bytes);
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

Persistence::EntityRegionSnapshot loadRawRegion(
    const std::shared_ptr<SharedFiles>& files,
    const Persistence::EntityRegionKey& key) {
    Persistence::FormatRegistry formats;
    registerMemoryFormat(formats);
    Persistence::PersistenceService service(formats);
    auto context = makeContext(files);
    auto format = service.openFormat(context);
    return format->entityContainer().loadRegion(key);
}

size_t rawRegionCount(const std::shared_ptr<SharedFiles>& files) {
    Persistence::FormatRegistry formats;
    registerMemoryFormat(formats);
    Persistence::PersistenceService service(formats);
    auto context = makeContext(files);
    auto format = service.openFormat(context);
    return format->entityContainer().listRegions(kZoneId).size();
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

void checkJournalRejection(
    std::vector<uint8_t> journal,
    const std::string& diagnostic,
    std::optional<size_t> reportedSize = {}) {
    auto files = std::make_shared<SharedFiles>();
    const EntityRecord persisted{
        Entity::EntityId{201, 202, 203},
        "rigel:persisted",
        glm::vec3(1.0f, 2.0f, 3.0f)};
    saveRawRegions(files, {regionSnapshot(0, {persisted})});
    installJournal(files, std::move(journal));
    if (reportedSize) {
        files->reportedReadSizes[kJournalPath] = *reportedSize;
    }
    const auto filesBefore = files->files;
    const auto durableFilesBefore = files->durableFiles;

    auto control = std::make_shared<MutationControl>();
    Voxel::WorldResources resources;
    Voxel::World world(resources);
    const EntityRecord live{
        Entity::EntityId{211, 212, 213},
        "rigel:live",
        glm::vec3(4.0f, 5.0f, 6.0f)};
    populateWorld(world, {live});
    Asset::AssetManager assets;

    const std::string error = loadFailure(
        files, world, assets, "memory", control);

    CHECK_EQ(
        error,
        std::string("Failed to replay entity recovery journal: ") +
            diagnostic);
    CHECK(control->attempted.empty());
    CHECK_EQ(files->files, filesBefore);
    CHECK_EQ(files->durableFiles, durableFilesBefore);
    CHECK(journalExists(files));
    CHECK_EQ(world.entities().size(), static_cast<size_t>(1));
    world.entities().forEach([&](const Entity::Entity& entity) {
        CHECK_EQ(entity.id(), live.id);
        CHECK_EQ(entity.typeId(), live.typeId);
        CHECK_EQ(entity.position(), live.position);
    });
}

std::string saveRecoverableRegionsFailure(
    const std::shared_ptr<SharedFiles>& files,
    std::vector<Persistence::EntityRegionSnapshot> regions,
    const std::shared_ptr<MutationControl>& control) {
    Persistence::FormatRegistry formats;
    registerMemoryFormat(formats);
    Persistence::PersistenceService service(formats);
    auto context = makeContext(files, control);
    auto format = service.openFormat(context);
    try {
        Persistence::detail::saveEntityRegionsRecoverably(
            *format, context, kZoneId, std::move(regions));
    } catch (const std::exception& error) {
        return error.what();
    }
    throw Test::TestFailure("Expected recoverable entity save to fail");
}

void replayJournal(const std::shared_ptr<SharedFiles>& files) {
    Persistence::FormatRegistry formats;
    registerMemoryFormat(formats);
    Persistence::PersistenceService service(formats);
    auto context = makeContext(files);
    auto format = service.openFormat(context);
    Persistence::detail::replayEntityRegionJournal(
        *format, context, kZoneId);
}

} // namespace

TEST_CASE(Persistence_SharedByteReader_bounds_sequential_reads) {
    const std::vector<uint8_t> fixture{10, 20, 30, 40};
    SharedByteReader reader(fixture);
    std::array<uint8_t, 4> destination{91, 92, 93, 94};

    reader.readBytes(destination.data(), 0);
    CHECK_EQ(reader.tell(), static_cast<size_t>(0));
    CHECK_EQ(destination, (std::array<uint8_t, 4>{91, 92, 93, 94}));

    reader.seek(1);
    checkSharedReaderFailure(reader, fixture.size());
    checkSharedReaderFailure(reader, std::numeric_limits<size_t>::max());

    reader.seek(0);
    reader.readBytes(destination.data(), fixture.size());
    CHECK_EQ(destination, (std::array<uint8_t, 4>{10, 20, 30, 40}));
    CHECK_EQ(reader.tell(), fixture.size());
}

TEST_CASE(Persistence_EntityRegionMoveRecoversAtEveryMutation) {
    const Entity::EntityId id{1, 2, 3};
    const EntityRecord prior{id, "rigel:mover", glm::vec3(1.0f, 2.0f, 3.0f)};
    EntityRecord desired = prior;
    desired.position = glm::vec3(513.0f, 4.0f, 5.0f);

    exerciseInterruptedSave({prior}, {desired});
}

TEST_CASE(Persistence_FirstEntitySaveDurablyOrdersDirectoryHierarchy) {
    const EntityRecord record{
        Entity::EntityId{4, 5, 6},
        "rigel:first_save",
        glm::vec3(513.0f, 4.0f, 5.0f)};

    for (const std::string& preferredFormat : {"memory", "cr"}) {
        auto files = std::make_shared<SharedFiles>();

        saveRecords(files, {record}, {}, preferredFormat);

        const auto& operations = files->durabilityOperations;
        const size_t journalWrite = operationIndex(
            operations, std::string("write ") + kJournalPath);
        const size_t regionWrite = operationIndexContaining(
            operations, "write ", "/entities/entityRegion_");
        const size_t journalRemoval = operationIndex(
            operations, std::string("remove ") + kJournalPath);

        for (const auto& component : entityDirectoryComponents()) {
            CHECK(files->directories.contains(component));
            CHECK(files->durableDirectories.contains(component));
            const size_t creation = operationIndex(
                operations, "mkdir new " + component);
            CHECK_EQ(
                operations.at(creation + 1),
                "sync " +
                    Persistence::detail::containingDirectory(component)
                        .generic_string() +
                    " for " + component);
            if (component == kRootPath) {
                CHECK(creation + 1 < journalWrite);
            } else {
                CHECK(creation + 1 < regionWrite);
            }
        }
        CHECK(regionWrite < journalRemoval);
        CHECK(files->durableFiles.contains(
            operations.at(regionWrite).substr(std::string("write ").size())));
        CHECK(!files->durableFiles.contains(kJournalPath));
        CHECK(!journalExists(files));
        checkExactState(files, {record}, preferredFormat);

        files->durabilityOperations.clear();
        saveRecords(files, {record}, {}, preferredFormat);

        CHECK_EQ(operationCount(
                     files->durabilityOperations, "mkdir new "),
                 static_cast<size_t>(0));
        CHECK_EQ(
            operationCount(files->durabilityOperations, "sync "),
            preferredFormat == "memory" ? static_cast<size_t>(12)
                                          : static_cast<size_t>(7));
        CHECK(!journalExists(files));
        checkExactState(files, {record}, preferredFormat);
    }
}

TEST_CASE(Persistence_EntityJournalRetainsAuthorityAcrossAncestorSyncFailure) {
    const EntityRecord record{
        Entity::EntityId{7, 8, 9},
        "rigel:ancestor_recovery",
        glm::vec3(513.0f, 4.0f, 5.0f)};
    const auto& components = entityDirectoryComponents();

    for (const std::string& preferredFormat : {"memory", "cr"}) {
        for (size_t componentIndex = 1;
             componentIndex < components.size();
             ++componentIndex) {
            for (bool retainFailedComponent : {false, true}) {
                auto files = std::make_shared<SharedFiles>();
                auto control = std::make_shared<MutationControl>();
                control->failDirectoryChild = components[componentIndex];

                CHECK_THROWS(saveRecords(
                    files, {record}, control, preferredFormat));

                CHECK(control->directoryFailureInjected);
                CHECK(journalExists(files));
                CHECK(files->durableFiles.contains(kJournalPath));
                CHECK(!hasEntityRegionFile(files));
                CHECK(files->directories.contains(components[componentIndex]));
                CHECK(!files->durableDirectories.contains(
                    components[componentIndex]));
                CHECK_EQ(
                    operationCount(
                        files->durabilityOperations,
                        std::string("remove ") + kJournalPath),
                    static_cast<size_t>(0));

                simulatePowerLoss(files, retainFailedComponent);
                CHECK(journalExists(files));
                CHECK_EQ(
                    files->directories.contains(components[componentIndex]),
                    retainFailedComponent);

                files->durabilityOperations.clear();
                const LoadedState recovered = loadRecords(
                    files, preferredFormat);

                CHECK_EQ(recovered.records.size(), static_cast<size_t>(1));
                CHECK_EQ(recovered.records.front().id, record.id);
                CHECK(!journalExists(files));
                const auto& replayOperations = files->durabilityOperations;
                const std::string directoryOperation =
                    std::string("mkdir ") +
                    (retainFailedComponent ? "existing " : "new ") +
                    components[componentIndex];
                const size_t creation = operationIndex(
                    replayOperations, directoryOperation);
                CHECK_EQ(
                    replayOperations.at(creation + 1),
                    "sync " +
                        Persistence::detail::containingDirectory(
                            components[componentIndex])
                            .generic_string() +
                        " for " + components[componentIndex]);
                const size_t regionWrite = operationIndexContaining(
                    replayOperations, "write ", "/entities/entityRegion_");
                const size_t journalRemoval = operationIndex(
                    replayOperations,
                    std::string("remove ") + kJournalPath);
                CHECK(creation + 1 < regionWrite);
                CHECK(regionWrite < journalRemoval);
                CHECK(files->durableFiles.contains(
                    replayOperations.at(regionWrite).substr(
                        std::string("write ").size())));
                CHECK(!files->durableFiles.contains(kJournalPath));

                for (const auto& component : components) {
                    CHECK(files->durableDirectories.contains(component));
                }
                simulatePowerLoss(files);
                checkExactState(files, {record}, preferredFormat);
            }
        }
    }
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

TEST_CASE(Persistence_EntityRemovalReplayDurablyRetriesAbsentRegion) {
    const EntityRecord prior{
        Entity::EntityId{50, 51, 52},
        "rigel:removed",
        glm::vec3(1.0f, 2.0f, 3.0f)};

    for (const std::string& preferredFormat : {"memory", "cr"}) {
        auto files = std::make_shared<SharedFiles>();
        saveRecords(files, {prior}, {}, preferredFormat);

        auto removalControl = std::make_shared<MutationControl>();
        removalControl->failAt = 1;
        removalControl->failureTiming = FailureTiming::AfterMutation;
        CHECK_THROWS(saveRecords(
            files, {}, removalControl, preferredFormat));
        CHECK(journalExists(files));
        CHECK(!hasEntityRegionFile(files));
        CHECK_EQ(
            files->unsynchronizedRemovals.size(), static_cast<size_t>(1));

        auto replayControl = std::make_shared<MutationControl>();
        const LoadedState replayed = loadRecords(
            files, preferredFormat, replayControl);

        CHECK(replayed.records.empty());
        CHECK(replayed.regions.empty());
        CHECK(!journalExists(files));
        CHECK(files->unsynchronizedRemovals.empty());
        CHECK_EQ(replayControl->attempted.size(), static_cast<size_t>(2));
        CHECK(replayControl->attempted[0].starts_with("remove "));
        CHECK(replayControl->attempted[0].find("entity-regions.journal") ==
              std::string::npos);
        CHECK_EQ(
            replayControl->attempted[1],
            std::string("remove ") + kJournalPath);

        simulatePowerLoss(files);
        checkExactState(files, {}, preferredFormat);
    }
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

        const std::string error = saveFailureAfterEntityMutation(
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

        const std::string error = saveFailureAfterEntityMutation(
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

TEST_CASE(Persistence_EntitySaveRejectsInvalidPositionsBeforeStorageMutation) {
    const EntityRecord prior{
        Entity::EntityId{180, 181, 182},
        "rigel:position_validation",
        glm::vec3(16.0f, 17.0f, 18.0f)};
    const std::vector<std::pair<glm::vec3, std::string>> cases = {
        {glm::vec3(std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f),
         "position.x"},
        {glm::vec3(0.0f, std::numeric_limits<float>::infinity(), 0.0f),
         "position.y"},
        {glm::vec3(0.0f, 0.0f, -std::numeric_limits<float>::infinity()),
         "position.z"},
        {glm::vec3(kPositivePositionOverflow, 0.0f, 0.0f), "position.x"},
        {glm::vec3(0.0f,
                   std::nextafter(
                       kNegativePositionBoundary,
                       -std::numeric_limits<float>::infinity()),
                   0.0f),
         "position.y"}};

    for (const std::string& preferredFormat : {"memory", "cr"}) {
        for (const auto& [position, field] : cases) {
            auto files = std::make_shared<SharedFiles>();
            saveRecords(files, {prior}, {}, preferredFormat);
            const auto persistedFiles = files->files;
            auto control = std::make_shared<MutationControl>();
            control->observeAllPaths = true;

            const std::string error = saveFailureAfterEntityMutation(
                files, {prior}, preferredFormat,
                [&](Entity::Entity& entity) {
                    entity.setPosition(position);
                },
                control,
                true);

            CHECK_EQ(
                error,
                "Invalid persistent entity " + field +
                    " for entity ID 180:181:182");
            CHECK(control->attempted.empty());
            CHECK_EQ(files->files, persistedFiles);
            checkExactState(files, {prior}, preferredFormat);
        }
    }
}

TEST_CASE(Persistence_EntitySaveAcceptsRepresentablePositionBoundaries) {
    const float positiveBoundary =
        std::nextafter(kPositivePositionOverflow, 0.0f);

    for (const std::string& preferredFormat : {"memory", "cr"}) {
        const std::vector<EntityRecord> records = {
            {Entity::EntityId{190, 191, 192},
             "rigel:positive_boundary",
             glm::vec3(positiveBoundary, 0.0f, 0.0f)},
            {Entity::EntityId{200, 201, 202},
             "rigel:negative_boundary",
             glm::vec3(kNegativePositionBoundary, 0.0f, 0.0f)},
            {Entity::EntityId{210, 211, 212},
             "rigel:negative_fraction",
             glm::vec3(-0.5f, 0.0f, 0.0f)}};
        auto files = std::make_shared<SharedFiles>();

        saveRecords(files, records, {}, preferredFormat);

        checkExactState(files, records, preferredFormat);
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

TEST_CASE(Persistence_EntityLoadRejectsInvalidPositionsBeforeSpawning) {
    const std::vector<float> invalidValues = {
        std::numeric_limits<float>::quiet_NaN(),
        kPositivePositionOverflow,
        std::nextafter(
            kNegativePositionBoundary,
            -std::numeric_limits<float>::infinity())};

    for (float invalidValue : invalidValues) {
        auto files = std::make_shared<SharedFiles>();
        const EntityRecord invalid{
            Entity::EntityId{220, 221, 222},
            "rigel:invalid_position",
            glm::vec3(invalidValue, 0.0f, 0.0f)};
        saveRawRegions(files, {regionSnapshot(0, {invalid})});

        Voxel::WorldResources resources;
        Voxel::World world(resources);
        const EntityRecord live{
            Entity::EntityId{230, 231, 232},
            "rigel:live",
            glm::vec3(32.0f, 2.0f, 3.0f)};
        populateWorld(world, {live});
        Asset::AssetManager assets;

        const std::string error = loadFailure(files, world, assets);

        CHECK(error.find("Invalid persistent entity position") !=
              std::string::npos);
        CHECK_EQ(world.entities().size(), static_cast<size_t>(1));
        const Entity::Entity* loadedLive = world.entities().get(live.id);
        CHECK(loadedLive != nullptr);
        CHECK_EQ(loadedLive->position(), live.position);
    }
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

TEST_CASE(Persistence_EntityJournalRejectsAggregateDeclarationsBeforeMutation) {
    checkJournalRejection(
        journalHeader(Persistence::detail::MaxEntityJournalRegions, 1),
        "Entity region journal aggregate region count exceeds limit");

    checkJournalRejection(
        journalHeader(0, 0),
        "Entity region journal encoded size exceeds limit",
        Persistence::detail::MaxEntityJournalEncodedBytes + 1);

    std::vector<uint8_t> payloadJournal = journalHeader(2, 0);
    appendDesiredPayload(payloadJournal, entityPayloadHeader(0));
    appendKey(payloadJournal, "", 1);
    appendU32(
        payloadJournal,
        static_cast<uint32_t>(
            Persistence::detail::MaxEntityJournalPayloadBytes));
    payloadJournal.resize(payloadJournal.size() + 12);
    checkJournalRejection(
        std::move(payloadJournal),
        "Entity region journal aggregate payload exceeds size limit");

    std::vector<uint8_t> entityJournal = journalHeader(1, 0);
    std::vector<uint8_t> entityPayload = entityPayloadHeader(1);
    appendEntityChunk(
        entityPayload,
        static_cast<uint32_t>(
            Persistence::detail::MaxEntityJournalEntities + 1));
    appendDesiredPayload(entityJournal, entityPayload);
    checkJournalRejection(
        std::move(entityJournal),
        "Entity region journal aggregate entity count exceeds limit");
}

TEST_CASE(Persistence_EntityJournalRejectsAggregateChunkWorkAcrossRegions) {
    constexpr size_t chunksPerRegion =
        Entity::detail::MaxChunksPerEntityRegion;
    static_assert(
        Persistence::detail::MaxEntityJournalChunks % chunksPerRegion == 0);
    constexpr size_t fullRegionCount =
        Persistence::detail::MaxEntityJournalChunks / chunksPerRegion;

    std::vector<uint8_t> journal = journalHeader(
        static_cast<uint32_t>(fullRegionCount + 1), 0);
    for (size_t region = 0; region < fullRegionCount; ++region) {
        std::vector<uint8_t> payload = entityPayloadHeader(
            static_cast<uint32_t>(chunksPerRegion));
        for (size_t chunk = 0; chunk < chunksPerRegion; ++chunk) {
            appendEntityChunk(payload, 0);
        }
        appendDesiredPayload(
            journal, payload, static_cast<uint32_t>(region));
    }
    appendDesiredPayload(
        journal,
        entityPayloadHeader(1),
        static_cast<uint32_t>(fullRegionCount));

    checkJournalRejection(
        std::move(journal),
        "Entity region journal aggregate chunk count exceeds limit");
}

TEST_CASE(Persistence_EntityJournalWriterRejectsAggregateWorkBeforeMutation) {
    auto files = std::make_shared<SharedFiles>();
    const EntityRecord prior{
        Entity::EntityId{221, 222, 223},
        "rigel:prior",
        glm::vec3(1.0f)};
    saveRawRegions(files, {regionSnapshot(0, {prior})});
    const auto filesBefore = files->files;
    const auto durableFilesBefore = files->durableFiles;

    std::vector<Persistence::EntityRegionSnapshot> regions;
    regions.reserve(Persistence::detail::MaxEntityJournalRegions + 1);
    for (uint32_t i = 0;
         i <= Persistence::detail::MaxEntityJournalRegions;
         ++i) {
        const EntityRecord record{
            Entity::EntityId{1, i + 1, 1},
            "rigel:aggregate_region",
            glm::vec3(0.0f)};
        regions.push_back(regionSnapshot(static_cast<int32_t>(i), {record}));
    }
    auto control = std::make_shared<MutationControl>();
    const std::string regionError = saveRecoverableRegionsFailure(
        files, std::move(regions), control);
    CHECK_EQ(
        regionError,
        "Entity region journal aggregate region count exceeds limit");
    CHECK(control->attempted.empty());
    CHECK_EQ(files->files, filesBefore);
    CHECK_EQ(files->durableFiles, durableFilesBefore);
    CHECK(!journalExists(files));

    std::vector<EntityRecord> records;
    records.reserve(Persistence::detail::MaxEntityJournalEntities + 1);
    for (size_t i = 0;
         i <= Persistence::detail::MaxEntityJournalEntities;
         ++i) {
        records.push_back(EntityRecord{
            Entity::EntityId{2, static_cast<uint32_t>(i + 1), 1},
            "rigel:aggregate_entity",
            glm::vec3(0.0f)});
    }
    control = std::make_shared<MutationControl>();
    const std::string entityError = saveRecoverableRegionsFailure(
        files, {regionSnapshot(0, records)}, control);
    CHECK_EQ(
        entityError,
        "Entity region journal aggregate entity count exceeds limit");
    CHECK(control->attempted.empty());
    CHECK_EQ(files->files, filesBefore);
    CHECK_EQ(files->durableFiles, durableFilesBefore);
    CHECK(!journalExists(files));
}

TEST_CASE(Persistence_EntityJournalRecoversAtAggregateRegionBoundary) {
    auto files = std::make_shared<SharedFiles>();
    std::vector<Persistence::EntityRegionSnapshot> regions;
    regions.reserve(Persistence::detail::MaxEntityJournalRegions);
    for (uint32_t i = 0;
         i < Persistence::detail::MaxEntityJournalRegions;
         ++i) {
        const EntityRecord record{
            Entity::EntityId{3, i + 1, 1},
            "rigel:region_boundary",
            glm::vec3(0.0f)};
        regions.push_back(regionSnapshot(static_cast<int32_t>(i), {record}));
    }
    auto control = std::make_shared<MutationControl>();
    control->failAt = 1;

    CHECK_EQ(
        saveRecoverableRegionsFailure(files, std::move(regions), control),
        "Failed to apply entity recovery journal to region "
        "rigel:default/(0, 0, 0): injected entity persistence interruption");
    CHECK(journalExists(files));

    replayJournal(files);

    CHECK(!journalExists(files));
    CHECK_EQ(
        rawRegionCount(files),
        static_cast<size_t>(Persistence::detail::MaxEntityJournalRegions));
}

TEST_CASE(Persistence_EntityJournalRecoversAtAggregateChunkBoundary) {
    auto files = std::make_shared<SharedFiles>();
    constexpr size_t chunksPerRegion =
        Entity::detail::MaxChunksPerEntityRegion;
    constexpr size_t regionCount =
        Persistence::detail::MaxEntityJournalChunks / chunksPerRegion;
    std::vector<Persistence::EntityRegionSnapshot> regions;
    regions.reserve(regionCount);
    for (size_t regionIndex = 0; regionIndex < regionCount; ++regionIndex) {
        Persistence::EntityRegionSnapshot region;
        region.key = Persistence::EntityRegionKey{
            kZoneId, static_cast<int32_t>(regionIndex), 0, 0};
        region.chunks.reserve(chunksPerRegion);
        for (size_t chunkIndex = 0;
             chunkIndex < chunksPerRegion;
             ++chunkIndex) {
            Persistence::EntityPersistedChunk chunk;
            chunk.coord = Voxel::ChunkCoord{
                static_cast<int32_t>(regionIndex * 16 + chunkIndex % 16),
                static_cast<int32_t>((chunkIndex / 16) % 16),
                static_cast<int32_t>(chunkIndex / 256)};
            if (chunkIndex == 0) {
                chunk.entities.push_back(persistedEntity(EntityRecord{
                    Entity::EntityId{
                        4, static_cast<uint32_t>(regionIndex + 1), 1},
                    "rigel:chunk_boundary",
                    glm::vec3(0.0f)}));
            }
            region.chunks.push_back(std::move(chunk));
        }
        regions.push_back(std::move(region));
    }
    auto control = std::make_shared<MutationControl>();
    control->failAt = 1;

    saveRecoverableRegionsFailure(files, std::move(regions), control);
    CHECK(journalExists(files));
    replayJournal(files);

    CHECK(!journalExists(files));
    CHECK_EQ(rawRegionCount(files), regionCount);
    size_t recoveredChunks = 0;
    for (size_t i = 0; i < regionCount; ++i) {
        recoveredChunks += loadRawRegion(
            files,
            Persistence::EntityRegionKey{
                kZoneId, static_cast<int32_t>(i), 0, 0}).chunks.size();
    }
    CHECK_EQ(
        recoveredChunks,
        Persistence::detail::MaxEntityJournalChunks);
}

TEST_CASE(Persistence_EntityJournalRecoversAtAggregateEntityBoundary) {
    auto files = std::make_shared<SharedFiles>();
    std::vector<EntityRecord> records;
    records.reserve(Persistence::detail::MaxEntityJournalEntities);
    for (size_t i = 0;
         i < Persistence::detail::MaxEntityJournalEntities;
         ++i) {
        records.push_back(EntityRecord{
            Entity::EntityId{5, static_cast<uint32_t>(i + 1), 1},
            "rigel:entity_boundary",
            glm::vec3(0.0f)});
    }
    auto control = std::make_shared<MutationControl>();
    control->failAt = 1;

    saveRecoverableRegionsFailure(
        files, {regionSnapshot(0, records)}, control);
    CHECK(journalExists(files));
    replayJournal(files);

    CHECK(!journalExists(files));
    const auto region = loadRawRegion(
        files, Persistence::EntityRegionKey{kZoneId, 0, 0, 0});
    CHECK_EQ(region.chunks.size(), static_cast<size_t>(1));
    CHECK_EQ(
        region.chunks[0].entities.size(),
        Persistence::detail::MaxEntityJournalEntities);
}

TEST_CASE(Persistence_EntityJournalRecoversAtEncodedSizeBoundary) {
    auto files = std::make_shared<SharedFiles>();
    Persistence::EntityRegionSnapshot region;
    region.key = Persistence::EntityRegionKey{kZoneId, 0, 0, 0};
    Persistence::EntityPersistedChunk chunk;
    chunk.coord = Voxel::ChunkCoord{0, 0, 0};
    constexpr size_t entityCount = 64;
    chunk.entities.reserve(entityCount);
    for (size_t i = 0; i < entityCount; ++i) {
        auto entity = persistedEntity(EntityRecord{
            Entity::EntityId{6, static_cast<uint32_t>(i + 1), 1},
            "",
            glm::vec3(0.0f)});
        chunk.entities.push_back(std::move(entity));
    }
    region.chunks.push_back(std::move(chunk));

    std::vector<uint8_t> prefix = journalHeader(1, 0);
    appendKey(prefix, kZoneId);
    appendU32(prefix, 0);
    const size_t targetPayloadBytes =
        Persistence::detail::MaxEntityJournalEncodedBytes - prefix.size();
    const auto baseInfo = Entity::detail::measureEntityRegionPayload(
        region.chunks);
    size_t stringBytes = targetPayloadBytes - baseInfo.encodedBytes;
    for (auto& entity : region.chunks[0].entities) {
        for (std::string* value : {&entity.typeId, &entity.modelId}) {
            const size_t bytes = std::min(
                stringBytes,
                static_cast<size_t>(Entity::detail::MaxEntityStringBytes));
            value->assign(bytes, 'x');
            stringBytes -= bytes;
        }
    }
    CHECK_EQ(stringBytes, static_cast<size_t>(0));
    CHECK_EQ(
        Entity::detail::measureEntityRegionPayload(region.chunks).encodedBytes,
        targetPayloadBytes);

    auto control = std::make_shared<MutationControl>();
    control->failAt = 1;
    saveRecoverableRegionsFailure(files, {std::move(region)}, control);
    CHECK(journalExists(files));
    CHECK_EQ(
        files->durableFiles.at(kJournalPath).size(),
        Persistence::detail::MaxEntityJournalEncodedBytes);

    replayJournal(files);

    CHECK(!journalExists(files));
    auto recovered = loadRawRegion(
        files, Persistence::EntityRegionKey{kZoneId, 0, 0, 0});
    CHECK_EQ(recovered.chunks.size(), static_cast<size_t>(1));
    CHECK_EQ(recovered.chunks[0].entities.size(), entityCount);

    bool extended = false;
    for (auto& entity : recovered.chunks[0].entities) {
        for (std::string* value : {&entity.typeId, &entity.modelId}) {
            if (value->size() < Entity::detail::MaxEntityStringBytes) {
                value->push_back('x');
                extended = true;
                break;
            }
        }
        if (extended) {
            break;
        }
    }
    CHECK(extended);
    const auto filesBefore = files->files;
    const auto durableFilesBefore = files->durableFiles;
    control = std::make_shared<MutationControl>();
    std::vector<Persistence::EntityRegionSnapshot> oversized;
    oversized.push_back(std::move(recovered));

    CHECK_EQ(
        saveRecoverableRegionsFailure(
            files, std::move(oversized), control),
        "Entity region journal encoded size exceeds limit");
    CHECK(control->attempted.empty());
    CHECK_EQ(files->files, filesBefore);
    CHECK_EQ(files->durableFiles, durableFilesBefore);
    CHECK(!journalExists(files));
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
