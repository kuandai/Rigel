#include "TestFramework.h"
#include "GeneratorDefinitionTestRegistry.h"

#include "WorldGenerationBootstrap.h"
#include "Rigel/Persistence/Backends/CR/CRFormat.h"
#include "Rigel/Persistence/Backends/Memory/MemoryFormat.h"
#include "Rigel/Persistence/Storage.h"
#include "Rigel/Voxel/BlockGalleryCatalog.h"
#include "Rigel/Voxel/BlockGalleryChunkGenerator.h"
#include "Rigel/Voxel/BlockType.h"
#include "Rigel/Voxel/GeneratorDefinition.h"

#include <condition_variable>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

using namespace Rigel;

Voxel::GeneratorDefinitionData definition(float density) {
    Voxel::GeneratorDefinitionData result =
        Test::generatorDefinitionFixture(
            "test:stone", "test:grass", "test:water");
    result.densityGraph.nodes.front().value = density;
    return result;
}

Persistence::NewWorldGeneration creation(
    uint32_t seed,
    float density,
    std::string displayName) {
    Persistence::NewWorldGeneration result;
    result.displayName = std::move(displayName);
    result.seed = seed;
    Voxel::BlockRegistry registry;
    result.definition = Test::preparedGeneratorFixture(
        definition(density), registry, "test:installed");
    return result;
}

Persistence::NewWorldGenerationFactory deferredCreation(
    Persistence::NewWorldGeneration value) {
    return [value = std::move(value)] { return value; };
}

Persistence::NewWorldGenerationFactory deferredCreation(
    const std::optional<Persistence::NewWorldGeneration>& value) {
    if (!value) {
        return {};
    }
    return deferredCreation(*value);
}

void checkStreamingMetricsEqual(
    const Voxel::ChunkStreamer::WorkMetrics& actual,
    const Voxel::ChunkStreamer::WorkMetrics& expected) {
    CHECK_EQ(actual.generationJobsStarted, expected.generationJobsStarted);
    CHECK_EQ(actual.generationJobsCompleted, expected.generationJobsCompleted);
    CHECK_EQ(actual.generationJobsCancelled, expected.generationJobsCancelled);
    CHECK_EQ(actual.generationJobsFailed, expected.generationJobsFailed);
    CHECK_EQ(
        actual.chunkLoadRequestsStarted,
        expected.chunkLoadRequestsStarted);
    CHECK_EQ(actual.meshJobsStarted, expected.meshJobsStarted);
    CHECK_EQ(actual.meshJobsCompleted, expected.meshJobsCompleted);
    CHECK_EQ(actual.meshJobsAccepted, expected.meshJobsAccepted);
    CHECK_EQ(actual.meshJobsRejectedStale, expected.meshJobsRejectedStale);
    CHECK_EQ(actual.meshJobsFailed, expected.meshJobsFailed);
    CHECK_EQ(actual.meshInvalidations, expected.meshInvalidations);
    CHECK_EQ(actual.meshRequestsCoalesced, expected.meshRequestsCoalesced);
    CHECK_EQ(
        actual.desiredBuildCoordinatesInspected,
        expected.desiredBuildCoordinatesInspected);
    CHECK_EQ(
        actual.desiredBuildCoordinatesSkippedByWorldBounds,
        expected.desiredBuildCoordinatesSkippedByWorldBounds);
    CHECK_EQ(
        actual.schedulerCoordinatesInspected,
        expected.schedulerCoordinatesInspected);
    CHECK_EQ(
        actual.cacheEvictionCoordinatesInspected,
        expected.cacheEvictionCoordinatesInspected);
    CHECK_EQ(
        actual.residentEvictionCoordinatesInspected,
        expected.residentEvictionCoordinatesInspected);
    CHECK_EQ(
        actual.deferredEvictionCoordinatesInspected,
        expected.deferredEvictionCoordinatesInspected);
    CHECK_EQ(
        actual.lastUpdateDesiredBuildCoordinatesInspected,
        expected.lastUpdateDesiredBuildCoordinatesInspected);
    CHECK_EQ(
        actual.lastUpdateDesiredBuildCoordinatesSkippedByWorldBounds,
        expected.lastUpdateDesiredBuildCoordinatesSkippedByWorldBounds);
    CHECK_EQ(
        actual.lastUpdateSchedulerCoordinatesInspected,
        expected.lastUpdateSchedulerCoordinatesInspected);
    CHECK_EQ(
        actual.lastUpdateCacheEvictionCoordinatesInspected,
        expected.lastUpdateCacheEvictionCoordinatesInspected);
    CHECK_EQ(
        actual.lastUpdateResidentEvictionCoordinatesInspected,
        expected.lastUpdateResidentEvictionCoordinatesInspected);
    CHECK_EQ(
        actual.lastUpdateDeferredEvictionCoordinatesInspected,
        expected.lastUpdateDeferredEvictionCoordinatesInspected);
}

