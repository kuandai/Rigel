#include "TestFramework.h"

#include "Rigel/Persistence/Storage.h"
#include "Rigel/Persistence/WorldSettings.h"
#include "Rigel/Voxel/GeneratorSnapshot.h"

#include <filesystem>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace Rigel;

namespace {

Voxel::WorldGenConfig savedDefinition() {
    Voxel::WorldGenConfig definition;
    definition.seed = 424242u;
    definition.world.version = 19u;
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

class ObservingStorage final : public Persistence::FilesystemBackend {
public:
    size_t failOrdinal = std::numeric_limits<size_t>::max();
    bool failAfterCommit = false;
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

private:
    size_t m_openOrdinal = 0;
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
        settings.generator.sourceRevision);
    CHECK_EQ(loaded.definition.world.seaLevel, 0);
    CHECK_EQ(loaded.definition.densityGraph.nodes.front().value, 0.75f);

    const std::string settingsYaml = readText(
        *reopenedStorage, worldRoot / "world-settings.yaml");
    CHECK(settingsYaml.find("schema_version: 1") != std::string::npos);
    CHECK(settingsYaml.find("seed: 424242") != std::string::npos);
    CHECK(settingsYaml.find("source_revision: 19") != std::string::npos);
    CHECK(settingsYaml.find("definition_schema_version: 1") !=
          std::string::npos);
    CHECK(settingsYaml.find("semantics_version: 1") != std::string::npos);
}

TEST_CASE(WorldSettings_publication_commits_snapshot_before_settings) {
    Test::TemporaryDirectory directory("rigel_world_settings");
    const auto worldRoot = directory.path() / "world_8";
    auto storage = std::make_shared<ObservingStorage>();
    const auto context = contextFor(worldRoot, storage);

    Persistence::publishNewWorldGeneration(
        savedSettings(), savedDefinition(), context);

    CHECK_EQ(storage->commits.size(), static_cast<size_t>(2));
    CHECK_EQ(
        std::filesystem::path(storage->commits[0]).filename(),
        std::filesystem::path("generator-definition.yaml"));
    CHECK_EQ(
        std::filesystem::path(storage->commits[1]).filename(),
        std::filesystem::path("world-settings.yaml"));
    CHECK(std::filesystem::exists(worldRoot / "generator-definition.yaml"));
    CHECK(std::filesystem::exists(worldRoot / "world-settings.yaml"));
}

TEST_CASE(WorldSettings_snapshot_write_failure_rolls_back_new_world) {
    Test::TemporaryDirectory directory("rigel_world_settings");
    const auto worldRoot = directory.path() / "world_9";
    auto storage = std::make_shared<ObservingStorage>();
    storage->failOrdinal = 1;
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
        storage->failOrdinal = 2;
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

TEST_CASE(WorldSettings_rejects_dual_seed_or_revision_authority_before_write) {
    Test::TemporaryDirectory directory("rigel_world_settings");
    auto storage = std::make_shared<Persistence::FilesystemBackend>();

    auto mismatchedSettings = savedSettings();
    mismatchedSettings.seed += 1;
    const auto seedRoot = directory.path() / "seed-mismatch";
    const auto seedContext = contextFor(seedRoot, storage);
    CHECK_THROWS(Persistence::publishNewWorldGeneration(
        mismatchedSettings, savedDefinition(), seedContext));
    CHECK(!std::filesystem::exists(seedRoot));

    mismatchedSettings = savedSettings();
    mismatchedSettings.generator.sourceRevision += 1;
    const auto revisionRoot = directory.path() / "revision-mismatch";
    const auto revisionContext = contextFor(revisionRoot, storage);
    CHECK_THROWS(Persistence::publishNewWorldGeneration(
        mismatchedSettings, savedDefinition(), revisionContext));
    CHECK(!std::filesystem::exists(revisionRoot));
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
