#include "TestFramework.h"

#include "Rigel/Persistence/Backends/CR/CRFormat.h"
#include "Rigel/Persistence/Backends/Memory/MemoryFormat.h"
#include "Rigel/Persistence/PersistenceService.h"
#include "Rigel/Persistence/Storage.h"
#include "Rigel/Persistence/WorldSettings.h"
#include "Rigel/Voxel/BlockRegistry.h"
#include "Rigel/Voxel/GeneratorSnapshot.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace Rigel;

namespace Rigel::Persistence {

// Most tests in this file focus on staging and recovery. Keep their call sites
// compact while exercising the production requirement that every publication
// also contains an authoritative backend marker.
static std::string publishNewWorldGeneration(
    const WorldSettings& settings,
    const Voxel::WorldGenConfig& definition,
    const PersistenceContext& context) {
    FormatRegistry formats;
    formats.registerFormat(
        Backends::Memory::descriptor(),
        Backends::Memory::factory(),
        Backends::Memory::probe());
    PersistenceService persistence(formats);
    PersistenceContext creationContext = context;
    creationContext.preferredFormat = "memory";
    return publishNewWorldGeneration(
        settings,
        definition,
        persistence,
        creationContext);
}

} // namespace Rigel::Persistence

namespace {

constexpr std::string_view kStagingOwnershipFilename =
    ".rigel-world-generation-stage";
constexpr std::string_view kCleanupOwnershipSuffix = ".rigel-cleanup";
constexpr size_t kStagingSlotCount = 64;

Voxel::WorldGenConfig savedDefinition() {
    Voxel::WorldGenConfig definition;
    definition.seed = 424242u;
    definition.world.version = Voxel::kGeneratorSemanticsVersion;
    definition.solidBlock = "base:stone_shale";
    definition.surfaceBlock = "base:grass";

    Voxel::WorldGenConfig::BiomeConfig biome;
    biome.name = "land";
    biome.surface.push_back({"base:grass", 1});
    definition.biomes.entries.push_back(std::move(biome));

    Voxel::WorldGenConfig::DensityNodeConfig node;
    node.id = "ground";
    node.type = "constant";
    node.value = 0.75f;
    definition.densityGraph.nodes.push_back(std::move(node));
    definition.densityGraph.outputs["base_density"] = "ground";
    definition.stageEnabled["caves"] = false;
    definition.stageEnabled["structures"] = false;
    return definition;
}

Persistence::WorldSettings savedSettings() {
    Persistence::WorldSettings settings;
    settings.displayName = "Close and Reload";
    settings.seed = 424242u;
    settings.generator.sourceId = "rigel:default";
    settings.generator.sourceRevision = 19u;
    settings.generator.definitionSchemaVersion =
        Voxel::kGeneratorDefinitionSchemaVersion;
    settings.generator.semanticsVersion = Voxel::kGeneratorSemanticsVersion;
    return settings;
}

void registerSavedDefinitionBlocks(Voxel::BlockRegistry& registry) {
    for (const std::string identifier : {
             "base:stone_shale",
             "base:grass",
             "base:water[type=source]",
             "base:sand"}) {
        Voxel::BlockType block;
        block.identifier = identifier;
        registry.registerBlock(identifier, std::move(block));
    }
}

Persistence::PersistenceContext contextFor(
    const std::filesystem::path& root,
    std::shared_ptr<Persistence::StorageBackend> storage) {
    Persistence::PersistenceContext context;
    context.rootPath = root.string();
    context.storage = std::move(storage);
    return context;
}

std::string readText(Persistence::StorageBackend& storage,
                     const std::filesystem::path& path) {
    auto reader = storage.openRead(path.string());
    const auto bytes = reader->readAt(0, reader->size());
    return std::string(bytes.begin(), bytes.end());
}

void writeText(Persistence::StorageBackend& storage,
               const std::filesystem::path& path,
               const std::string& text) {
    auto session = storage.openWrite(path.string());
    session->writer().writeBytes(
        reinterpret_cast<const uint8_t*>(text.data()), text.size());
    session->commit();
}

void corruptMemoryWorldMetadataWorldId(
    Persistence::StorageBackend& storage,
    const std::filesystem::path& path) {
    std::string metadata = readText(storage, path);
    if (metadata.size() <= sizeof(uint32_t)) {
        throw std::runtime_error(
            "Cannot corrupt truncated memory world metadata");
    }
    char& firstWorldIdByte = metadata[sizeof(uint32_t)];
    firstWorldIdByte = firstWorldIdByte == 'x' ? 'y' : 'x';
    writeText(storage, path, metadata);
}

void corruptMemoryWorldMetadataDisplayName(
    Persistence::StorageBackend& storage,
    const std::filesystem::path& path) {
    std::string metadata = readText(storage, path);
    if (metadata.size() < 2 * sizeof(uint32_t)) {
        throw std::runtime_error(
            "Cannot corrupt truncated memory world metadata");
    }
    uint32_t worldIdSize = 0;
    for (size_t i = 0; i < sizeof(uint32_t); ++i) {
        worldIdSize = (worldIdSize << 8) |
            static_cast<unsigned char>(metadata[i]);
    }
    if (worldIdSize >
        metadata.size() - 2 * sizeof(uint32_t)) {
        throw std::runtime_error(
            "Cannot corrupt truncated memory world metadata");
    }
    const size_t displayNameOffset =
        2 * sizeof(uint32_t) + worldIdSize;
    if (displayNameOffset >= metadata.size()) {
        throw std::runtime_error(
            "Cannot corrupt empty memory world display name");
    }
    char& firstDisplayNameByte = metadata[displayNameOffset];
    firstDisplayNameByte =
        firstDisplayNameByte == 'x' ? 'y' : 'x';
    writeText(storage, path, metadata);
}

Persistence::WorldMetadata loadMemoryWorldMetadata(
    const std::filesystem::path& root,
    const std::shared_ptr<Persistence::StorageBackend>& storage) {
    Persistence::FormatRegistry formats;
    formats.registerFormat(
        Persistence::Backends::Memory::descriptor(),
        Persistence::Backends::Memory::factory(),
        Persistence::Backends::Memory::probe());
    Persistence::PersistenceService persistence(formats);
    auto context = contextFor(root, storage);
    context.discoverExistingFormat = true;
    return persistence.loadWorldMetadata(context);
}

std::vector<std::filesystem::path> stagingRoots(
    const std::filesystem::path& worldRoot) {
    std::vector<std::filesystem::path> roots;
    const std::string prefix = worldRoot.filename().string() + ".staging.";
    for (const auto& entry :
         std::filesystem::directory_iterator(worldRoot.parent_path())) {
        if (entry.path().filename().string().starts_with(prefix)) {
            roots.push_back(entry.path());
        }
    }
    return roots;
}

std::vector<std::filesystem::path> stagingDirectories(
    const std::filesystem::path& worldRoot) {
    std::vector<std::filesystem::path> directories;
    for (const std::filesystem::path& root : stagingRoots(worldRoot)) {
        if (std::filesystem::is_directory(
                std::filesystem::symlink_status(root))) {
            directories.push_back(root);
        }
    }
    return directories;
}

std::filesystem::path cleanupOwnershipPathForTest(
    const std::filesystem::path& stagedRoot) {
    return std::filesystem::path(
        stagedRoot.string() + std::string(kCleanupOwnershipSuffix));
}

std::string stagingOwnershipMarkerForTest(
    const std::filesystem::path& worldRoot,
    const std::filesystem::path& stagedRoot) {
    std::string marker = "rigel-world-generation-staging\nversion: 1\n";
    for (const auto& [field, path] :
         std::vector<std::pair<std::string, std::filesystem::path>>{
             {"world-root", worldRoot}, {"staging-root", stagedRoot}}) {
        const std::string identity =
            path.lexically_normal().generic_string();
        marker += field + "-bytes: " + std::to_string(identity.size()) +
            "\n" + identity + "\n";
    }
    return marker;
}

std::string cleanupOwnershipMarkerForTest(
    const std::filesystem::path& worldRoot,
    const std::filesystem::path& stagedRoot) {
    std::string marker =
        "rigel-world-generation-staging-cleanup\nversion: 1\n";
    for (const auto& [field, path] :
         std::vector<std::pair<std::string, std::filesystem::path>>{
             {"world-root", worldRoot}, {"staging-root", stagedRoot}}) {
        const std::string identity =
            path.lexically_normal().generic_string();
        marker += field + "-bytes: " + std::to_string(identity.size()) +
            "\n" + identity + "\n";
    }
    return marker;
}

std::string handoffOwnershipMarkerForTest(
    const std::filesystem::path& worldRoot,
    const std::filesystem::path& stagedRoot) {
    std::string marker =
        "rigel-world-generation-publication-handoff\nversion: 1\n";
    for (const auto& [field, path] :
         std::vector<std::pair<std::string, std::filesystem::path>>{
             {"world-root", worldRoot}, {"staging-root", stagedRoot}}) {
        const std::string identity =
            path.lexically_normal().generic_string();
        marker += field + "-bytes: " + std::to_string(identity.size()) +
            "\n" + identity + "\n";
    }
    return marker;
}

class CurrentPathGuard {
public:
    explicit CurrentPathGuard(const std::filesystem::path& path)
        : m_original(std::filesystem::current_path()) {
        std::filesystem::current_path(path);
    }

    ~CurrentPathGuard() {
        std::error_code error;
        std::filesystem::current_path(m_original, error);
    }

private:
    std::filesystem::path m_original;
};

class ObservedWriteSession final : public Persistence::AtomicWriteSession {
public:
    ObservedWriteSession(
        std::unique_ptr<Persistence::AtomicWriteSession> inner,
        std::string path,
        size_t ordinal,
        size_t failOrdinal,
        bool failAfterCommit,
        std::vector<std::string>& commits)
        : m_inner(std::move(inner))
        , m_path(std::move(path))
        , m_ordinal(ordinal)
        , m_failOrdinal(failOrdinal)
        , m_failAfterCommit(failAfterCommit)
        , m_commits(commits) {
    }

    Persistence::ByteWriter& writer() override {
        return m_inner->writer();
    }

    void commit() override {
        if (m_ordinal == m_failOrdinal && !m_failAfterCommit) {
            throw std::runtime_error("injected publication failure");
        }
        m_inner->commit();
        m_commits.push_back(m_path);
        if (m_ordinal == m_failOrdinal && m_failAfterCommit) {
            throw std::runtime_error("injected post-commit failure");
        }
    }

    void abort() override {
        m_inner->abort();
    }

private:
    std::unique_ptr<Persistence::AtomicWriteSession> m_inner;
    std::string m_path;
    size_t m_ordinal;
    size_t m_failOrdinal;
    bool m_failAfterCommit;
    std::vector<std::string>& m_commits;
};

class ObservingStorage : public Persistence::FilesystemBackend {
public:
    size_t failOrdinal = std::numeric_limits<size_t>::max();
    bool failAfterCommit = false;
    bool failRemovals = false;
    std::vector<std::string> commits;

    std::unique_ptr<Persistence::AtomicWriteSession> openWrite(
        const std::string& path) override {
        auto inner = Persistence::FilesystemBackend::openWrite(path);
        return std::make_unique<ObservedWriteSession>(
            std::move(inner),
            path,
            ++m_openOrdinal,
            failOrdinal,
            failAfterCommit,
            commits);
    }