std::string readDocument(
    Persistence::StorageBackend& storage,
    const std::filesystem::path& path) {
    auto reader = storage.openRead(path.string());
    std::string result(reader->size(), '\0');
    if (!result.empty()) {
        reader->readBytes(
            reinterpret_cast<uint8_t*>(result.data()), result.size());
    }
    return result;
}

void writeDocument(
    Persistence::StorageBackend& storage,
    const std::filesystem::path& path,
    const std::string& contents) {
    auto session = storage.openWrite(path.string());
    session->writer().writeBytes(
        reinterpret_cast<const uint8_t*>(contents.data()), contents.size());
    session->commit();
}

void configureWorldSet(
    Voxel::WorldSet& worldSet,
    const std::filesystem::path& root,
    std::shared_ptr<Persistence::StorageBackend> storage,
    const std::string& preferredFormat,
    bool includeSurfaceBlock = true) {
    worldSet.persistenceFormats().registerFormat(
        Persistence::Backends::Memory::descriptor(),
        Persistence::Backends::Memory::factory(),
        Persistence::Backends::Memory::probe());
    worldSet.persistenceFormats().registerFormat(
        Persistence::Backends::CR::descriptor(),
        Persistence::Backends::CR::factory(),
        Persistence::Backends::CR::probe());
    worldSet.setPersistenceRoot(root.string());
    worldSet.setPersistenceStorage(std::move(storage));
    worldSet.setPersistencePreferredFormat(preferredFormat);
    for (const std::string& identifier : {
             "test:stone", "test:grass", "test:water", "test:sand"}) {
        if (!includeSurfaceBlock && identifier == "test:grass") {
            continue;
        }
        Voxel::BlockType block;
        block.identifier = identifier;
        worldSet.resources().registry().registerBlock(identifier, block);
    }
}

class FailingCommitSession final : public Persistence::AtomicWriteSession {
public:
    FailingCommitSession(
        std::unique_ptr<Persistence::AtomicWriteSession> inner,
        bool failAfterCommit)
        : m_inner(std::move(inner))
        , m_failAfterCommit(failAfterCommit) {
    }

    Persistence::ByteWriter& writer() override { return m_inner->writer(); }

    void commit() override {
        if (!m_failAfterCommit) {
            throw std::runtime_error("injected backend identity failure");
        }
        m_inner->commit();
        throw std::runtime_error("injected post-commit backend identity failure");
    }

    void abort() override { m_inner->abort(); }

private:
    std::unique_ptr<Persistence::AtomicWriteSession> m_inner;
    bool m_failAfterCommit = false;
};

class FailingBackendIdentityStorage final
    : public Persistence::FilesystemBackend {
public:
    explicit FailingBackendIdentityStorage(bool failAfterCommit)
        : m_failAfterCommit(failAfterCommit) {
    }

    std::unique_ptr<Persistence::AtomicWriteSession> openWrite(
        const std::string& path) override {
        auto inner = Persistence::FilesystemBackend::openWrite(path);
        if (std::filesystem::path(path).filename() == "world.meta") {
            return std::make_unique<FailingCommitSession>(
                std::move(inner), m_failAfterCommit);
        }
        return inner;
    }

private:
    bool m_failAfterCommit = false;
};

class LockObservingStorage final
    : public Persistence::FilesystemBackend {
public:
    std::unique_ptr<Persistence::WorldGenerationBootstrapLock>
    lockWorldGenerationBootstrap(const std::string& worldRoot) override {
        {
            std::lock_guard lock(m_mutex);
            ++m_lockAttempts;
            m_changed.notify_all();
        }
        return Persistence::FilesystemBackend::lockWorldGenerationBootstrap(
            worldRoot);
    }

    void waitForLockAttempts(size_t count) {
        std::unique_lock lock(m_mutex);
        m_changed.wait(lock, [&] { return m_lockAttempts >= count; });
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_changed;
    size_t m_lockAttempts = 0;
};

class PublicationObservingStorage final
    : public Persistence::FilesystemBackend {
public:
    bool createDirectoryExclusive(const std::string& path) override {
        ++m_directoryReservationAttempts;
        return Persistence::FilesystemBackend::createDirectoryExclusive(path);
    }

    void publishDirectory(
        const std::string& stagedPath,
        const std::string& finalPath) override {
        ++m_publicationAttempts;
        Persistence::FilesystemBackend::publishDirectory(
            stagedPath, finalPath);
    }

    size_t directoryReservationAttempts() const {
        return m_directoryReservationAttempts;
    }

    size_t publicationAttempts() const { return m_publicationAttempts; }

private:
    size_t m_directoryReservationAttempts = 0;
    size_t m_publicationAttempts = 0;
};

} // namespace

