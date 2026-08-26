#include "TestFramework.h"

#include "WorldGenerationBootstrap.h"
#include "Rigel/Persistence/Backends/CR/CRFormat.h"
#include "Rigel/Persistence/Backends/Memory/MemoryFormat.h"
#include "Rigel/Persistence/Storage.h"
#include "Rigel/Voxel/BlockType.h"
#include "Rigel/Voxel/GeneratorSnapshot.h"

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

using namespace Rigel;

Voxel::WorldGenConfig definition(uint32_t seed, float density) {
    Voxel::WorldGenConfig result;
    result.seed = seed;
    result.world.version = Voxel::kGeneratorSemanticsVersion;
    result.solidBlock = "test:stone";
    result.surfaceBlock = "test:grass";
    result.waterBlock = "test:water";
    result.shoreBlock = "test:sand";

    Voxel::WorldGenConfig::BiomeConfig biome;
    biome.name = "land";
    biome.surface.push_back({"test:grass", 1});
    result.biomes.entries.push_back(std::move(biome));

    Voxel::WorldGenConfig::DensityNodeConfig node;
    node.id = "ground";
    node.type = "constant";
    node.value = density;
    result.densityGraph.nodes.push_back(std::move(node));
    result.densityGraph.outputs["base_density"] = "ground";
    result.stageEnabled["caves"] = false;
    result.stageEnabled["structures"] = false;
    return result;
}

Persistence::NewWorldGeneration creation(
    uint32_t seed,
    float density,
    std::string displayName) {
    Persistence::NewWorldGeneration result;
    result.settings.displayName = std::move(displayName);
    result.settings.seed = seed;
    result.settings.generator.sourceId = "test:installed";
    result.settings.generator.sourceRevision = 1;
    result.settings.generator.definitionSchemaVersion =
        Voxel::kGeneratorDefinitionSchemaVersion;
    result.settings.generator.semanticsVersion =
        Voxel::kGeneratorSemanticsVersion;
    result.definition = definition(seed, density);
    return result;
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
    bool includeShoreBlock = true) {
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
        if (!includeShoreBlock && identifier == "test:sand") {
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

} // namespace

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
            worldSet, 1, world, view, creation(101, 0.25f, "failure"), context));
        CHECK(world.generator() == nullptr);
        CHECK(view.generator() == nullptr);
        CHECK(!std::filesystem::exists(root));
        CHECK_EQ(
            Rigel::Persistence::inspectSavedWorldGeneration(context),
            Rigel::Persistence::SavedWorldGenerationPresence::Missing);
    }
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
            installedAtCreation,
            worldSet.persistenceContext(1));
        CHECK_EQ(result.generator->config().seed, 111u);
        CHECK_EQ(result.generator, world.generator());
        CHECK_EQ(result.generator, view.generator());
    }

    auto changedInstall = creation(222, 0.75f, "changed install");
    changedInstall.definition.shoreBlock = "test:missing-from-install";
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
            installed,
            worldSet.persistenceContext(1));
        CHECK_EQ(result.persistenceFormat, std::string("memory"));
        CHECK_EQ(result.generator->config().seed, 111u);
        CHECK_EQ(
            result.generator->config().densityGraph.nodes.front().value,
            0.25f);
        CHECK_EQ(result.generator, world.generator());
        CHECK_EQ(result.generator, view.generator());
    }
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
            worldSet, 1, world, view, saved, worldSet.persistenceContext(1));
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
            std::nullopt,
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
            saved,
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
        std::nullopt,
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
            creation(445, 0.5f, "corrupt identity world"),
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
        std::nullopt,
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
            creation(555, 0.5f, "unavailable content world"),
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
        std::nullopt,
        reopeningSet.persistenceContext(1)));
    CHECK(world.generator() == nullptr);
    CHECK(view.generator() == nullptr);
    CHECK(std::filesystem::is_regular_file(root / "world.meta"));
    CHECK(!std::filesystem::exists(root / "worldInfo.json"));
}