    void remove(const std::string& path) override {
        if (failRemovals) {
            throw std::runtime_error("injected cleanup failure");
        }
        Persistence::FilesystemBackend::remove(path);
    }

private:
    size_t m_openOrdinal = 0;
};

class AfterCommitWriteSession final
    : public Persistence::AtomicWriteSession {
public:
    AfterCommitWriteSession(
        std::unique_ptr<Persistence::AtomicWriteSession> inner,
        std::function<void()> afterCommit)
        : m_inner(std::move(inner))
        , m_afterCommit(std::move(afterCommit)) {
    }

    Persistence::ByteWriter& writer() override {
        return m_inner->writer();
    }

    void commit() override {
        m_inner->commit();
        m_afterCommit();
    }

    void abort() override {
        m_inner->abort();
    }

private:
    std::unique_ptr<Persistence::AtomicWriteSession> m_inner;
    std::function<void()> m_afterCommit;
};

enum class StagedMetadataCorruption {
    WorldId,
    DisplayName
};

class CorruptingStagedMetadataStorage final
    : public Persistence::FilesystemBackend {
public:
    explicit CorruptingStagedMetadataStorage(
        StagedMetadataCorruption corruption)
        : m_corruption(corruption) {
    }

    std::unique_ptr<Persistence::AtomicWriteSession> openWrite(
        const std::string& path) override {
        auto inner = Persistence::FilesystemBackend::openWrite(path);
        if (metadataCorrupted ||
            std::filesystem::path(path).filename() != "world.meta") {
            return inner;
        }
        return std::make_unique<AfterCommitWriteSession>(
            std::move(inner),
            [this, path] {
                metadataCorrupted = true;
                stagedRoot = std::filesystem::path(path).parent_path();
                if (m_corruption ==
                    StagedMetadataCorruption::WorldId) {
                    corruptMemoryWorldMetadataWorldId(*this, path);
                } else {
                    corruptMemoryWorldMetadataDisplayName(*this, path);
                }
            });
    }

    void publishDirectory(const std::string& stagedPath,
                          const std::string& finalPath) override {
        ++publishAttempts;
        Persistence::FilesystemBackend::publishDirectory(
            stagedPath, finalPath);
    }

    bool metadataCorrupted = false;
    size_t publishAttempts = 0;
    std::filesystem::path stagedRoot;

private:
    StagedMetadataCorruption m_corruption;
};

class PublishCountingStorage final
    : public Persistence::FilesystemBackend {
public:
    void publishDirectory(const std::string& stagedPath,
                          const std::string& finalPath) override {
        ++publishAttempts;
        Persistence::FilesystemBackend::publishDirectory(
            stagedPath, finalPath);
    }

    size_t publishAttempts = 0;
};

class PausedPublicationStorage final
    : public Persistence::FilesystemBackend {
public:
    void publishDirectory(const std::string& stagedPath,
                          const std::string& finalPath) override {
        {
            std::unique_lock lock(m_mutex);
            m_publishReady = true;
            m_changed.notify_all();
            m_changed.wait(lock, [&] { return m_releasePublish; });
        }
        Persistence::FilesystemBackend::publishDirectory(stagedPath, finalPath);
    }

    void waitUntilPublishReady() {
        std::unique_lock lock(m_mutex);
        m_changed.wait(lock, [&] { return m_publishReady; });
    }

    void releasePublish() {
        std::lock_guard lock(m_mutex);
        m_releasePublish = true;
        m_changed.notify_all();
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_changed;
    bool m_publishReady = false;
    bool m_releasePublish = false;
};

class ObservedLockAcquisitionStorage final
    : public Persistence::FilesystemBackend {
public:
    std::unique_ptr<Persistence::WorldGenerationBootstrapLock>
    lockWorldGenerationBootstrap(const std::string& worldRoot) override {
        {
            std::lock_guard lock(m_mutex);
            m_attempting = true;
            m_changed.notify_all();
        }
        auto result = Persistence::FilesystemBackend::
            lockWorldGenerationBootstrap(worldRoot);
        {
            std::lock_guard lock(m_mutex);
            m_acquired = true;
            m_changed.notify_all();
        }
        return result;
    }

    void waitUntilAttempting() {
        std::unique_lock lock(m_mutex);
        m_changed.wait(lock, [&] { return m_attempting; });
    }

    bool acquired() {
        std::lock_guard lock(m_mutex);
        return m_acquired;
    }

    bool waitBrieflyForAcquisition() {
        std::unique_lock lock(m_mutex);
        return m_changed.wait_for(
            lock,
            std::chrono::milliseconds(100),
            [&] { return m_acquired; });
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_changed;
    bool m_attempting = false;
    bool m_acquired = false;
};

class PayloadCleanupFailureStorage final
    : public ObservingStorage {
public:
    void remove(const std::string& path) override {
        const std::filesystem::path removalPath(path);
        if (!m_failed && removalPath.filename() ==
                "generator-definition.yaml") {
            m_failed = true;
            failedStage = removalPath.parent_path();
            throw std::runtime_error(
                "injected payload cleanup failure");
        }
        Persistence::FilesystemBackend::remove(path);
    }

    std::filesystem::path failedStage;

private:
    bool m_failed = false;
};

class EmptyStageRemovalFailureStorage final
    : public ObservingStorage {
public:
    bool createDirectoryExclusive(const std::string& path) override {
        const bool created = Persistence::FilesystemBackend::
            createDirectoryExclusive(path);
        if (created) {
            stagedRoot = path;
        }
        return created;
    }

    void remove(const std::string& path) override {
        if (!m_failed && std::filesystem::path(path) == stagedRoot) {
            m_failed = true;
            throw std::runtime_error(
                "injected empty staging directory cleanup interruption");
        }
        Persistence::FilesystemBackend::remove(path);
    }

    std::filesystem::path stagedRoot;

private:
    bool m_failed = false;
};

class CollidingReservationStorage final
    : public Persistence::FilesystemBackend {
public:
    enum class CollisionKind {
        Directory,
        RegularFile,
        DirectorySymlink
    };

    CollidingReservationStorage(
        CollisionKind collisionKind,
        std::filesystem::path symlinkTarget = {})
        : m_collisionKind(collisionKind)
        , m_symlinkTarget(std::move(symlinkTarget)) {
    }

    bool createDirectoryExclusive(const std::string& path) override {
        ++reservationAttempts;
        if (collisionPath.empty()) {
            collisionPath = path;
            std::filesystem::create_directories(
                collisionPath.parent_path());
            if (m_collisionKind == CollisionKind::Directory) {
                std::filesystem::create_directory(collisionPath);
                writeText(
                    *this,
                    collisionPath / "must-survive.txt",
                    "pre-existing directory");
            } else if (m_collisionKind == CollisionKind::RegularFile) {
                writeText(
                    *this,
                    collisionPath,
                    "pre-existing regular file");
            } else {
                std::filesystem::create_directory(m_symlinkTarget);
                writeText(
                    *this,
                    m_symlinkTarget / "must-survive.txt",
                    "pre-existing symlink target");
                std::filesystem::create_directory_symlink(
                    m_symlinkTarget, collisionPath);
            }
        }
        return Persistence::FilesystemBackend::createDirectoryExclusive(path);
    }

    size_t reservationAttempts = 0;
    std::filesystem::path collisionPath;

private:
    CollisionKind m_collisionKind;
    std::filesystem::path m_symlinkTarget;
};

class ReusingPublishedStagingPathStorage final
    : public Persistence::FilesystemBackend {
public:
    void publishDirectory(const std::string& stagedPath,
                          const std::string& finalPath) override {
        Persistence::FilesystemBackend::publishDirectory(
            stagedPath, finalPath);
        reusedPath = stagedPath;
        std::filesystem::create_directory(reusedPath);
        writeText(
            *this,
            reusedPath / "must-survive.txt",
            "unowned post-publish reuse");
        if (displacePublishedRootBeforeFailure) {
            displacedWorldRoot = finalPath + ".displaced";
            Persistence::FilesystemBackend::publishDirectory(
                finalPath, displacedWorldRoot.string());
        }
        throw std::runtime_error("injected post-publication failure");
    }

    void remove(const std::string& path) override {
        if (std::filesystem::path(path) ==
            cleanupOwnershipPathForTest(reusedPath)) {
            throw std::runtime_error(
                "injected post-publication tombstone cleanup failure");
        }
        Persistence::FilesystemBackend::remove(path);
    }

    bool displacePublishedRootBeforeFailure = false;
    std::filesystem::path reusedPath;
    std::filesystem::path displacedWorldRoot;
};

class DefinitelyFailedPublicationStorage final
    : public Persistence::FilesystemBackend {
public:
    void publishDirectory(const std::string& stagedPath,
                          const std::string&) override {
        stagedRoot = stagedPath;
        throw Persistence::DirectoryPublicationError(
            Persistence::DirectoryPublicationState::NotPublished,
            "injected definite pre-rename publication failure");
    }

    std::filesystem::path stagedRoot;
};

class IndeterminatePublicationStorage final
    : public Persistence::FilesystemBackend {
public:
    void publishDirectory(const std::string& stagedPath,
                          const std::string& finalPath) override {
        stagedRoot = stagedPath;
        Persistence::FilesystemBackend::publishDirectory(
            stagedPath, finalPath);
        throw Persistence::DirectoryPublicationError(
            Persistence::DirectoryPublicationState::Indeterminate,
            "injected interruption after publication rename");
    }

    std::filesystem::path stagedRoot;
};

class ChildRetirementFailureStorage final
    : public Persistence::FilesystemBackend {
public:
    bool createDirectoryExclusive(const std::string& path) override {
        const bool created = Persistence::FilesystemBackend::
            createDirectoryExclusive(path);
        if (created) {
            stagedRoot = path;
        }
        return created;
    }

    void remove(const std::string& path) override {
        const std::filesystem::path removalPath(path);
        if (!m_failed && removalPath.parent_path() == stagedRoot &&
            removalPath.filename() == kStagingOwnershipFilename) {
            m_failed = true;
            throw std::runtime_error(
                "injected child-authority retirement failure");
        }
        Persistence::FilesystemBackend::remove(path);
    }

    std::filesystem::path stagedRoot;

private:
    bool m_failed = false;
};

class MovedBackPublicationStorage final
    : public Persistence::FilesystemBackend {
public:
    void publishDirectory(const std::string& stagedPath,
                          const std::string& finalPath) override {
        stagedRoot = stagedPath;
        Persistence::FilesystemBackend::publishDirectory(
            stagedPath, finalPath);
        std::filesystem::rename(finalPath, stagedPath);
        throw std::runtime_error(
            "injected reversal after publication rename");
    }

    std::filesystem::path stagedRoot;
};

class HandoffRemovalFailureStorage final
    : public Persistence::FilesystemBackend {
public:
    void publishDirectory(const std::string& stagedPath,
                          const std::string& finalPath) override {
        stagedRoot = stagedPath;
        Persistence::FilesystemBackend::publishDirectory(
            stagedPath, finalPath);
        m_published = true;
    }

    void remove(const std::string& path) override {
        if (m_published && !m_failed &&
            std::filesystem::path(path) ==
                cleanupOwnershipPathForTest(stagedRoot)) {
            m_failed = true;
            throw std::runtime_error(
                "injected external handoff retirement failure");
        }
        Persistence::FilesystemBackend::remove(path);
    }

    std::filesystem::path stagedRoot;

private:
    bool m_published = false;
    bool m_failed = false;
};

class PostCreateDirectorySyncFailureStorage final
    : public Persistence::FilesystemBackend {
public:
    bool createDirectoryExclusive(const std::string& path) override {
        const bool created = Persistence::FilesystemBackend::
            createDirectoryExclusive(path);
        if (created) {
            failedStage = path;
            throw std::runtime_error(
                "injected directory synchronization failure after create");
        }
        return false;
    }

    std::filesystem::path failedStage;
};

class AmbiguousReservationFailureStorage final
    : public Persistence::FilesystemBackend {
public:
    bool createDirectoryExclusive(const std::string& path) override {
        const bool created = Persistence::FilesystemBackend::
            createDirectoryExclusive(path);
        if (created) {
            failedStages.emplace_back(path);
            throw std::runtime_error(
                "injected ambiguous directory reservation failure");
        }
        return false;
    }

    void remove(const std::string& path) override {
        if (std::find(
                failedStages.begin(),
                failedStages.end(),
                std::filesystem::path(path)) != failedStages.end()) {
            throw std::runtime_error(
                "injected ambiguous reservation rollback failure");
        }
        Persistence::FilesystemBackend::remove(path);
    }

    std::vector<std::filesystem::path> failedStages;
};

class CollidingReservationRemovalFailureStorage final
    : public Persistence::FilesystemBackend {
public:
    bool createDirectoryExclusive(const std::string& path) override {
        ++reservationAttempts;
        if (collisionPath.empty()) {
            collisionPath = path;
            std::filesystem::create_directories(
                collisionPath.parent_path());
            std::filesystem::create_directory(collisionPath);
            writeText(
                *this,
                collisionPath / "must-survive.txt",
                "pre-existing directory");
        }
        return Persistence::FilesystemBackend::
            createDirectoryExclusive(path);
    }

    void remove(const std::string& path) override {
        if (std::filesystem::path(path) ==
            cleanupOwnershipPathForTest(collisionPath)) {
            removalAttempted = true;
            throw std::runtime_error("injected tombstone removal failure");
        }
        Persistence::FilesystemBackend::remove(path);
    }

    size_t reservationAttempts = 0;
    bool removalAttempted = false;
    std::filesystem::path collisionPath;
};

class CollidingCleanupOwnershipStorage final
    : public Persistence::FilesystemBackend {
public:
    bool createFileExclusive(
        const std::string& path,
        const std::string& contents) override {
        ++reservationAttempts;
        if (collisionPath.empty()) {
            collisionPath = path;
            CHECK(Persistence::FilesystemBackend::createFileExclusive(
                path, "unrelated cleanup entry"));
        }
        return Persistence::FilesystemBackend::createFileExclusive(
            path, contents);
    }

    size_t reservationAttempts = 0;
    std::filesystem::path collisionPath;
};

struct FailedPublisherCoordination {
    std::mutex mutex;
    std::condition_variable changed;
    std::filesystem::path abandonedStage;
    bool failedPublisherReady = false;
    bool releaseFailedPublisher = false;
};

class PausedFailedPublisherStorage final
    : public Persistence::FilesystemBackend {
public:
    explicit PausedFailedPublisherStorage(
        FailedPublisherCoordination& coordination)
        : m_coordination(coordination) {
    }

    bool createDirectoryExclusive(const std::string& path) override {
        const bool created = Persistence::FilesystemBackend::
            createDirectoryExclusive(path);
        if (created) {
            std::lock_guard lock(m_coordination.mutex);
            m_coordination.abandonedStage = path;
        }
        return created;
    }

    std::unique_ptr<Persistence::AtomicWriteSession> openWrite(
        const std::string&) override {
        std::unique_lock lock(m_coordination.mutex);
        m_coordination.failedPublisherReady = true;
        m_coordination.changed.notify_all();
        m_coordination.changed.wait(lock, [&] {
            return m_coordination.releaseFailedPublisher;
        });
        throw std::runtime_error("injected first publisher failure");
    }

    void remove(const std::string&) override {
        throw std::runtime_error("injected first publisher cleanup failure");
    }

    void waitUntilReady() {
        std::unique_lock lock(m_coordination.mutex);
        m_coordination.changed.wait(lock, [&] {
            return m_coordination.failedPublisherReady;
        });
    }

    void releaseFailure() {
        std::lock_guard lock(m_coordination.mutex);
        m_coordination.releaseFailedPublisher = true;
        m_coordination.changed.notify_all();
    }

private:
    FailedPublisherCoordination& m_coordination;
};

class WaitingRecoveryPublisherStorage final
    : public Persistence::FilesystemBackend {
public:
    explicit WaitingRecoveryPublisherStorage(
        FailedPublisherCoordination& coordination)
        : m_coordination(coordination) {
    }

    std::unique_ptr<Persistence::WorldGenerationBootstrapLock>
    lockWorldGenerationBootstrap(const std::string& worldRoot) override {
        {
            std::lock_guard lock(m_mutex);
            m_attempting = true;
            m_changed.notify_all();
        }
        auto result = Persistence::FilesystemBackend::
            lockWorldGenerationBootstrap(worldRoot);
        {
            std::lock_guard lock(m_mutex);
            m_acquired = true;
            m_changed.notify_all();
        }
        return result;
    }

    bool createDirectoryExclusive(const std::string& path) override {
        std::filesystem::path abandonedStage;
        {
            std::lock_guard lock(m_coordination.mutex);
            abandonedStage = m_coordination.abandonedStage;
        }
        cleanupCompleteBeforeReservation =
            !abandonedStage.empty() &&
            entryKind(abandonedStage.string()) ==
                Persistence::StorageEntryKind::Missing &&
            entryKind(cleanupOwnershipPathForTest(abandonedStage).string()) ==
                Persistence::StorageEntryKind::Missing;
        return Persistence::FilesystemBackend::
            createDirectoryExclusive(path);
    }

    void waitUntilAttempting() {
        std::unique_lock lock(m_mutex);
        m_changed.wait(lock, [&] { return m_attempting; });
    }

    bool waitBrieflyForAcquisition() {
        std::unique_lock lock(m_mutex);
        return m_changed.wait_for(
            lock,
            std::chrono::milliseconds(100),
            [&] { return m_acquired; });
    }

    bool cleanupCompleteBeforeReservation = false;

private:
    FailedPublisherCoordination& m_coordination;
    std::mutex m_mutex;
    std::condition_variable m_changed;
    bool m_attempting = false;
    bool m_acquired = false;
};

class SelectiveCleanupFailureStorage final
    : public Persistence::FilesystemBackend {
public:
    bool createDirectoryExclusive(const std::string& path) override {
        ++reservationAttempts;
        return Persistence::FilesystemBackend::
            createDirectoryExclusive(path);
    }

    void remove(const std::string& path) override {
        if (std::filesystem::path(path) == failingStage) {
            throw std::runtime_error("injected selective cleanup failure");
        }
        Persistence::FilesystemBackend::remove(path);
    }

    std::filesystem::path failingStage;
    size_t reservationAttempts = 0;
};

} // namespace

TEST_CASE(WorldSettings_close_reload_uses_only_saved_generator_snapshot) {
    Test::TemporaryDirectory directory("rigel_world_settings");
    const auto worldRoot = directory.path() / "world_7";
    auto storage = std::make_shared<Persistence::FilesystemBackend>();
    auto context = contextFor(worldRoot, storage);
    Voxel::WorldGenConfig installed = savedDefinition();
    const Persistence::WorldSettings settings = savedSettings();

    Persistence::publishNewWorldGeneration(settings, installed, context);
    CHECK_EQ(
        Persistence::inspectSavedWorldGeneration(context),
        Persistence::SavedWorldGenerationPresence::Published);

    installed.seed = 1u;
    installed.world.version = 999u;
    installed.world.seaLevel = -100;
    installed.densityGraph.nodes.front().value = -0.5f;
    storage.reset();

    auto reopenedStorage = std::make_shared<Persistence::FilesystemBackend>();
    context.storage = reopenedStorage;
    const Persistence::SavedWorldGeneration loaded =
        Persistence::loadSavedWorldGeneration(context);
    CHECK_EQ(loaded.settings, settings);
    CHECK_EQ(loaded.definition.seed, settings.seed);
    CHECK_EQ(
        loaded.definition.world.version,
        settings.generator.semanticsVersion);
    CHECK_EQ(loaded.definition.world.seaLevel, 0);
    CHECK_EQ(loaded.definition.densityGraph.nodes.front().value, 0.75f);

    const std::string settingsYaml = readText(
        *reopenedStorage, worldRoot / "world-settings.yaml");
    CHECK(settingsYaml.starts_with("world:\n"));
    CHECK(settingsYaml.find("schema_version: 1") != std::string::npos);
    CHECK(settingsYaml.find("seed: 424242") != std::string::npos);
    CHECK(settingsYaml.find("source_revision: 19") != std::string::npos);
    CHECK(settingsYaml.find("definition_schema_version: 1") !=
          std::string::npos);
    CHECK(settingsYaml.find("semantics_version: 1") != std::string::npos);
}

TEST_CASE(WorldSettings_publication_commits_marker_before_payload) {
    Test::TemporaryDirectory directory("rigel_world_settings");
    const auto worldRoot = directory.path() / "world_8";
    auto storage = std::make_shared<ObservingStorage>();
    const auto context = contextFor(worldRoot, storage);

    Persistence::publishNewWorldGeneration(
        savedSettings(), savedDefinition(), context);

    CHECK_EQ(storage->commits.size(), static_cast<size_t>(5));
    CHECK_EQ(
        std::filesystem::path(storage->commits[0]).filename(),
        std::filesystem::path(kStagingOwnershipFilename));
    CHECK_EQ(
        std::filesystem::path(storage->commits[1]).filename(),
        std::filesystem::path("generator-definition.yaml"));
    CHECK_EQ(
        std::filesystem::path(storage->commits[2]).filename(),
        std::filesystem::path("world-settings.yaml"));
    CHECK_EQ(
        std::filesystem::path(storage->commits[3]).filename(),
        std::filesystem::path("world.meta"));
    const std::filesystem::path stagedRoot =
        std::filesystem::path(storage->commits[0]).parent_path();
    CHECK_EQ(
        std::filesystem::path(storage->commits[4]),
        cleanupOwnershipPathForTest(stagedRoot));
    CHECK(!std::filesystem::exists(
        worldRoot / kStagingOwnershipFilename));
    CHECK(!std::filesystem::exists(
        cleanupOwnershipPathForTest(stagedRoot)));
    CHECK(std::filesystem::exists(worldRoot / "generator-definition.yaml"));
    CHECK(std::filesystem::exists(worldRoot / "world-settings.yaml"));
    CHECK(std::filesystem::exists(worldRoot / "world.meta"));
}

TEST_CASE(WorldSettings_parentless_relative_root_publishes_and_reloads) {
    Test::TemporaryDirectory directory("rigel_world_settings");
    CurrentPathGuard currentPath(directory.path());
    const std::filesystem::path relativeRoot = "world_parentless";
    const std::filesystem::path actualRoot = directory.path() / relativeRoot;
    auto storage = std::make_shared<Persistence::FilesystemBackend>();
    auto context = contextFor(relativeRoot, storage);

    Persistence::publishNewWorldGeneration(
        savedSettings(), savedDefinition(), context);
    CHECK(std::filesystem::is_directory(actualRoot));
    CHECK(!std::filesystem::exists(
        actualRoot / kStagingOwnershipFilename));
    CHECK(stagingRoots(actualRoot).empty());

    storage.reset();
    auto reopenedStorage =
        std::make_shared<Persistence::FilesystemBackend>();
    context.storage = reopenedStorage;
    const auto loaded = Persistence::loadSavedWorldGeneration(context);
    CHECK_EQ(loaded.settings, savedSettings());
    CHECK_EQ(loaded.definition.seed, savedDefinition().seed);

    Persistence::FormatRegistry formats;
    formats.registerFormat(
        Persistence::Backends::Memory::descriptor(),
        Persistence::Backends::Memory::factory(),
        Persistence::Backends::Memory::probe());
    Persistence::PersistenceService persistence(formats);
    context.discoverExistingFormat = true;
    auto format = persistence.openFormat(context);
    CHECK_EQ(format->descriptor().id, std::string("memory"));
}

TEST_CASE(WorldSettings_cr_bootstrap_publishes_and_reloads) {
    Test::TemporaryDirectory directory("rigel_world_settings");
    const auto worldRoot = directory.path() / "world_cr_lifecycle";
    auto storage = std::make_shared<Persistence::FilesystemBackend>();
    auto context = contextFor(worldRoot, storage);
    context.preferredFormat = "cr";
    Persistence::FormatRegistry formats;
    formats.registerFormat(
        Persistence::Backends::CR::descriptor(),
        Persistence::Backends::CR::factory(),
        Persistence::Backends::CR::probe());
    Persistence::PersistenceService persistence(formats);
    Voxel::BlockRegistry blocks;
    registerSavedDefinitionBlocks(blocks);
    Persistence::NewWorldGeneration creation{
        savedSettings(), savedDefinition()};

    const auto created = Persistence::bootstrapWorldGeneration(
        creation,
        persistence,
        blocks,
        context);

    CHECK_EQ(created.generation.settings, savedSettings());
    CHECK_EQ(created.persistenceFormat, std::string("cr"));
    CHECK(std::filesystem::is_directory(worldRoot));
    CHECK(stagingRoots(worldRoot).empty());
    CHECK(std::filesystem::is_regular_file(
        worldRoot / "worldInfo.json"));
    context.discoverExistingFormat = true;
    const auto createdMetadata = persistence.loadWorldMetadata(context);
    CHECK_EQ(
        createdMetadata.worldId,
        worldRoot.filename().string());
    CHECK_EQ(
        createdMetadata.displayName,
        savedSettings().displayName);

    storage.reset();
    context.storage =
        std::make_shared<Persistence::FilesystemBackend>();
    context.preferredFormat.clear();
    const auto reloaded = Persistence::bootstrapWorldGeneration(
        std::nullopt,
        persistence,
        blocks,
        context);

    CHECK_EQ(reloaded.generation.settings, savedSettings());
    CHECK_EQ(reloaded.generation.definition.seed, savedDefinition().seed);
    CHECK_EQ(reloaded.persistenceFormat, std::string("cr"));
    const auto reloadedMetadata =
        persistence.loadWorldMetadata(context);
    CHECK_EQ(
        reloadedMetadata.worldId,
        worldRoot.filename().string());
    CHECK_EQ(
        reloadedMetadata.displayName,
        savedSettings().displayName);
}

TEST_CASE(WorldSettings_rejects_corrupted_staged_world_id_before_publish) {
    Test::TemporaryDirectory directory("rigel_world_settings");
    const auto worldRoot =
        directory.path() / "world_corrupt_staged_identity";
    auto storage =
        std::make_shared<CorruptingStagedMetadataStorage>(
            StagedMetadataCorruption::WorldId);
    const auto context = contextFor(worldRoot, storage);

    CHECK_THROWS(Persistence::publishNewWorldGeneration(
        savedSettings(), savedDefinition(), context));

    CHECK(storage->metadataCorrupted);
    CHECK_EQ(storage->publishAttempts, static_cast<size_t>(0));
    CHECK(!std::filesystem::exists(worldRoot));
    CHECK(!storage->stagedRoot.empty());
    CHECK(!std::filesystem::exists(storage->stagedRoot));
    CHECK(!std::filesystem::exists(
        cleanupOwnershipPathForTest(storage->stagedRoot)));
    CHECK(stagingRoots(worldRoot).empty());
}

TEST_CASE(WorldSettings_rejects_corrupted_staged_display_name_before_publish) {
    Test::TemporaryDirectory directory("rigel_world_settings");
    const auto worldRoot =
        directory.path() / "world_corrupt_staged_display_name";
    auto storage =
        std::make_shared<CorruptingStagedMetadataStorage>(
            StagedMetadataCorruption::DisplayName);
    const auto context = contextFor(worldRoot, storage);

    CHECK_THROWS(Persistence::publishNewWorldGeneration(
        savedSettings(), savedDefinition(), context));

    CHECK(storage->metadataCorrupted);
    CHECK_EQ(storage->publishAttempts, static_cast<size_t>(0));
    CHECK(!std::filesystem::exists(worldRoot));
    CHECK(!storage->stagedRoot.empty());
    CHECK(!std::filesystem::exists(storage->stagedRoot));
    CHECK(!std::filesystem::exists(
        cleanupOwnershipPathForTest(storage->stagedRoot)));
    CHECK(stagingRoots(worldRoot).empty());
}

TEST_CASE(WorldSettings_definite_pre_rename_failure_cleans_handoff) {
    Test::TemporaryDirectory directory("rigel_world_settings");
    const auto worldRoot = directory.path() / "world_publish_failure";
    auto storage =
        std::make_shared<DefinitelyFailedPublicationStorage>();
    const auto context = contextFor(worldRoot, storage);

    CHECK_THROWS(Persistence::publishNewWorldGeneration(
        savedSettings(), savedDefinition(), context));

    CHECK(!storage->stagedRoot.empty());
    CHECK(!std::filesystem::exists(worldRoot));
    CHECK(!std::filesystem::exists(storage->stagedRoot));
    CHECK(!std::filesystem::exists(
        cleanupOwnershipPathForTest(storage->stagedRoot)));
    CHECK(stagingRoots(worldRoot).empty());
}

TEST_CASE(WorldSettings_handoff_precedes_child_marker_during_recovery) {
    Test::TemporaryDirectory directory("rigel_world_settings");
    const auto worldRoot = directory.path() / "world_handoff_child_cut";
    auto interruptedStorage =
        std::make_shared<ChildRetirementFailureStorage>();
    const auto interruptedContext = contextFor(worldRoot, interruptedStorage);

    CHECK_THROWS(Persistence::publishNewWorldGeneration(
        savedSettings(), savedDefinition(), interruptedContext));
    const std::filesystem::path stagedRoot =
        interruptedStorage->stagedRoot;
    CHECK(!std::filesystem::exists(worldRoot));
    CHECK(std::filesystem::is_regular_file(
        stagedRoot / kStagingOwnershipFilename));
    CHECK_EQ(
        readText(
            *interruptedStorage,
            cleanupOwnershipPathForTest(stagedRoot)),
        handoffOwnershipMarkerForTest(worldRoot, stagedRoot));
    CHECK(std::filesystem::exists(stagedRoot / "generator-definition.yaml"));
    CHECK(std::filesystem::exists(stagedRoot / "world-settings.yaml"));
    CHECK(std::filesystem::exists(stagedRoot / "world.meta"));

    auto restartedStorage =
        std::make_shared<Persistence::FilesystemBackend>();
    const auto restartedContext = contextFor(worldRoot, restartedStorage);
    Persistence::recoverAbandonedWorldGenerationStaging(restartedContext);
    CHECK(std::filesystem::exists(stagedRoot / "generator-definition.yaml"));
    CHECK(std::filesystem::exists(stagedRoot / "world-settings.yaml"));
    CHECK(std::filesystem::exists(stagedRoot / "world.meta"));

    Persistence::FormatRegistry formats;
    formats.registerFormat(
        Persistence::Backends::Memory::descriptor(),
        Persistence::Backends::Memory::factory(),
        Persistence::Backends::Memory::probe());
    Persistence::PersistenceService persistence(formats);
    Voxel::BlockRegistry blocks;
    registerSavedDefinitionBlocks(blocks);
    const auto bootstrapped = Persistence::bootstrapWorldGeneration(
        std::nullopt,
        persistence,
        blocks,
        restartedContext);

    CHECK_EQ(bootstrapped.generation.settings, savedSettings());
    CHECK(std::filesystem::is_directory(worldRoot));
    CHECK(!std::filesystem::exists(stagedRoot));
    CHECK(!std::filesystem::exists(
        cleanupOwnershipPathForTest(stagedRoot)));
    CHECK(!std::filesystem::exists(
        worldRoot / kStagingOwnershipFilename));
}

TEST_CASE(WorldSettings_restart_finishes_indeterminate_published_handoff) {
    Test::TemporaryDirectory directory("rigel_world_settings");
    const auto worldRoot = directory.path() / "world_handoff_restart";
    auto interruptedStorage =
        std::make_shared<IndeterminatePublicationStorage>();
    const auto interruptedContext = contextFor(worldRoot, interruptedStorage);

    CHECK_THROWS(Persistence::publishNewWorldGeneration(
        savedSettings(), savedDefinition(), interruptedContext));
    const std::filesystem::path stagedRoot =
        interruptedStorage->stagedRoot;
    CHECK(std::filesystem::is_directory(worldRoot));
    CHECK(!std::filesystem::exists(stagedRoot));
    CHECK(!std::filesystem::exists(
        worldRoot / kStagingOwnershipFilename));
    CHECK_EQ(
        readText(
            *interruptedStorage,
            cleanupOwnershipPathForTest(stagedRoot)),
        handoffOwnershipMarkerForTest(worldRoot, stagedRoot));

    auto restartedStorage =
        std::make_shared<Persistence::FilesystemBackend>();
    const auto restartedContext = contextFor(worldRoot, restartedStorage);
    Persistence::recoverAbandonedWorldGenerationStaging(restartedContext);
    CHECK(std::filesystem::exists(
        cleanupOwnershipPathForTest(stagedRoot)));

    Persistence::FormatRegistry formats;
    formats.registerFormat(
        Persistence::Backends::Memory::descriptor(),
        Persistence::Backends::Memory::factory(),
        Persistence::Backends::Memory::probe());
    Persistence::PersistenceService persistence(formats);
    Voxel::BlockRegistry blocks;
    registerSavedDefinitionBlocks(blocks);
    const auto bootstrapped = Persistence::bootstrapWorldGeneration(
        std::nullopt,
        persistence,
        blocks,
        restartedContext);

    CHECK_EQ(bootstrapped.generation.settings, savedSettings());
    CHECK_EQ(bootstrapped.persistenceFormat, std::string("memory"));
    CHECK(!std::filesystem::exists(
        cleanupOwnershipPathForTest(stagedRoot)));
    CHECK(!std::filesystem::exists(
        worldRoot / kStagingOwnershipFilename));
}

TEST_CASE(WorldSettings_handoff_final_rejects_corrupted_world_id_without_deletion) {
    Test::TemporaryDirectory directory("rigel_world_settings");
    const auto worldRoot =
        directory.path() / "world_corrupt_final_handoff_identity";
    auto interruptedStorage =
        std::make_shared<IndeterminatePublicationStorage>();
    const auto interruptedContext = contextFor(worldRoot, interruptedStorage);

    CHECK_THROWS(Persistence::publishNewWorldGeneration(
        savedSettings(), savedDefinition(), interruptedContext));
    const std::filesystem::path stagedRoot =
        interruptedStorage->stagedRoot;
    const auto metadataBefore =
        loadMemoryWorldMetadata(worldRoot, interruptedStorage);
    corruptMemoryWorldMetadataWorldId(
        *interruptedStorage, worldRoot / "world.meta");
    const auto corruptedMetadata =
        loadMemoryWorldMetadata(worldRoot, interruptedStorage);
    const std::string corruptedBytes =
        readText(*interruptedStorage, worldRoot / "world.meta");
    CHECK_NE(corruptedMetadata.worldId, metadataBefore.worldId);
    CHECK_EQ(corruptedMetadata.displayName, metadataBefore.displayName);

    auto restartedStorage =
        std::make_shared<PublishCountingStorage>();
    const auto restartedContext = contextFor(worldRoot, restartedStorage);
    Persistence::FormatRegistry formats;
    formats.registerFormat(
        Persistence::Backends::Memory::descriptor(),
        Persistence::Backends::Memory::factory(),
        Persistence::Backends::Memory::probe());
    Persistence::PersistenceService persistence(formats);
    Voxel::BlockRegistry blocks;
    registerSavedDefinitionBlocks(blocks);

    CHECK_THROWS(Persistence::bootstrapWorldGeneration(
        std::nullopt,
        persistence,
        blocks,
        restartedContext));

    CHECK_EQ(restartedStorage->publishAttempts, static_cast<size_t>(0));
    CHECK(std::filesystem::is_directory(worldRoot));
    CHECK(!std::filesystem::exists(stagedRoot));
    CHECK_EQ(
        readText(*restartedStorage, worldRoot / "world.meta"),
        corruptedBytes);
    CHECK_EQ(
        readText(
            *restartedStorage,
            cleanupOwnershipPathForTest(stagedRoot)),
        handoffOwnershipMarkerForTest(worldRoot, stagedRoot));
}

TEST_CASE(WorldSettings_restart_retires_interrupted_external_handoff) {
    Test::TemporaryDirectory directory("rigel_world_settings");
    const auto worldRoot = directory.path() / "world_handoff_retirement";
    auto interruptedStorage =
        std::make_shared<HandoffRemovalFailureStorage>();
    const auto interruptedContext = contextFor(worldRoot, interruptedStorage);

    CHECK_THROWS(Persistence::publishNewWorldGeneration(
        savedSettings(), savedDefinition(), interruptedContext));
    const std::filesystem::path stagedRoot =
        interruptedStorage->stagedRoot;
    CHECK(std::filesystem::is_directory(worldRoot));
    CHECK(!std::filesystem::exists(stagedRoot));
    CHECK(!std::filesystem::exists(
        worldRoot / kStagingOwnershipFilename));
    CHECK_EQ(
        readText(
            *interruptedStorage,
            cleanupOwnershipPathForTest(stagedRoot)),
        handoffOwnershipMarkerForTest(worldRoot, stagedRoot));

    auto restartedStorage =
        std::make_shared<Persistence::FilesystemBackend>();
    const auto restartedContext = contextFor(worldRoot, restartedStorage);
    Persistence::FormatRegistry formats;
    formats.registerFormat(
        Persistence::Backends::Memory::descriptor(),
        Persistence::Backends::Memory::factory(),
        Persistence::Backends::Memory::probe());
    Persistence::PersistenceService persistence(formats);
    Voxel::BlockRegistry blocks;
    registerSavedDefinitionBlocks(blocks);
    const auto bootstrapped = Persistence::bootstrapWorldGeneration(
        std::nullopt,
        persistence,
        blocks,
        restartedContext);

    CHECK_EQ(bootstrapped.generation.settings, savedSettings());
    CHECK(!std::filesystem::exists(
        cleanupOwnershipPathForTest(stagedRoot)));
    CHECK(std::filesystem::exists(worldRoot / "generator-definition.yaml"));
    CHECK(std::filesystem::exists(worldRoot / "world-settings.yaml"));
    CHECK(std::filesystem::exists(worldRoot / "world.meta"));
}

TEST_CASE(WorldSettings_recovery_resumes_moved_back_handoff_without_deletion) {
    Test::TemporaryDirectory directory("rigel_world_settings");
    const auto worldRoot = directory.path() / "world_moved_back_handoff";
    auto interruptedStorage =
        std::make_shared<MovedBackPublicationStorage>();
    const auto interruptedContext = contextFor(worldRoot, interruptedStorage);

    CHECK_THROWS(Persistence::publishNewWorldGeneration(
        savedSettings(), savedDefinition(), interruptedContext));
    const std::filesystem::path stagedRoot =
        interruptedStorage->stagedRoot;
    CHECK(!std::filesystem::exists(worldRoot));
    CHECK(std::filesystem::is_directory(stagedRoot));
    CHECK(!std::filesystem::exists(
        stagedRoot / kStagingOwnershipFilename));
    CHECK(std::filesystem::exists(stagedRoot / "generator-definition.yaml"));
    CHECK(std::filesystem::exists(stagedRoot / "world-settings.yaml"));
    CHECK(std::filesystem::exists(stagedRoot / "world.meta"));

    auto restartedStorage =
        std::make_shared<Persistence::FilesystemBackend>();
    const auto restartedContext = contextFor(worldRoot, restartedStorage);
    Persistence::recoverAbandonedWorldGenerationStaging(restartedContext);
    CHECK(std::filesystem::exists(stagedRoot / "generator-definition.yaml"));
    CHECK(std::filesystem::exists(stagedRoot / "world-settings.yaml"));
    CHECK(std::filesystem::exists(stagedRoot / "world.meta"));

    Persistence::FormatRegistry formats;
    formats.registerFormat(
        Persistence::Backends::Memory::descriptor(),
        Persistence::Backends::Memory::factory(),
        Persistence::Backends::Memory::probe());
    Persistence::PersistenceService persistence(formats);
    Voxel::BlockRegistry blocks;
    registerSavedDefinitionBlocks(blocks);
    const auto bootstrapped = Persistence::bootstrapWorldGeneration(
        std::nullopt,
        persistence,
        blocks,
        restartedContext);

    CHECK_EQ(bootstrapped.generation.settings, savedSettings());
    CHECK(std::filesystem::is_directory(worldRoot));
    CHECK(!std::filesystem::exists(stagedRoot));
    CHECK(!std::filesystem::exists(
        cleanupOwnershipPathForTest(stagedRoot)));
}

TEST_CASE(WorldSettings_cr_recovery_resumes_moved_back_handoff) {
    Test::TemporaryDirectory directory("rigel_world_settings");
    const auto worldRoot =
        directory.path() / "world_cr_moved_back_handoff";
    auto interruptedStorage =
        std::make_shared<MovedBackPublicationStorage>();
    auto interruptedContext =
        contextFor(worldRoot, interruptedStorage);
    interruptedContext.preferredFormat = "cr";
    Persistence::FormatRegistry formats;
    formats.registerFormat(
        Persistence::Backends::CR::descriptor(),
        Persistence::Backends::CR::factory(),
        Persistence::Backends::CR::probe());
    Persistence::PersistenceService persistence(formats);

    CHECK_THROWS(Persistence::publishNewWorldGeneration(
        savedSettings(),
        savedDefinition(),
        persistence,
        interruptedContext));
    const std::filesystem::path stagedRoot =
        interruptedStorage->stagedRoot;
    CHECK(!std::filesystem::exists(worldRoot));
    CHECK(std::filesystem::is_directory(stagedRoot));
    CHECK(std::filesystem::is_regular_file(
        stagedRoot / "worldInfo.json"));
    auto stagedContext =
        contextFor(stagedRoot, interruptedStorage);
    stagedContext.discoverExistingFormat = true;
    const auto stagedMetadata =
        persistence.loadWorldMetadata(stagedContext);
    CHECK_EQ(
        stagedMetadata.worldId,
        stagedRoot.filename().string());
    CHECK_EQ(
        stagedMetadata.displayName,
        savedSettings().displayName);

    auto restartedStorage =
        std::make_shared<Persistence::FilesystemBackend>();
    auto restartedContext =
        contextFor(worldRoot, restartedStorage);
    Voxel::BlockRegistry blocks;
    registerSavedDefinitionBlocks(blocks);
    const auto bootstrapped = Persistence::bootstrapWorldGeneration(
        std::nullopt,
        persistence,
        blocks,
        restartedContext);

    CHECK_EQ(bootstrapped.generation.settings, savedSettings());
    CHECK_EQ(bootstrapped.persistenceFormat, std::string("cr"));
    CHECK(std::filesystem::is_directory(worldRoot));
    CHECK(!std::filesystem::exists(stagedRoot));
    CHECK(!std::filesystem::exists(
        cleanupOwnershipPathForTest(stagedRoot)));
    restartedContext.discoverExistingFormat = true;
    const auto finalMetadata =
        persistence.loadWorldMetadata(restartedContext);
    CHECK_EQ(
        finalMetadata.worldId,
        worldRoot.filename().string());
    CHECK_EQ(
        finalMetadata.displayName,
        savedSettings().displayName);
}

TEST_CASE(WorldSettings_handoff_stage_rejects_corrupted_world_id_before_publish) {
    Test::TemporaryDirectory directory("rigel_world_settings");
    const auto worldRoot =
        directory.path() / "world_corrupt_staged_handoff_identity";
    auto interruptedStorage =
        std::make_shared<MovedBackPublicationStorage>();
    const auto interruptedContext = contextFor(worldRoot, interruptedStorage);

    CHECK_THROWS(Persistence::publishNewWorldGeneration(
        savedSettings(), savedDefinition(), interruptedContext));
    const std::filesystem::path stagedRoot =
        interruptedStorage->stagedRoot;
    const auto metadataBefore =
        loadMemoryWorldMetadata(stagedRoot, interruptedStorage);
    corruptMemoryWorldMetadataWorldId(
        *interruptedStorage, stagedRoot / "world.meta");
    const auto corruptedMetadata =
        loadMemoryWorldMetadata(stagedRoot, interruptedStorage);
    const std::string corruptedBytes =
        readText(*interruptedStorage, stagedRoot / "world.meta");
    CHECK_NE(corruptedMetadata.worldId, metadataBefore.worldId);
    CHECK_EQ(corruptedMetadata.displayName, metadataBefore.displayName);

    auto restartedStorage =
        std::make_shared<PublishCountingStorage>();
    const auto restartedContext = contextFor(worldRoot, restartedStorage);
    Persistence::recoverAbandonedWorldGenerationStaging(restartedContext);
    Persistence::FormatRegistry formats;
    formats.registerFormat(
        Persistence::Backends::Memory::descriptor(),
        Persistence::Backends::Memory::factory(),
        Persistence::Backends::Memory::probe());
    Persistence::PersistenceService persistence(formats);
    Voxel::BlockRegistry blocks;
    registerSavedDefinitionBlocks(blocks);

    CHECK_THROWS(Persistence::bootstrapWorldGeneration(
        std::nullopt,
        persistence,
        blocks,
        restartedContext));

    CHECK_EQ(restartedStorage->publishAttempts, static_cast<size_t>(0));
    CHECK(!std::filesystem::exists(worldRoot));
    CHECK(std::filesystem::is_directory(stagedRoot));
    CHECK_EQ(
        readText(*restartedStorage, stagedRoot / "world.meta"),
        corruptedBytes);
    CHECK_EQ(
        readText(
            *restartedStorage,
            cleanupOwnershipPathForTest(stagedRoot)),
        handoffOwnershipMarkerForTest(worldRoot, stagedRoot));
}

TEST_CASE(WorldSettings_recovery_cannot_delete_relocated_published_world) {
    Test::TemporaryDirectory directory("rigel_world_settings");
    const auto worldRoot = directory.path() / "world_relocated_after_publish";
    const auto relocatedRoot = std::filesystem::path(
        worldRoot.string() + ".staging.0");
    auto storage = std::make_shared<Persistence::FilesystemBackend>();
    const auto context = contextFor(worldRoot, storage);
    Persistence::publishNewWorldGeneration(
        savedSettings(), savedDefinition(), context);

    std::filesystem::rename(worldRoot, relocatedRoot);
    CHECK(!std::filesystem::exists(
        relocatedRoot / kStagingOwnershipFilename));
    Persistence::recoverAbandonedWorldGenerationStaging(context);

    CHECK(std::filesystem::exists(
        relocatedRoot / "generator-definition.yaml"));
    CHECK(std::filesystem::exists(
        relocatedRoot / "world-settings.yaml"));
    CHECK(std::filesystem::exists(relocatedRoot / "world.meta"));
}

TEST_CASE(WorldSettings_marker_write_failure_cleans_exclusive_reservation) {
    for (const bool failAfterCommit : {false, true}) {
        Test::TemporaryDirectory directory("rigel_world_settings");
        const auto worldRoot = directory.path() /
            (failAfterCommit ? "marker-after-commit" : "marker-before-commit");
        auto storage = std::make_shared<ObservingStorage>();
        storage->failOrdinal = 1;
        storage->failAfterCommit = failAfterCommit;
        const auto context = contextFor(worldRoot, storage);

        CHECK_THROWS(Persistence::publishNewWorldGeneration(
            savedSettings(), savedDefinition(), context));
        CHECK(!std::filesystem::exists(worldRoot));

        CHECK(stagingRoots(worldRoot).empty());
    }
}

TEST_CASE(WorldSettings_post_create_sync_failure_cleans_current_reservation) {
    Test::TemporaryDirectory directory("rigel_world_settings");
    const auto worldRoot =
        directory.path() / "world_post_create_sync_failure";
    auto storage =
        std::make_shared<PostCreateDirectorySyncFailureStorage>();
    const auto context = contextFor(worldRoot, storage);

    CHECK_THROWS(Persistence::publishNewWorldGeneration(
        savedSettings(), savedDefinition(), context));
    CHECK(!storage->failedStage.empty());
    CHECK(!std::filesystem::exists(storage->failedStage));
    CHECK(!std::filesystem::exists(
        cleanupOwnershipPathForTest(storage->failedStage)));
}

TEST_CASE(WorldSettings_markerless_ambiguous_remnants_are_bounded) {
    Test::TemporaryDirectory directory("rigel_world_settings");
    const auto worldRoot =
        directory.path() / "world_bounded_ambiguous_reservations";
    auto storage =
        std::make_shared<AmbiguousReservationFailureStorage>();
    const auto context = contextFor(worldRoot, storage);

    for (size_t slot = 0; slot < kStagingSlotCount; ++slot) {
        CHECK_THROWS(Persistence::publishNewWorldGeneration(
            savedSettings(), savedDefinition(), context));
        CHECK_EQ(storage->failedStages.size(), slot + 1);
        const auto expected = std::filesystem::path(
            worldRoot.string() + ".staging." + std::to_string(slot));
        CHECK_EQ(storage->failedStages.back(), expected);
        CHECK(std::filesystem::is_directory(expected));
        CHECK(!std::filesystem::exists(
            expected / kStagingOwnershipFilename));
        CHECK(!std::filesystem::exists(
            cleanupOwnershipPathForTest(expected)));
    }

    bool actionableExhaustion = false;
    try {
        Persistence::publishNewWorldGeneration(
            savedSettings(), savedDefinition(), context);
    } catch (const std::exception& failure) {
        actionableExhaustion =
            std::string(failure.what()).find(
                "all 64 bounded staging slots are occupied") !=
            std::string::npos;
    }
    CHECK(actionableExhaustion);
    CHECK_EQ(stagingDirectories(worldRoot).size(), kStagingSlotCount);

    auto restartedStorage =
        std::make_shared<Persistence::FilesystemBackend>();
    Persistence::recoverAbandonedWorldGenerationStaging(
        contextFor(worldRoot, restartedStorage));
    CHECK_EQ(stagingDirectories(worldRoot).size(), kStagingSlotCount);
}

TEST_CASE(WorldSettings_retries_preexisting_canonical_staging_directory) {
    Test::TemporaryDirectory directory("rigel_world_settings");
    const auto worldRoot = directory.path() / "world_directory_collision";
    auto storage = std::make_shared<CollidingReservationStorage>(
        CollidingReservationStorage::CollisionKind::Directory);
    const auto context = contextFor(worldRoot, storage);

    Persistence::publishNewWorldGeneration(
        savedSettings(), savedDefinition(), context);

    CHECK_EQ(storage->reservationAttempts, static_cast<size_t>(2));
    CHECK_EQ(
        readText(*storage, storage->collisionPath / "must-survive.txt"),
        std::string("pre-existing directory"));
    CHECK(!std::filesystem::exists(
        storage->collisionPath / kStagingOwnershipFilename));
    CHECK_EQ(
        Persistence::inspectSavedWorldGeneration(context),
        Persistence::SavedWorldGenerationPresence::Published);
}

TEST_CASE(WorldSettings_retries_preexisting_canonical_staging_file) {
    Test::TemporaryDirectory directory("rigel_world_settings");
    const auto worldRoot = directory.path() / "world_file_collision";
    auto storage = std::make_shared<CollidingReservationStorage>(
        CollidingReservationStorage::CollisionKind::RegularFile);
    const auto context = contextFor(worldRoot, storage);

    Persistence::publishNewWorldGeneration(
        savedSettings(), savedDefinition(), context);

    CHECK_EQ(storage->reservationAttempts, static_cast<size_t>(2));
    CHECK_EQ(
        readText(*storage, storage->collisionPath),
        std::string("pre-existing regular file"));
    CHECK(!std::filesystem::exists(
        cleanupOwnershipPathForTest(storage->collisionPath)));
    CHECK_EQ(
        Persistence::inspectSavedWorldGeneration(context),
        Persistence::SavedWorldGenerationPresence::Published);
}

TEST_CASE(WorldSettings_preexisting_directory_never_gains_cleanup_tombstone) {
    Test::TemporaryDirectory directory("rigel_world_settings");
    const auto worldRoot =
        directory.path() / "world_directory_tombstone_interruption";
    auto storage =
        std::make_shared<CollidingReservationRemovalFailureStorage>();
    const auto context = contextFor(worldRoot, storage);

    Persistence::publishNewWorldGeneration(
        savedSettings(), savedDefinition(), context);

    CHECK_EQ(storage->reservationAttempts, static_cast<size_t>(2));
    CHECK(!storage->removalAttempted);
    CHECK_EQ(
        readText(*storage, storage->collisionPath / "must-survive.txt"),
        std::string("pre-existing directory"));
    CHECK(!std::filesystem::exists(
        storage->collisionPath / kStagingOwnershipFilename));
    CHECK(!std::filesystem::exists(
        cleanupOwnershipPathForTest(storage->collisionPath)));

    auto restartedStorage =
        std::make_shared<Persistence::FilesystemBackend>();
    Persistence::recoverAbandonedWorldGenerationStaging(
        contextFor(worldRoot, restartedStorage));
    CHECK_EQ(
        readText(
            *restartedStorage,
            storage->collisionPath / "must-survive.txt"),
        std::string("pre-existing directory"));
}

TEST_CASE(WorldSettings_retries_preexisting_canonical_staging_symlink) {
#ifdef _WIN32
    throw Test::TestSkip(
        "Directory symlink collision is validated on POSIX platforms");
#else
    Test::TemporaryDirectory directory("rigel_world_settings");
    const auto worldRoot = directory.path() / "world_symlink_collision";
    const auto symlinkTarget = directory.path() / "external-stage-target";
    auto storage = std::make_shared<CollidingReservationStorage>(
        CollidingReservationStorage::CollisionKind::DirectorySymlink,
        symlinkTarget);
    const auto context = contextFor(worldRoot, storage);

    Persistence::publishNewWorldGeneration(
        savedSettings(), savedDefinition(), context);

    CHECK_EQ(storage->reservationAttempts, static_cast<size_t>(2));
    CHECK(std::filesystem::is_symlink(
        std::filesystem::symlink_status(storage->collisionPath)));
    CHECK_EQ(
        readText(*storage, symlinkTarget / "must-survive.txt"),
        std::string("pre-existing symlink target"));
    CHECK(!std::filesystem::exists(
        symlinkTarget / kStagingOwnershipFilename));
    CHECK_EQ(
        Persistence::inspectSavedWorldGeneration(context),
        Persistence::SavedWorldGenerationPresence::Published);
#endif
}

TEST_CASE(WorldSettings_retries_without_replacing_colliding_cleanup_entry) {
    Test::TemporaryDirectory directory("rigel_world_settings");
    const auto worldRoot =
        directory.path() / "world_cleanup_entry_collision";
    auto storage =
        std::make_shared<CollidingCleanupOwnershipStorage>();
    const auto context = contextFor(worldRoot, storage);

    Persistence::publishNewWorldGeneration(
        savedSettings(), savedDefinition(), context);

    CHECK_EQ(storage->reservationAttempts, static_cast<size_t>(2));
    CHECK_EQ(
        readText(*storage, storage->collisionPath),
        std::string("unrelated cleanup entry"));
    const std::string collisionName =
        storage->collisionPath.filename().string();
    CHECK(collisionName.ends_with(kCleanupOwnershipSuffix));
    const std::filesystem::path collidingStage =
        storage->collisionPath.parent_path() /
        collisionName.substr(
            0,
            collisionName.size() - kCleanupOwnershipSuffix.size());
    CHECK(!std::filesystem::exists(collidingStage));
    CHECK_EQ(
        Persistence::inspectSavedWorldGeneration(context),
        Persistence::SavedWorldGenerationPresence::Published);
}

TEST_CASE(WorldSettings_post_publish_failure_preserves_reused_staging_path) {
    Test::TemporaryDirectory directory("rigel_world_settings");
    const auto worldRoot = directory.path() / "world_reused_staging_path";
    auto storage =
        std::make_shared<ReusingPublishedStagingPathStorage>();
    const auto context = contextFor(worldRoot, storage);

    CHECK_THROWS(Persistence::publishNewWorldGeneration(
        savedSettings(), savedDefinition(), context));

    CHECK_EQ(
        readText(*storage, storage->reusedPath / "must-survive.txt"),
        std::string("unowned post-publish reuse"));
    CHECK(!std::filesystem::exists(
        storage->reusedPath / kStagingOwnershipFilename));
    CHECK(std::filesystem::is_regular_file(
        cleanupOwnershipPathForTest(storage->reusedPath)));
    CHECK_EQ(
        Persistence::inspectSavedWorldGeneration(context),
        Persistence::SavedWorldGenerationPresence::Published);
    CHECK(std::filesystem::exists(worldRoot / "world.meta"));
    CHECK(!std::filesystem::exists(worldRoot / "worldInfo.json"));

    Persistence::FormatRegistry formats;
    formats.registerFormat(
        Persistence::Backends::Memory::descriptor(),
        Persistence::Backends::Memory::factory(),
        Persistence::Backends::Memory::probe());
    formats.registerFormat(
        Persistence::Backends::CR::descriptor(),
        Persistence::Backends::CR::factory(),
        Persistence::Backends::CR::probe());
    Persistence::PersistenceService persistence(formats);
    auto oppositePreferenceContext = context;
    oppositePreferenceContext.preferredFormat = "cr";
    oppositePreferenceContext.discoverExistingFormat = true;
    auto reopenedFormat = persistence.openFormat(oppositePreferenceContext);
    CHECK_EQ(reopenedFormat->descriptor().id, std::string("memory"));
    CHECK(!std::filesystem::exists(worldRoot / "worldInfo.json"));

    Voxel::BlockRegistry blocks;
    registerSavedDefinitionBlocks(blocks);
    CHECK_THROWS(Persistence::bootstrapWorldGeneration(
        std::nullopt,
        persistence,
        blocks,
        context));
    CHECK(std::filesystem::exists(worldRoot / "generator-definition.yaml"));
    CHECK(std::filesystem::exists(worldRoot / "world-settings.yaml"));
    CHECK(std::filesystem::exists(worldRoot / "world.meta"));
    CHECK_EQ(
        readText(*storage, storage->reusedPath / "must-survive.txt"),
        std::string("unowned post-publish reuse"));
    CHECK(std::filesystem::is_regular_file(
        cleanupOwnershipPathForTest(storage->reusedPath)));

    auto restartedStorage =
        std::make_shared<Persistence::FilesystemBackend>();
    const auto restartedContext = contextFor(worldRoot, restartedStorage);
    Persistence::recoverAbandonedWorldGenerationStaging(restartedContext);

    CHECK_EQ(
        readText(
            *restartedStorage,
            storage->reusedPath / "must-survive.txt"),
        std::string("unowned post-publish reuse"));
    CHECK(std::filesystem::is_regular_file(
        cleanupOwnershipPathForTest(storage->reusedPath)));

    const auto displacedWorldRoot = std::filesystem::path(
        worldRoot.string() + ".displaced-after-recovery");
    std::filesystem::rename(worldRoot, displacedWorldRoot);
    CHECK(!std::filesystem::exists(worldRoot));
    Persistence::recoverAbandonedWorldGenerationStaging(restartedContext);

    CHECK_EQ(
        readText(
            *restartedStorage,
            storage->reusedPath / "must-survive.txt"),
        std::string("unowned post-publish reuse"));
    CHECK(std::filesystem::is_regular_file(
        cleanupOwnershipPathForTest(storage->reusedPath)));
    CHECK_EQ(
        Persistence::inspectSavedWorldGeneration(
            contextFor(displacedWorldRoot, restartedStorage)),
        Persistence::SavedWorldGenerationPresence::Published);
}

TEST_CASE(WorldSettings_ambiguous_publish_preserves_reused_stage_without_root) {
    Test::TemporaryDirectory directory("rigel_world_settings");
    const auto worldRoot =
        directory.path() / "world_ambiguous_publish_reuse";
    auto storage =
        std::make_shared<ReusingPublishedStagingPathStorage>();
    storage->displacePublishedRootBeforeFailure = true;
    const auto context = contextFor(worldRoot, storage);

    CHECK_THROWS(Persistence::publishNewWorldGeneration(
        savedSettings(), savedDefinition(), context));

    CHECK(!std::filesystem::exists(worldRoot));
    CHECK_EQ(
        readText(*storage, storage->reusedPath / "must-survive.txt"),
        std::string("unowned post-publish reuse"));
    CHECK(!std::filesystem::exists(
        storage->reusedPath / kStagingOwnershipFilename));
    CHECK(std::filesystem::is_regular_file(
        cleanupOwnershipPathForTest(storage->reusedPath)));
    CHECK_EQ(
        Persistence::inspectSavedWorldGeneration(
            contextFor(storage->displacedWorldRoot, storage)),
        Persistence::SavedWorldGenerationPresence::Published);
}

TEST_CASE(WorldSettings_snapshot_write_failure_rolls_back_new_world) {
    Test::TemporaryDirectory directory("rigel_world_settings");
    const auto worldRoot = directory.path() / "world_9";
    auto storage = std::make_shared<ObservingStorage>();
    storage->failOrdinal = 2;
    const auto context = contextFor(worldRoot, storage);

    CHECK_THROWS(Persistence::publishNewWorldGeneration(
        savedSettings(), savedDefinition(), context));
    CHECK(!std::filesystem::exists(worldRoot));
    CHECK_EQ(
        Persistence::inspectSavedWorldGeneration(context),
        Persistence::SavedWorldGenerationPresence::Missing);
}

TEST_CASE(WorldSettings_settings_write_failure_removes_committed_snapshot) {
    for (const bool failAfterCommit : {false, true}) {
        Test::TemporaryDirectory directory("rigel_world_settings");
        const auto worldRoot = directory.path() / "world_10";
        auto storage = std::make_shared<ObservingStorage>();
        storage->failOrdinal = 3;
        storage->failAfterCommit = failAfterCommit;
        const auto context = contextFor(worldRoot, storage);

        CHECK_THROWS(Persistence::publishNewWorldGeneration(
            savedSettings(), savedDefinition(), context));
        CHECK(!std::filesystem::exists(worldRoot));
        CHECK_EQ(
            Persistence::inspectSavedWorldGeneration(context),
            Persistence::SavedWorldGenerationPresence::Missing);
    }
}

TEST_CASE(WorldSettings_startup_recovers_failed_staging_cleanup) {
    for (const auto& [failOrdinal, hasSnapshot] :
         std::vector<std::pair<size_t, bool>>{{2, false}, {3, true}}) {
        Test::TemporaryDirectory directory("rigel_world_settings");
        const auto worldRoot = directory.path() /
            ("world_cleanup_failure_" + std::to_string(failOrdinal));
        auto storage = std::make_shared<ObservingStorage>();
        storage->failOrdinal = failOrdinal;
        storage->failRemovals = true;
        const auto context = contextFor(worldRoot, storage);

        CHECK_THROWS(Persistence::publishNewWorldGeneration(
            savedSettings(), savedDefinition(), context));
        CHECK(!std::filesystem::exists(worldRoot));
        CHECK_EQ(
            Persistence::inspectSavedWorldGeneration(context),
            Persistence::SavedWorldGenerationPresence::Missing);

        const std::vector<std::filesystem::path> abandoned =
            stagingDirectories(worldRoot);
        CHECK_EQ(abandoned.size(), static_cast<size_t>(1));
        CHECK(std::filesystem::is_regular_file(
            abandoned.front() / kStagingOwnershipFilename));
        CHECK_EQ(
            std::filesystem::exists(
                abandoned.front() / "generator-definition.yaml"),
            hasSnapshot);
        CHECK(!std::filesystem::exists(
            abandoned.front() / "world-settings.yaml"));

        auto restartedStorage =
            std::make_shared<Persistence::FilesystemBackend>();
        const auto restartedContext = contextFor(worldRoot, restartedStorage);
        Persistence::recoverAbandonedWorldGenerationStaging(restartedContext);

        CHECK(!std::filesystem::exists(abandoned.front()));
        Persistence::publishNewWorldGeneration(
            savedSettings(), savedDefinition(), restartedContext);
        CHECK_EQ(
            Persistence::inspectSavedWorldGeneration(restartedContext),
            Persistence::SavedWorldGenerationPresence::Published);
    }
}

TEST_CASE(WorldSettings_payload_cleanup_failure_preserves_child_marker) {
    Test::TemporaryDirectory directory("rigel_world_settings");
    const auto worldRoot =
        directory.path() / "world_payload_cleanup_failure";
    const auto unrelated = std::filesystem::path(
        worldRoot.string() + ".staging.7");
    std::filesystem::create_directory(unrelated);

    auto storage =
        std::make_shared<PayloadCleanupFailureStorage>();
    writeText(*storage, unrelated / "must-survive.txt", "unrelated data");
    storage->failOrdinal = 4;
    const auto context = contextFor(worldRoot, storage);

    CHECK_THROWS(Persistence::publishNewWorldGeneration(
        savedSettings(), savedDefinition(), context));
    CHECK(!std::filesystem::exists(worldRoot));

    const std::filesystem::path ownedStage = storage->failedStage;
    CHECK(!ownedStage.empty());
    CHECK(std::filesystem::is_directory(ownedStage));
    CHECK(std::filesystem::is_regular_file(
        ownedStage / kStagingOwnershipFilename));
    CHECK(std::filesystem::is_regular_file(
        cleanupOwnershipPathForTest(ownedStage)));
    CHECK(std::filesystem::is_regular_file(
        ownedStage / "generator-definition.yaml"));
    CHECK_EQ(
        readText(*storage, unrelated / "must-survive.txt"),
        std::string("unrelated data"));

    auto restartedStorage =
        std::make_shared<Persistence::FilesystemBackend>();
    const auto restartedContext = contextFor(worldRoot, restartedStorage);
    Persistence::recoverAbandonedWorldGenerationStaging(restartedContext);

    CHECK(!std::filesystem::exists(ownedStage));
    CHECK(!std::filesystem::exists(
        cleanupOwnershipPathForTest(ownedStage)));
    CHECK_EQ(
        readText(*restartedStorage, unrelated / "must-survive.txt"),
        std::string("unrelated data"));

    Persistence::publishNewWorldGeneration(
        savedSettings(), savedDefinition(), restartedContext);
    CHECK(!std::filesystem::exists(
        worldRoot / kStagingOwnershipFilename));
}

TEST_CASE(WorldSettings_recovery_reclaims_empty_tombstoned_stage) {
    Test::TemporaryDirectory directory("rigel_world_settings");
    const auto worldRoot =
        directory.path() / "world_empty_tombstoned_stage";
    auto storage =
        std::make_shared<EmptyStageRemovalFailureStorage>();
    storage->failOrdinal = 3;
    const auto context = contextFor(worldRoot, storage);

    CHECK_THROWS(Persistence::publishNewWorldGeneration(
        savedSettings(), savedDefinition(), context));
    const std::filesystem::path stagedRoot = storage->stagedRoot;
    CHECK(!stagedRoot.empty());
    CHECK(std::filesystem::is_directory(stagedRoot));
    CHECK(std::filesystem::is_empty(stagedRoot));
    CHECK(!std::filesystem::exists(
        stagedRoot / kStagingOwnershipFilename));
    CHECK(std::filesystem::is_regular_file(
        cleanupOwnershipPathForTest(stagedRoot)));

    auto restartedStorage =
        std::make_shared<Persistence::FilesystemBackend>();
    const auto restartedContext = contextFor(worldRoot, restartedStorage);
    Persistence::recoverAbandonedWorldGenerationStaging(restartedContext);

    CHECK(!std::filesystem::exists(stagedRoot));
    CHECK(!std::filesystem::exists(
        cleanupOwnershipPathForTest(stagedRoot)));
    Persistence::publishNewWorldGeneration(
        savedSettings(), savedDefinition(), restartedContext);
    CHECK(!std::filesystem::exists(
        worldRoot / kStagingOwnershipFilename));
}

TEST_CASE(WorldSettings_recovery_removes_valid_dangling_tombstone) {
    Test::TemporaryDirectory directory("rigel_world_settings");
    const auto worldRoot = directory.path() / "world_dangling_tombstone";
    const auto stagedRoot = std::filesystem::path(
        worldRoot.string() + ".staging.7");
    const auto cleanupPath = cleanupOwnershipPathForTest(stagedRoot);
    auto storage = std::make_shared<Persistence::FilesystemBackend>();
    const auto context = contextFor(worldRoot, storage);
    writeText(
        *storage,
        cleanupPath,
        cleanupOwnershipMarkerForTest(worldRoot, stagedRoot));

    CHECK(!std::filesystem::exists(stagedRoot));
    CHECK(std::filesystem::is_regular_file(cleanupPath));
    Persistence::recoverAbandonedWorldGenerationStaging(context);

    CHECK(!std::filesystem::exists(stagedRoot));
    CHECK(!std::filesystem::exists(cleanupPath));
}

TEST_CASE(WorldSettings_tombstone_only_nonempty_stage_preserves_entries) {
    Test::TemporaryDirectory directory("rigel_world_settings");
    const auto worldRoot = directory.path() / "world_tombstone_only_stage";
    const auto stagedRoot = std::filesystem::path(
        worldRoot.string() + ".staging.7");
    const auto cleanupPath = cleanupOwnershipPathForTest(stagedRoot);
    auto storage = std::make_shared<Persistence::FilesystemBackend>();
    const auto context = contextFor(worldRoot, storage);
    std::filesystem::create_directory(stagedRoot);
    writeText(*storage, stagedRoot / "must-survive.txt", "unowned stage");
    writeText(
        *storage,
        cleanupPath,
        cleanupOwnershipMarkerForTest(worldRoot, stagedRoot));

    CHECK(!std::filesystem::exists(worldRoot));
    Persistence::recoverAbandonedWorldGenerationStaging(context);

    CHECK_EQ(
        readText(*storage, stagedRoot / "must-survive.txt"),
        std::string("unowned stage"));
    CHECK(std::filesystem::is_regular_file(cleanupPath));
}

TEST_CASE(WorldSettings_publish_cleans_all_candidates_before_reservation) {
    Test::TemporaryDirectory directory("rigel_world_settings");
    const auto worldRoot = directory.path() / "world_selective_cleanup";
    const auto failingStage = std::filesystem::path(
        worldRoot.string() + ".staging.7");
    const auto removableStage = std::filesystem::path(
        worldRoot.string() + ".staging.9");
    std::filesystem::create_directory(failingStage);
    std::filesystem::create_directory(removableStage);

    auto storage =
        std::make_shared<SelectiveCleanupFailureStorage>();
    writeText(
        *storage,
        failingStage / kStagingOwnershipFilename,
        stagingOwnershipMarkerForTest(worldRoot, failingStage));
    writeText(
        *storage,
        cleanupOwnershipPathForTest(failingStage),
        cleanupOwnershipMarkerForTest(worldRoot, failingStage));
    writeText(
        *storage,
        removableStage / kStagingOwnershipFilename,
        stagingOwnershipMarkerForTest(worldRoot, removableStage));
    writeText(
        *storage,
        cleanupOwnershipPathForTest(removableStage),
        cleanupOwnershipMarkerForTest(worldRoot, removableStage));
    storage->failingStage = failingStage;
    const auto context = contextFor(worldRoot, storage);

    CHECK_THROWS(Persistence::publishNewWorldGeneration(
        savedSettings(), savedDefinition(), context));

    CHECK_EQ(storage->reservationAttempts, static_cast<size_t>(0));
    CHECK(std::filesystem::is_directory(failingStage));
    CHECK(std::filesystem::is_regular_file(
        cleanupOwnershipPathForTest(failingStage)));
    CHECK(!std::filesystem::exists(removableStage));
    CHECK(!std::filesystem::exists(
        cleanupOwnershipPathForTest(removableStage)));
    CHECK(!std::filesystem::exists(worldRoot));
}

TEST_CASE(WorldSettings_recovery_preserves_unowned_canonical_entries) {
    Test::TemporaryDirectory directory("rigel_world_settings");
    const auto worldRoot = directory.path() / "world_recovery_ownership";
    auto storage = std::make_shared<Persistence::FilesystemBackend>();
    const auto context = contextFor(worldRoot, storage);
    Persistence::publishNewWorldGeneration(
        savedSettings(), savedDefinition(), context);

    const auto unmarkedDirectory = std::filesystem::path(
        worldRoot.string() + ".staging.7");
    std::filesystem::create_directory(unmarkedDirectory);
    writeText(
        *storage, unmarkedDirectory / "must-survive.txt", "unowned directory");
    const auto malformedCleanupMarker =
        cleanupOwnershipPathForTest(unmarkedDirectory);
    writeText(
        *storage,
        malformedCleanupMarker,
        "not a Rigel cleanup marker");

    const auto wrongMarkerDirectory = std::filesystem::path(
        worldRoot.string() + ".staging.9");
    std::filesystem::create_directory(wrongMarkerDirectory);
    writeText(
        *storage,
        wrongMarkerDirectory / kStagingOwnershipFilename,
        stagingOwnershipMarkerForTest(worldRoot, unmarkedDirectory));
    writeText(
        *storage,
        wrongMarkerDirectory / "must-survive.txt",
        "marker bound to a different path");

    const auto malformedMarkerDirectory = std::filesystem::path(
        worldRoot.string() + ".staging.11");
    std::filesystem::create_directory(malformedMarkerDirectory);
    writeText(
        *storage,
        malformedMarkerDirectory / kStagingOwnershipFilename,
        "not a Rigel staging marker");

    const auto regularFile = std::filesystem::path(
        worldRoot.string() + ".staging.13");
    writeText(*storage, regularFile, "unowned regular file");

    const std::vector<std::filesystem::path> noncanonicalDirectories = {
        std::filesystem::path(worldRoot.string() + ".staging.backup"),
        std::filesystem::path(worldRoot.string() + ".staging.64"),
        std::filesystem::path(worldRoot.string() + ".staging.17.18.backup"),
        std::filesystem::path(worldRoot.string() + ".staging.017")};
    for (const auto& entry : noncanonicalDirectories) {
        std::filesystem::create_directory(entry);
        writeText(
            *storage,
            entry / kStagingOwnershipFilename,
            stagingOwnershipMarkerForTest(worldRoot, entry));
        writeText(*storage, entry / "must-survive.txt", "noncanonical name");
    }

#ifndef _WIN32
    const auto symlinkTarget = directory.path() / "unowned-symlink-target";
    std::filesystem::create_directory(symlinkTarget);
    const auto directorySymlink = std::filesystem::path(
        worldRoot.string() + ".staging.15");
    writeText(*storage, symlinkTarget / "must-survive.txt", "symlink target");
    writeText(
        *storage,
        symlinkTarget / kStagingOwnershipFilename,
        stagingOwnershipMarkerForTest(worldRoot, directorySymlink));
    std::filesystem::create_directory_symlink(
        symlinkTarget, directorySymlink);

    const auto markerSymlinkDirectory = std::filesystem::path(
        worldRoot.string() + ".staging.19");
    std::filesystem::create_directory(markerSymlinkDirectory);
    const auto externalMarker = directory.path() / "external-marker";
    writeText(
        *storage,
        externalMarker,
        stagingOwnershipMarkerForTest(worldRoot, markerSymlinkDirectory));
    std::filesystem::create_symlink(
        externalMarker,
        markerSymlinkDirectory / kStagingOwnershipFilename);
    writeText(
        *storage,
        markerSymlinkDirectory / "must-survive.txt",
        "symlink marker is not ownership");
#endif

    Persistence::recoverAbandonedWorldGenerationStaging(context);

    CHECK_EQ(
        readText(*storage, unmarkedDirectory / "must-survive.txt"),
        std::string("unowned directory"));
    CHECK_EQ(
        readText(*storage, malformedCleanupMarker),
        std::string("not a Rigel cleanup marker"));
    CHECK_EQ(
        readText(*storage, wrongMarkerDirectory / "must-survive.txt"),
        std::string("marker bound to a different path"));
    CHECK_EQ(
        readText(
            *storage,
            malformedMarkerDirectory / kStagingOwnershipFilename),
        std::string("not a Rigel staging marker"));
    CHECK_EQ(
        readText(*storage, regularFile),
        std::string("unowned regular file"));
    for (const auto& entry : noncanonicalDirectories) {
        CHECK_EQ(
            readText(*storage, entry / "must-survive.txt"),
            std::string("noncanonical name"));
    }
#ifndef _WIN32
    CHECK(std::filesystem::is_symlink(
        std::filesystem::symlink_status(directorySymlink)));
    CHECK_EQ(
        readText(*storage, symlinkTarget / "must-survive.txt"),
        std::string("symlink target"));
    CHECK_EQ(
        readText(*storage, markerSymlinkDirectory / "must-survive.txt"),
        std::string("symlink marker is not ownership"));
    CHECK(std::filesystem::is_symlink(std::filesystem::symlink_status(
        markerSymlinkDirectory / kStagingOwnershipFilename)));
#endif
}

TEST_CASE(WorldSettings_rejects_trailing_separator_world_roots) {
    Test::TemporaryDirectory directory("rigel_world_settings");
    const auto worldRoot = directory.path() / "world_trailing_separator";
    auto storage = std::make_shared<Persistence::FilesystemBackend>();
    auto context = contextFor(worldRoot, storage);
    context.rootPath += std::filesystem::path::preferred_separator;

    CHECK_THROWS(Persistence::inspectSavedWorldGeneration(context));
    CHECK_THROWS(
        Persistence::recoverAbandonedWorldGenerationStaging(context));
    CHECK_THROWS(Persistence::publishNewWorldGeneration(
        savedSettings(), savedDefinition(), context));
    CHECK_THROWS(Persistence::loadSavedWorldGeneration(context));
    CHECK(!std::filesystem::exists(worldRoot));
    CHECK(stagingRoots(worldRoot).empty());
}

TEST_CASE(WorldSettings_recovery_waits_for_live_publication) {
    Test::TemporaryDirectory directory("rigel_world_settings");
    const auto worldRoot = directory.path() / "world_recovery_publish_race";
    const auto unrelated = std::filesystem::path(
        worldRoot.string() + ".staging.7");
    std::filesystem::create_directory(unrelated);

    auto publisherStorage =
        std::make_shared<PausedPublicationStorage>();
    auto recoveryStorage =
        std::make_shared<ObservedLockAcquisitionStorage>();
    writeText(
        *publisherStorage,
        unrelated / "must-survive.txt",
        "unrelated data");
    const auto publisherContext = contextFor(worldRoot, publisherStorage);
    const auto recoveryContext = contextFor(worldRoot, recoveryStorage);

    std::exception_ptr publicationFailure;
    std::exception_ptr recoveryFailure;
    std::thread publisher([&] {
        try {
            Persistence::publishNewWorldGeneration(
                savedSettings(), savedDefinition(), publisherContext);
        } catch (...) {
            publicationFailure = std::current_exception();
        }
    });
    publisherStorage->waitUntilPublishReady();

    std::thread recovery([&] {
        try {
            Persistence::recoverAbandonedWorldGenerationStaging(
                recoveryContext);
        } catch (...) {
            recoveryFailure = std::current_exception();
        }
    });
    recoveryStorage->waitUntilAttempting();
    const bool acquiredWhilePublisherPaused =
        recoveryStorage->waitBrieflyForAcquisition();
    publisherStorage->releasePublish();
    publisher.join();
    recovery.join();

    if (publicationFailure) {
        std::rethrow_exception(publicationFailure);
    }
    if (recoveryFailure) {
        std::rethrow_exception(recoveryFailure);
    }
    CHECK(!acquiredWhilePublisherPaused);
    CHECK(recoveryStorage->acquired());
    CHECK_EQ(
        Persistence::inspectSavedWorldGeneration(recoveryContext),
        Persistence::SavedWorldGenerationPresence::Published);
    const auto loaded =
        Persistence::loadSavedWorldGeneration(recoveryContext);
    CHECK_EQ(loaded.settings, savedSettings());
    CHECK_EQ(loaded.definition.seed, savedDefinition().seed);
    CHECK_EQ(
        readText(*recoveryStorage, unrelated / "must-survive.txt"),
        std::string("unrelated data"));
}

TEST_CASE(WorldSettings_waiting_publisher_skips_ambiguous_failed_reservation) {
    Test::TemporaryDirectory directory("rigel_world_settings");
    const auto worldRoot =
        directory.path() / "world_empty_prescan_publish_race";
    FailedPublisherCoordination coordination;
    auto failedStorage =
        std::make_shared<PausedFailedPublisherStorage>(coordination);
    auto waitingStorage =
        std::make_shared<WaitingRecoveryPublisherStorage>(coordination);
    const auto failedContext = contextFor(worldRoot, failedStorage);
    const auto waitingContext = contextFor(worldRoot, waitingStorage);

    Persistence::recoverAbandonedWorldGenerationStaging(failedContext);
    Persistence::recoverAbandonedWorldGenerationStaging(waitingContext);

    std::exception_ptr failedPublication;
    std::exception_ptr waitingPublicationFailure;
    std::thread firstPublisher([&] {
        try {
            Persistence::publishNewWorldGeneration(
                savedSettings(), savedDefinition(), failedContext);
        } catch (...) {
            failedPublication = std::current_exception();
        }
    });
    failedStorage->waitUntilReady();

    std::thread waitingPublisher([&] {
        try {
            Persistence::publishNewWorldGeneration(
                savedSettings(), savedDefinition(), waitingContext);
        } catch (...) {
            waitingPublicationFailure = std::current_exception();
        }
    });
    waitingStorage->waitUntilAttempting();
    const bool acquiredWhileFirstPublisherPaused =
        waitingStorage->waitBrieflyForAcquisition();
    failedStorage->releaseFailure();
    firstPublisher.join();
    waitingPublisher.join();

    CHECK(!acquiredWhileFirstPublisherPaused);
    CHECK(failedPublication != nullptr);
    if (waitingPublicationFailure) {
        std::rethrow_exception(waitingPublicationFailure);
    }
    CHECK(!waitingStorage->cleanupCompleteBeforeReservation);
    CHECK_EQ(
        Persistence::inspectSavedWorldGeneration(waitingContext),
        Persistence::SavedWorldGenerationPresence::Published);
    CHECK_EQ(stagingDirectories(worldRoot).size(), static_cast<size_t>(1));
    CHECK(std::filesystem::is_directory(coordination.abandonedStage));
    CHECK(!std::filesystem::exists(
        coordination.abandonedStage / kStagingOwnershipFilename));
    CHECK(!std::filesystem::exists(
        cleanupOwnershipPathForTest(coordination.abandonedStage)));
}

TEST_CASE(WorldSettings_concurrent_creation_publishes_one_consistent_world) {
    Test::TemporaryDirectory directory("rigel_world_settings");
    const auto worldRoot = directory.path() / "world_concurrent";
    auto storageA = std::make_shared<Persistence::FilesystemBackend>();
    auto storageB = std::make_shared<Persistence::FilesystemBackend>();
    const auto contextA = contextFor(worldRoot, storageA);
    const auto contextB = contextFor(worldRoot, storageB);

    auto settingsA = savedSettings();
    auto definitionA = savedDefinition();
    settingsA.displayName = "Publisher A";
    settingsA.seed = 101;
    definitionA.seed = 101;
    definitionA.densityGraph.nodes.front().value = 0.25f;

    auto settingsB = savedSettings();
    auto definitionB = savedDefinition();
    settingsB.displayName = "Publisher B";
    settingsB.seed = 202;
    definitionB.seed = 202;
    definitionB.densityGraph.nodes.front().value = 0.5f;

    std::atomic<size_t> successes = 0;
    std::atomic<size_t> failures = 0;
    std::mutex startMutex;
    std::condition_variable startChanged;
    size_t ready = 0;
    auto publish = [&](const auto& settings,
                       const auto& definition,
                       const auto& context) {
        {
            std::unique_lock lock(startMutex);
            ++ready;
            startChanged.notify_all();
            startChanged.wait(lock, [&] { return ready == 2; });
        }
        try {
            Persistence::publishNewWorldGeneration(
                settings, definition, context);
            ++successes;
        } catch (...) {
            ++failures;
        }
    };
    std::thread first(
        publish,
        std::cref(settingsA),
        std::cref(definitionA),
        std::cref(contextA));
    std::thread second(
        publish,
        std::cref(settingsB),
        std::cref(definitionB),
        std::cref(contextB));
    first.join();
    second.join();

    CHECK_EQ(successes.load(), static_cast<size_t>(1));
    CHECK_EQ(failures.load(), static_cast<size_t>(1));
    const auto loaded = Persistence::loadSavedWorldGeneration(contextA);
    if (loaded.settings.displayName == settingsA.displayName) {
        CHECK_EQ(loaded.settings.seed, settingsA.seed);
        CHECK_EQ(loaded.definition.densityGraph.nodes.front().value, 0.25f);
    } else {
        CHECK_EQ(loaded.settings.displayName, settingsB.displayName);
        CHECK_EQ(loaded.settings.seed, settingsB.seed);
        CHECK_EQ(loaded.definition.densityGraph.nodes.front().value, 0.5f);
    }
}

TEST_CASE(WorldSettings_legacy_save_is_rejected_without_mutation) {
    Test::TemporaryDirectory directory("rigel_world_settings");
    const auto worldRoot = directory.path() / "world_11";
    auto storage = std::make_shared<Persistence::FilesystemBackend>();
    const auto context = contextFor(worldRoot, storage);
    const std::string legacyBytes = "legacy CR metadata bytes";
    const auto legacyPath = worldRoot / "world.meta";
    writeText(*storage, legacyPath, legacyBytes);

    CHECK_EQ(
        Persistence::inspectSavedWorldGeneration(context),
        Persistence::SavedWorldGenerationPresence::LegacyOrIncomplete);
    CHECK_THROWS(Persistence::loadSavedWorldGeneration(context));
    CHECK_THROWS(Persistence::publishNewWorldGeneration(
        savedSettings(), savedDefinition(), context));
    CHECK_EQ(readText(*storage, legacyPath), legacyBytes);
    CHECK(!std::filesystem::exists(worldRoot / "world-settings.yaml"));
    CHECK(!std::filesystem::exists(worldRoot / "generator-definition.yaml"));
}

TEST_CASE(WorldSettings_rejects_dual_runtime_identity_before_write) {
    Test::TemporaryDirectory directory("rigel_world_settings");
    auto storage = std::make_shared<Persistence::FilesystemBackend>();

    auto mismatchedSettings = savedSettings();
    mismatchedSettings.seed += 1;
    const auto seedRoot = directory.path() / "seed-mismatch";
    const auto seedContext = contextFor(seedRoot, storage);
    CHECK_THROWS(Persistence::publishNewWorldGeneration(
        mismatchedSettings, savedDefinition(), seedContext));
    CHECK(!std::filesystem::exists(seedRoot));

    auto mismatchedDefinition = savedDefinition();
    mismatchedDefinition.world.version += 1;
    const auto semanticsRoot = directory.path() / "semantics-mismatch";
    const auto semanticsContext = contextFor(semanticsRoot, storage);
    CHECK_THROWS(Persistence::publishNewWorldGeneration(
        savedSettings(), mismatchedDefinition, semanticsContext));
    CHECK(!std::filesystem::exists(semanticsRoot));
}

TEST_CASE(WorldSettings_rejects_documents_larger_than_reload_limits) {
    Test::TemporaryDirectory directory("rigel_world_settings");
    const auto worldRoot = directory.path() / "oversized-settings";
    auto storage = std::make_shared<Persistence::FilesystemBackend>();
    const auto context = contextFor(worldRoot, storage);
    auto settings = savedSettings();
    settings.generator.sourceId = "rigel:" + std::string(17 * 1024, 'x');

    CHECK_THROWS(Persistence::publishNewWorldGeneration(
        settings, savedDefinition(), context));
    CHECK(!std::filesystem::exists(worldRoot));

    const auto snapshotRoot = directory.path() / "oversized-snapshot";
    const auto snapshotContext = contextFor(snapshotRoot, storage);
    auto definition = savedDefinition();
    definition.solidBlock = std::string(4 * 1024 * 1024, 's');
    CHECK_THROWS(Persistence::publishNewWorldGeneration(
        savedSettings(), definition, snapshotContext));
    CHECK(!std::filesystem::exists(snapshotRoot));
}

TEST_CASE(WorldSettings_rejects_each_unsupported_version_without_repairing_save) {
    const std::vector<std::pair<std::string, std::string>> replacements = {
        {"schema_version: 1", "schema_version: 2"},
        {"definition_schema_version: 1", "definition_schema_version: 2"},
        {"semantics_version: 1", "semantics_version: 2"},
    };

    for (size_t index = 0; index < replacements.size(); ++index) {
        Test::TemporaryDirectory directory("rigel_world_settings");
        const auto worldRoot =
            directory.path() / ("unsupported-" + std::to_string(index));
        auto storage = std::make_shared<Persistence::FilesystemBackend>();
        const auto context = contextFor(worldRoot, storage);
        Persistence::publishNewWorldGeneration(
            savedSettings(), savedDefinition(), context);

        const auto settingsPath = worldRoot / "world-settings.yaml";
        const auto snapshotPath = worldRoot / "generator-definition.yaml";
        std::string settingsDocument = readText(*storage, settingsPath);
        const std::string snapshotBefore = readText(*storage, snapshotPath);
        const auto& [supported, unsupported] = replacements[index];
        const size_t position = settingsDocument.find(supported);
        CHECK(position != std::string::npos);
        settingsDocument.replace(position, supported.size(), unsupported);
        writeText(*storage, settingsPath, settingsDocument);

        CHECK_THROWS(Persistence::loadSavedWorldGeneration(context));
        CHECK_EQ(readText(*storage, settingsPath), settingsDocument);
        CHECK_EQ(readText(*storage, snapshotPath), snapshotBefore);
    }
}