TEST_CASE(ApplicationWorldGenerationBootstrap_invalid_world_id_rejects_before_publication) {
    Rigel::Test::TemporaryDirectory directory(
        "rigel_application_bootstrap_invalid_world_id");
    const auto root = directory.path() / "world_1";
    auto storage =
        std::make_shared<Rigel::Persistence::FilesystemBackend>();
    Rigel::Voxel::WorldSet worldSet;
    configureWorldSet(worldSet, root, storage, "memory");
    Rigel::Voxel::World& world = worldSet.createWorld(1);
    Rigel::Voxel::WorldView view(world, worldSet.resources());
    const auto context = worldSet.persistenceContext(1);
    worldSet.setPersistenceActiveFormat(1, "cr");
    const auto before = view.streamingMetrics();
    size_t resolverCalls = 0;
    Rigel::Persistence::NewWorldGenerationFactory resolver = [&] {
        ++resolverCalls;
        return creation(101u, 0.25f, "must not publish");
    };

    CHECK_THROWS(Rigel::detail::bootstrapApplicationWorldGeneration(
        worldSet,
        2,
        world,
        view,
        resolver,
        context));

    CHECK_EQ(resolverCalls, size_t{0});
    CHECK(!std::filesystem::exists(root));
    CHECK_EQ(
        worldSet.persistenceContext(1).preferredFormat,
        std::string("cr"));
    CHECK(world.generator() == nullptr);
    CHECK(view.generator() == nullptr);
    checkStreamingMetricsEqual(view.streamingMetrics(), before);
}

TEST_CASE(ApplicationWorldGenerationBootstrap_preowned_world_rejects_before_publication) {
    Rigel::Test::TemporaryDirectory directory(
        "rigel_application_bootstrap_preowned_world");
    const auto root = directory.path() / "world_1";
    auto storage =
        std::make_shared<Rigel::Persistence::FilesystemBackend>();
    Rigel::Voxel::WorldSet worldSet;
    configureWorldSet(worldSet, root, storage, "memory");
    Rigel::Voxel::World& world = worldSet.createWorld(1);
    auto owned = Rigel::Test::makeWorldGeneratorFixture(
        worldSet.resources().registry(), definition(-0.5f), 202u);
    world.setGenerator(owned);
    Rigel::Voxel::WorldView view(world, worldSet.resources());
    const auto context = worldSet.persistenceContext(1);
    worldSet.setPersistenceActiveFormat(1, "cr");
    const auto before = view.streamingMetrics();
    size_t resolverCalls = 0;
    Rigel::Persistence::NewWorldGenerationFactory resolver = [&] {
        ++resolverCalls;
        return creation(303u, 0.25f, "must not replace ownership");
    };

    CHECK_THROWS(Rigel::detail::bootstrapApplicationWorldGeneration(
        worldSet,
        1,
        world,
        view,
        resolver,
        context));

    CHECK_EQ(resolverCalls, size_t{0});
    CHECK(!std::filesystem::exists(root));
    CHECK_EQ(
        worldSet.persistenceContext(1).preferredFormat,
        std::string("cr"));
    CHECK_EQ(world.generator(), owned);
    CHECK_EQ(view.generator(), owned);
    checkStreamingMetricsEqual(view.streamingMetrics(), before);
}

TEST_CASE(ApplicationWorldGenerationBootstrap_mismatched_view_rejects_before_publication) {
    Rigel::Test::TemporaryDirectory directory(
        "rigel_application_bootstrap_mismatched_view");
    const auto root = directory.path() / "world_1";
    auto storage =
        std::make_shared<Rigel::Persistence::FilesystemBackend>();
    Rigel::Voxel::WorldSet worldSet;
    configureWorldSet(worldSet, root, storage, "memory");
    Rigel::Voxel::World& world = worldSet.createWorld(1);
    Rigel::Voxel::World unrelatedWorld(worldSet.resources());
    Rigel::Voxel::WorldView view(
        unrelatedWorld, worldSet.resources());
    const auto context = worldSet.persistenceContext(1);
    worldSet.setPersistenceActiveFormat(1, "cr");
    const auto before = view.streamingMetrics();
    size_t resolverCalls = 0;
    Rigel::Persistence::NewWorldGenerationFactory resolver = [&] {
        ++resolverCalls;
        return creation(404u, 0.25f, "must not split ownership");
    };

    CHECK_THROWS(Rigel::detail::bootstrapApplicationWorldGeneration(
        worldSet,
        1,
        world,
        view,
        resolver,
        context));

    CHECK_EQ(resolverCalls, size_t{0});
    CHECK(!std::filesystem::exists(root));
    CHECK_EQ(
        worldSet.persistenceContext(1).preferredFormat,
        std::string("cr"));
    CHECK(world.generator() == nullptr);
    CHECK(unrelatedWorld.generator() == nullptr);
    CHECK(view.generator() == nullptr);
    checkStreamingMetricsEqual(view.streamingMetrics(), before);
}

TEST_CASE(ApplicationWorldGenerationBootstrap_published_save_never_resolves_install) {
    Rigel::Test::TemporaryDirectory directory(
        "rigel_application_bootstrap_published_lazy");
    const auto root = directory.path() / "world_1";
    auto storage =
        std::make_shared<Rigel::Persistence::FilesystemBackend>();

    {
        Rigel::Voxel::WorldSet publishingSet;
        configureWorldSet(publishingSet, root, storage, "memory");
        Rigel::Voxel::World& world = publishingSet.createWorld(1);
        Rigel::Voxel::WorldView view(world, publishingSet.resources());
        Rigel::detail::bootstrapApplicationWorldGeneration(
            publishingSet,
            1,
            world,
            view,
            deferredCreation(creation(101u, 0.25f, "published")),
            publishingSet.persistenceContext(1));
    }

    Rigel::Voxel::WorldSet reopeningSet;
    configureWorldSet(reopeningSet, root, storage, "cr");
    Rigel::Voxel::World& world = reopeningSet.createWorld(1);
    Rigel::Voxel::WorldView view(world, reopeningSet.resources());
    size_t resolverCalls = 0;
    Rigel::Persistence::NewWorldGenerationFactory resolver = [&] {
        ++resolverCalls;
        return creation(999u, -1.0f, "must not resolve");
    };

    const auto result =
        Rigel::detail::bootstrapApplicationWorldGeneration(
            reopeningSet,
            1,
            world,
            view,
            resolver,
            reopeningSet.persistenceContext(1));

    CHECK_EQ(resolverCalls, size_t{0});
    CHECK_EQ(result.generator->seed(), 101u);
    CHECK_EQ(result.generator->definition().densityGraph.nodes.front().value,
             0.25f);
}

TEST_CASE(ApplicationWorldGenerationBootstrap_missing_save_resolves_once_without_partial_failure) {
    Rigel::Test::TemporaryDirectory directory(
        "rigel_application_bootstrap_missing_lazy_failure");
    const auto root = directory.path() / "world_1";
    auto storage =
        std::make_shared<Rigel::Persistence::FilesystemBackend>();
    Rigel::Voxel::WorldSet worldSet;
    configureWorldSet(worldSet, root, storage, "memory");
    Rigel::Voxel::World& world = worldSet.createWorld(1);
    Rigel::Voxel::WorldView view(world, worldSet.resources());
    size_t resolverCalls = 0;
    Rigel::Persistence::NewWorldGenerationFactory resolver = [&]()
        -> Rigel::Persistence::NewWorldGeneration {
        ++resolverCalls;
        throw std::runtime_error("injected installed definition failure");
    };

    CHECK_THROWS(Rigel::detail::bootstrapApplicationWorldGeneration(
        worldSet,
        1,
        world,
        view,
        resolver,
        worldSet.persistenceContext(1)));
    CHECK_EQ(resolverCalls, size_t{1});
    CHECK(world.generator() == nullptr);
    CHECK(view.generator() == nullptr);
    CHECK(!std::filesystem::exists(root));
    CHECK_EQ(view.streamingMetrics().generationJobsStarted, uint64_t{0});
    CHECK_EQ(view.streamingMetrics().chunkLoadRequestsStarted, uint64_t{0});
    CHECK_EQ(view.streamingMetrics().meshJobsStarted, uint64_t{0});
}

TEST_CASE(ApplicationWorldGenerationBootstrap_publish_while_waiting_skips_resolver) {
    Rigel::Test::TemporaryDirectory directory(
        "rigel_application_bootstrap_publish_while_waiting");
    const auto root = directory.path() / "world_1";
    auto storage = std::make_shared<LockObservingStorage>();

    Rigel::Voxel::WorldSet firstSet;
    configureWorldSet(firstSet, root, storage, "memory");
    Rigel::Voxel::World& firstWorld = firstSet.createWorld(1);
    Rigel::Voxel::WorldView firstView(firstWorld, firstSet.resources());
    Rigel::Voxel::WorldSet waitingSet;
    configureWorldSet(waitingSet, root, storage, "memory");
    Rigel::Voxel::World& waitingWorld = waitingSet.createWorld(1);
    Rigel::Voxel::WorldView waitingView(
        waitingWorld, waitingSet.resources());

    std::mutex gateMutex;
    std::condition_variable gateChanged;
    bool firstResolverEntered = false;
    bool releaseFirstResolver = false;
    size_t firstResolverCalls = 0;
    size_t waitingResolverCalls = 0;
    std::exception_ptr firstFailure;
    std::exception_ptr waitingFailure;

    Rigel::Persistence::NewWorldGenerationFactory firstResolver = [&] {
        std::unique_lock lock(gateMutex);
        ++firstResolverCalls;
        firstResolverEntered = true;
        gateChanged.notify_all();
        gateChanged.wait(lock, [&] { return releaseFirstResolver; });
        return creation(707u, 0.25f, "published under lock");
    };
    Rigel::Persistence::NewWorldGenerationFactory waitingResolver = [&] {
        ++waitingResolverCalls;
        return creation(909u, -1.0f, "must remain unused");
    };

    std::thread publisher([&] {
        try {
            static_cast<void>(
                Rigel::detail::bootstrapApplicationWorldGeneration(
                    firstSet,
                    1,
                    firstWorld,
                    firstView,
                    firstResolver,
                    firstSet.persistenceContext(1)));
        } catch (...) {
            firstFailure = std::current_exception();
        }
    });
    {
        std::unique_lock lock(gateMutex);
        gateChanged.wait(lock, [&] { return firstResolverEntered; });
    }
    std::thread waiter([&] {
        try {
            static_cast<void>(
                Rigel::detail::bootstrapApplicationWorldGeneration(
                    waitingSet,
                    1,
                    waitingWorld,
                    waitingView,
                    waitingResolver,
                    waitingSet.persistenceContext(1)));
        } catch (...) {
            waitingFailure = std::current_exception();
        }
    });
    storage->waitForLockAttempts(2);
    {
        std::lock_guard lock(gateMutex);
        releaseFirstResolver = true;
        gateChanged.notify_all();
    }
    publisher.join();
    waiter.join();

    CHECK(firstFailure == nullptr);
    CHECK(waitingFailure == nullptr);
    CHECK_EQ(firstResolverCalls, size_t{1});
    CHECK_EQ(waitingResolverCalls, size_t{0});
    CHECK_EQ(waitingWorld.generator()->seed(), 707u);
    CHECK_EQ(
        waitingWorld.generator()->definition().densityGraph.nodes.front().value,
        0.25f);
}

TEST_CASE(ApplicationWorldGenerationBootstrap_failure_never_installs_generator) {
    for (const bool failAfterCommit : {false, true}) {
        Rigel::Test::TemporaryDirectory directory(
            failAfterCommit
                ? "rigel_application_bootstrap_after_commit"
                : "rigel_application_bootstrap_before_commit");
        const auto root = directory.path() / "world_1";
        auto storage = std::make_shared<FailingBackendIdentityStorage>(
            failAfterCommit);
        Rigel::Voxel::WorldSet worldSet;
        configureWorldSet(worldSet, root, storage, "memory");
        Rigel::Voxel::World& world = worldSet.createWorld(1);
        Rigel::Voxel::WorldView view(world, worldSet.resources());
        const auto context = worldSet.persistenceContext(1);

        CHECK_THROWS(Rigel::detail::bootstrapApplicationWorldGeneration(
            worldSet,
            1,
            world,
            view,
            deferredCreation(creation(101, 0.25f, "failure")),
            context));
        CHECK(world.generator() == nullptr);
        CHECK(view.generator() == nullptr);
        CHECK(!std::filesystem::exists(root));
        CHECK_EQ(
            Rigel::Persistence::inspectSavedWorldGeneration(context),
            Rigel::Persistence::SavedWorldGenerationPresence::Missing);
    }
}

TEST_CASE(ApplicationWorldGenerationBootstrap_malformed_save_starts_no_generation) {
    Rigel::Test::TemporaryDirectory directory(
        "rigel_application_bootstrap_malformed_save");
    const auto root = directory.path() / "world_1";
    auto storage =
        std::make_shared<Rigel::Persistence::FilesystemBackend>();
    writeDocument(
        *storage,
        root / "world-settings.yaml",
        "world: invalid\n");
    writeDocument(
        *storage,
        root / "generator-definition.yaml",
        Rigel::Voxel::serializeGeneratorDefinitionSnapshot(
            definition(0.25f)));

    Rigel::Voxel::WorldSet worldSet;
    configureWorldSet(worldSet, root, storage, "memory");
    Rigel::Voxel::World& world = worldSet.createWorld(1);
    Rigel::Voxel::WorldView view(world, worldSet.resources());
    const auto before = view.streamingMetrics();

    CHECK_THROWS(Rigel::detail::bootstrapApplicationWorldGeneration(
        worldSet,
        1,
        world,
        view,
        deferredCreation(creation(999u, -1.0f, "installed fallback")),
        worldSet.persistenceContext(1)));
    CHECK(world.generator() == nullptr);
    CHECK(view.generator() == nullptr);
    CHECK_EQ(
        view.streamingMetrics().generationJobsStarted,
        before.generationJobsStarted);
    CHECK_EQ(view.streamingMetrics().generationJobsStarted, uint64_t{0});
    CHECK_EQ(view.streamingMetrics().chunkLoadRequestsStarted, uint64_t{0});
    CHECK_EQ(view.streamingMetrics().meshJobsStarted, uint64_t{0});
}

TEST_CASE(ApplicationWorldGenerationBootstrap_reload_uses_saved_snapshot) {
    Rigel::Test::TemporaryDirectory directory("rigel_application_bootstrap_reload");
    const auto root = directory.path() / "world_1";
    const auto installedAtCreation = creation(111, 0.25f, "saved world");

    {
        auto storage = std::make_shared<Rigel::Persistence::FilesystemBackend>();
        Rigel::Voxel::WorldSet worldSet;
        configureWorldSet(worldSet, root, storage, "memory");
        Rigel::Voxel::World& world = worldSet.createWorld(1);
        Rigel::Voxel::WorldView view(world, worldSet.resources());
        const auto result = Rigel::detail::bootstrapApplicationWorldGeneration(
            worldSet,
            1,
            world,
            view,
            deferredCreation(installedAtCreation),
            worldSet.persistenceContext(1));
        CHECK_EQ(result.generator->seed(), 111u);
        CHECK_EQ(result.generator, world.generator());
        CHECK_EQ(result.generator, view.generator());
    }

    auto changedInstall = creation(222, 0.75f, "changed install");
    changedInstall.definition.data.terrain.solidMaterial =
        "test:missing-from-install";
    for (const auto& installed : {
             std::optional<Rigel::Persistence::NewWorldGeneration>(
                 changedInstall),
             std::optional<Rigel::Persistence::NewWorldGeneration>()}) {
        auto storage = std::make_shared<Rigel::Persistence::FilesystemBackend>();
        Rigel::Voxel::WorldSet worldSet;
        configureWorldSet(worldSet, root, storage, "cr");
        Rigel::Voxel::World& world = worldSet.createWorld(1);
        Rigel::Voxel::WorldView view(world, worldSet.resources());
        const auto result = Rigel::detail::bootstrapApplicationWorldGeneration(
            worldSet,
            1,
            world,
            view,
            deferredCreation(installed),
            worldSet.persistenceContext(1));
        CHECK_EQ(result.persistenceFormat, std::string("memory"));
        CHECK_EQ(result.generator->seed(), 111u);
        CHECK_EQ(
            result.generator->definition().densityGraph.nodes.front().value,
            0.25f);
        CHECK_EQ(result.generator, world.generator());
        CHECK_EQ(result.generator, view.generator());
    }
}

TEST_CASE(ApplicationWorldGenerationBootstrap_invalid_creation_starts_no_generation) {
    Rigel::Test::TemporaryDirectory directory(
        "rigel_application_bootstrap_invalid_creation");
    const auto root = directory.path() / "world_1";
    auto input = creation(600u, 0.25f, "invalid world");
    input.definition.data.biomes.entries.clear();
    auto storage = std::make_shared<Rigel::Persistence::FilesystemBackend>();
    Rigel::Voxel::WorldSet worldSet;
    configureWorldSet(worldSet, root, storage, "memory");
    Rigel::Voxel::World& world = worldSet.createWorld(1);
    Rigel::Voxel::WorldView view(world, worldSet.resources());

    CHECK_THROWS(Rigel::detail::bootstrapApplicationWorldGeneration(
        worldSet,
        1,
        world,
        view,
        deferredCreation(input),
        worldSet.persistenceContext(1)));
    CHECK(world.generator() == nullptr);
    CHECK(view.generator() == nullptr);
    CHECK(!std::filesystem::exists(root));
    CHECK_EQ(view.streamingMetrics().generationJobsStarted, uint64_t{0});
}

TEST_CASE(ApplicationWorldGenerationBootstrap_gallery_bounds_reject_before_publication) {
    Rigel::Test::TemporaryDirectory directory(
        "rigel_application_bootstrap_gallery_bounds");
    const auto root = directory.path() / "world_1";
    auto storage =
        std::make_shared<PublicationObservingStorage>();
    Rigel::Voxel::WorldSet worldSet;
    configureWorldSet(worldSet, root, storage, "memory");
    worldSet.resources().registry().freeze();
    auto catalog =
        std::make_shared<const Rigel::Voxel::BlockGalleryCatalog>(
            worldSet.resources().registry());
    auto gallery =
        std::make_shared<const Rigel::Voxel::BlockGalleryChunkGenerator>(
            worldSet.resources().registry(), std::move(catalog));
    Rigel::Voxel::World& world = worldSet.createWorld(1);
    Rigel::Voxel::WorldView view(world, worldSet.resources());
    const auto context = worldSet.persistenceContext(1);
    worldSet.setPersistenceActiveFormat(1, "cr");
    const auto before = view.streamingMetrics();
    size_t resolverCalls = 0;
    Rigel::Persistence::NewWorldGenerationFactory resolver = [&] {
        ++resolverCalls;
        return creation(601u, 0.25f, "incompatible gallery");
    };

    CHECK_THROWS(Rigel::detail::bootstrapApplicationWorldGeneration(
        worldSet,
        1,
        world,
        view,
        resolver,
        context,
        gallery));

    CHECK_EQ(resolverCalls, size_t{1});
    CHECK_EQ(storage->directoryReservationAttempts(), size_t{0});
    CHECK_EQ(storage->publicationAttempts(), size_t{0});
    CHECK(!std::filesystem::exists(root));
    for (const auto& entry : std::filesystem::directory_iterator(
             directory.path())) {
        CHECK(!entry.path().filename().string().starts_with(
            "world_1.staging."));
    }
    CHECK_EQ(
        Rigel::Persistence::inspectSavedWorldGeneration(context),
        Rigel::Persistence::SavedWorldGenerationPresence::Missing);
    CHECK_EQ(
        worldSet.persistenceContext(1).preferredFormat,
        std::string("cr"));
    CHECK(world.generator() == nullptr);
    CHECK(view.generator() == nullptr);
    checkStreamingMetricsEqual(view.streamingMetrics(), before);
}

TEST_CASE(ApplicationWorldGenerationBootstrap_markerless_save_fails_unchanged) {
    Rigel::Test::TemporaryDirectory directory(
        "rigel_application_bootstrap_markerless");
    const auto root = directory.path() / "world_1";
    const auto saved = creation(333, 0.5f, "markerless world");
    auto storage = std::make_shared<Rigel::Persistence::FilesystemBackend>();

    {
        Rigel::Voxel::WorldSet worldSet;
        configureWorldSet(worldSet, root, storage, "memory");
        Rigel::Voxel::World& world = worldSet.createWorld(1);
        Rigel::Voxel::WorldView view(world, worldSet.resources());
        Rigel::detail::bootstrapApplicationWorldGeneration(
            worldSet,
            1,
            world,
            view,
            deferredCreation(saved),
            worldSet.persistenceContext(1));
        storage->remove((root / "world.meta").string());
    }

    for (const std::string preferredFormat : {"memory", "cr"}) {
        Rigel::Voxel::WorldSet worldSet;
        configureWorldSet(worldSet, root, storage, preferredFormat);
        Rigel::Voxel::World& world = worldSet.createWorld(1);
        Rigel::Voxel::WorldView view(world, worldSet.resources());

        CHECK_THROWS(Rigel::detail::bootstrapApplicationWorldGeneration(
            worldSet,
            1,
            world,
            view,
            Rigel::Persistence::NewWorldGenerationFactory{},
            worldSet.persistenceContext(1)));
        CHECK(world.generator() == nullptr);
        CHECK(view.generator() == nullptr);
        CHECK(!std::filesystem::exists(root / "world.meta"));
        CHECK(!std::filesystem::exists(root / "worldInfo.json"));
        CHECK(std::filesystem::is_regular_file(
            root / "world-settings.yaml"));
        CHECK(std::filesystem::is_regular_file(
            root / "generator-definition.yaml"));
    }
}

TEST_CASE(ApplicationWorldGenerationBootstrap_weak_evidence_fails_unchanged) {
    Rigel::Test::TemporaryDirectory directory(
        "rigel_application_bootstrap_weak_evidence");
    const auto root = directory.path() / "world_1";
    const auto saved = creation(444, 0.5f, "weak evidence world");

    auto storage = std::make_shared<Rigel::Persistence::FilesystemBackend>();
    {
        Rigel::Voxel::WorldSet publishingSet;
        configureWorldSet(publishingSet, root, storage, "memory");
        Rigel::Voxel::World& publishingWorld = publishingSet.createWorld(1);
        Rigel::Voxel::WorldView publishingView(
            publishingWorld, publishingSet.resources());
        Rigel::detail::bootstrapApplicationWorldGeneration(
            publishingSet,
            1,
            publishingWorld,
            publishingView,
            deferredCreation(saved),
            publishingSet.persistenceContext(1));
    }
    storage->remove((root / "world.meta").string());
    storage->mkdirs((root / "zones").string());

    Rigel::Voxel::WorldSet reopeningSet;
    configureWorldSet(reopeningSet, root, storage, "memory");
    Rigel::Voxel::World& world = reopeningSet.createWorld(1);
    Rigel::Voxel::WorldView view(world, reopeningSet.resources());
    CHECK_THROWS(Rigel::detail::bootstrapApplicationWorldGeneration(
        reopeningSet,
        1,
        world,
        view,
        Rigel::Persistence::NewWorldGenerationFactory{},
        reopeningSet.persistenceContext(1)));
    CHECK(world.generator() == nullptr);
    CHECK(view.generator() == nullptr);
    CHECK(!std::filesystem::exists(root / "world.meta"));
    CHECK(!std::filesystem::exists(root / "worldInfo.json"));
    CHECK(std::filesystem::exists(root / "world-settings.yaml"));
    CHECK(std::filesystem::exists(root / "generator-definition.yaml"));
    CHECK(std::filesystem::is_directory(root / "zones"));
}

TEST_CASE(ApplicationWorldGenerationBootstrap_corrupt_backend_identity_fails_unchanged) {
    Rigel::Test::TemporaryDirectory directory(
        "rigel_application_bootstrap_corrupt_identity");
    const auto root = directory.path() / "world_1";
    const auto markerPath = root / "world.meta";
    auto storage = std::make_shared<Rigel::Persistence::FilesystemBackend>();

    {
        Rigel::Voxel::WorldSet publishingSet;
        configureWorldSet(publishingSet, root, storage, "memory");
        Rigel::Voxel::World& publishingWorld = publishingSet.createWorld(1);
        Rigel::Voxel::WorldView publishingView(
            publishingWorld, publishingSet.resources());
        Rigel::detail::bootstrapApplicationWorldGeneration(
            publishingSet,
            1,
            publishingWorld,
            publishingView,
            deferredCreation(
                creation(445, 0.5f, "corrupt identity world")),
            publishingSet.persistenceContext(1));
    }

    const std::string corruptMarker = "not valid metadata";
    writeDocument(*storage, markerPath, corruptMarker);

    Rigel::Voxel::WorldSet reopeningSet;
    configureWorldSet(reopeningSet, root, storage, "cr");
    Rigel::Voxel::World& world = reopeningSet.createWorld(1);
    Rigel::Voxel::WorldView view(world, reopeningSet.resources());
    CHECK_THROWS(Rigel::detail::bootstrapApplicationWorldGeneration(
        reopeningSet,
        1,
        world,
        view,
        Rigel::Persistence::NewWorldGenerationFactory{},
        reopeningSet.persistenceContext(1)));
    CHECK(world.generator() == nullptr);
    CHECK(view.generator() == nullptr);
    CHECK_EQ(readDocument(*storage, markerPath), corruptMarker);
    CHECK(!std::filesystem::exists(root / "worldInfo.json"));
    CHECK(std::filesystem::is_regular_file(root / "world-settings.yaml"));
    CHECK(std::filesystem::is_regular_file(
        root / "generator-definition.yaml"));
}

TEST_CASE(ApplicationWorldGenerationBootstrap_invalid_saved_content_is_not_claimed) {
    Rigel::Test::TemporaryDirectory directory(
        "rigel_application_bootstrap_invalid_saved_content");
    const auto root = directory.path() / "world_1";

    auto storage = std::make_shared<Rigel::Persistence::FilesystemBackend>();
    {
        Rigel::Voxel::WorldSet publishingSet;
        configureWorldSet(publishingSet, root, storage, "memory");
        Rigel::Voxel::World& publishingWorld = publishingSet.createWorld(1);
        Rigel::Voxel::WorldView publishingView(
            publishingWorld, publishingSet.resources());
        Rigel::detail::bootstrapApplicationWorldGeneration(
            publishingSet,
            1,
            publishingWorld,
            publishingView,
            deferredCreation(
                creation(555, 0.5f, "unavailable content world")),
            publishingSet.persistenceContext(1));
    }
    Rigel::Voxel::WorldSet reopeningSet;
    configureWorldSet(reopeningSet, root, storage, "memory", false);
    Rigel::Voxel::World& world = reopeningSet.createWorld(1);
    Rigel::Voxel::WorldView view(world, reopeningSet.resources());
    CHECK_THROWS(Rigel::detail::bootstrapApplicationWorldGeneration(
        reopeningSet,
        1,
        world,
        view,
        Rigel::Persistence::NewWorldGenerationFactory{},
        reopeningSet.persistenceContext(1)));
    CHECK(world.generator() == nullptr);
    CHECK(view.generator() == nullptr);
    CHECK(std::filesystem::is_regular_file(root / "world.meta"));
    CHECK(!std::filesystem::exists(root / "worldInfo.json"));
}
