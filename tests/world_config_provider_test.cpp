#include "TestFramework.h"

#include "Rigel/Voxel/WorldConfigProvider.h"
#include "Rigel/Voxel/WorldConfigBootstrap.h"
#include "Rigel/Voxel/GeneratorSnapshot.h"
#include "Rigel/Asset/Types.h"
#include "Rigel/Persistence/Storage.h"
#include "Rigel/Persistence/WorldSettings.h"

#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <stdexcept>
#include <unordered_map>

using namespace Rigel::Voxel;
using namespace Rigel::Config;

namespace {

class MemoryConfigSource : public IConfigSource {
public:
    MemoryConfigSource(std::string sourceName,
                       std::string yaml,
                       std::unordered_map<std::string, std::string> paths = {})
        : m_name(std::move(sourceName))
        , m_yaml(std::move(yaml))
        , m_paths(std::move(paths))
    {}

    std::optional<std::string> load() const override {
        return m_yaml;
    }

    std::string name() const override {
        return m_name;
    }

    std::optional<ConfigSourceResult> loadPath(std::string_view path) const override {
        auto it = m_paths.find(std::string(path));
        if (it == m_paths.end()) {
            return std::nullopt;
        }
        return ConfigSourceResult{m_name + ":" + it->first, it->second};
    }

private:
    std::string m_name;
    std::string m_yaml;
    std::unordered_map<std::string, std::string> m_paths;
};

class FixedRawLoader final : public Rigel::Asset::IAssetLoader {
public:
    explicit FixedRawLoader(std::string yaml)
        : m_yaml(std::move(yaml)) {
    }

    std::string_view category() const override {
        return "raw";
    }

    std::shared_ptr<Rigel::Asset::AssetBase> load(
        const Rigel::Asset::LoadContext&) override {
        auto asset = std::make_shared<Rigel::Asset::RawAsset>();
        asset->data.assign(m_yaml.begin(), m_yaml.end());
        return asset;
    }

private:
    std::string m_yaml;
};

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

std::string withGeneratorSource(
    std::string yaml,
    std::string_view id = "rigel:test",
    uint32_t revision = 7) {
    return "generator:\n"
        "  id: " + std::string(id) + "\n"
        "  source_revision: " + std::to_string(revision) + "\n" + yaml;
}

} // namespace

TEST_CASE(WorldConfigProvider_FileSource) {
    Rigel::Test::TemporaryDirectory directory("rigel_world_config");
    const auto path = directory.path() / "world.yaml";
    {
        std::ofstream out(path);
        out << withGeneratorSource("");
        out << "seed: 99\n";
        out << "solid_block: base:stone_shale\n";
    }

    WorldConfigProvider provider;
    provider.addSource(std::make_unique<FileConfigSource>(path.string()));
    WorldGenConfig config = provider.loadConfig().generation;

    CHECK_EQ(config.seed, static_cast<uint32_t>(99));
    CHECK_EQ(config.solidBlock, "base:stone_shale");
}

TEST_CASE(WorldConfigProvider_UsesExplicitHighestPrecedenceGeneratorSource) {
    WorldConfigProvider provider;
    provider.addSource(std::make_unique<MemoryConfigSource>(
        "shipped definition",
        withGeneratorSource(
            "seed: 11\nworld:\n  sea_level: 10\n",
            "rigel:default",
            4)));
    provider.addSource(std::make_unique<MemoryConfigSource>(
        "selected definition",
        withGeneratorSource(
            "seed: 22\nworld:\n  sea_level: 20\n",
            "example:skylands",
            9)));

    const WorldConfiguration config = provider.loadConfig();
    CHECK_EQ(config.generatorSource.id, std::string("example:skylands"));
    CHECK_EQ(config.generatorSource.revision, 9u);
    CHECK_EQ(config.generation.seed, 22u);
    CHECK_EQ(config.generation.world.seaLevel, 20);
}

TEST_CASE(WorldConfigProvider_RejectsUnknownOrInapplicableCreationFields) {
    for (const std::string yaml : {
             "density_grph:\n  outputs: {}\n",
             "world:\n  version: 12\n",
             "generator:\n"
             "  id: rigel:duplicate\n"
             "  source_revision: 8\n",
             "density_graph:\n"
             "  nodes:\n"
             "    - id: ground\n"
             "      type: constant\n"
             "      value: 1\n"
             "      offset: 2\n",
             "density_graph:\n  nodes: {}\n",
             "density_graph:\n"
             "  nodes:\n"
             "    - type: constant\n"
             "      value: 1\n",
             "density_graph:\n"
             "  nodes:\n"
             "    - id: duplicate\n"
             "      type: constant\n"
             "      value: 1\n"
             "    - id: duplicate\n"
             "      type: constant\n"
             "      value: 2\n",
             "density_graph:\n"
             "  nodes:\n"
             "    - id: curve\n"
             "      type: spline\n"
             "      inputs: [source]\n"
             "      spline:\n"
             "        - {x: 1}\n",
             "biomes:\n  entries: {}\n"}) {
        WorldConfigProvider provider;
        provider.addSource(std::make_unique<MemoryConfigSource>(
            "invalid definition", withGeneratorSource(yaml)));
        CHECK_THROWS(provider.loadConfig());
    }
}

TEST_CASE(WorldConfigProvider_RequiresCompleteGeneratorSourceIdentity) {
    for (const std::string yaml : std::initializer_list<std::string>{
             "seed: 11\n",
             "generator:\n  id: rigel:missing-revision\n",
             "generator:\n  id: unnamespaced\n  source_revision: 1\n",
             "generator:\n  id: rigel:zero\n  source_revision: 0\n",
             withGeneratorSource("seed: 11\n")}) {
        WorldConfigProvider provider;
        provider.addSource(std::make_unique<MemoryConfigSource>(
            "invalid identity", yaml));
        CHECK_THROWS(provider.loadConfig());
    }
}

TEST_CASE(WorldConfigProvider_CouplesDefinitionChangesToSourceIdentity) {
    WorldConfigProvider inheritedIdentity;
    inheritedIdentity.addSource(std::make_unique<MemoryConfigSource>(
        "base definition",
        withGeneratorSource(
            "world:\n  sea_level: 10\n", "rigel:base", 2)));
    inheritedIdentity.addSource(std::make_unique<MemoryConfigSource>(
        "unidentified definition change",
        "terrain:\n  surface_depth: 4\n"));
    CHECK_THROWS(inheritedIdentity.loadConfig());

    WorldConfigProvider unidentifiedOverlay;
    unidentifiedOverlay.addSource(std::make_unique<MemoryConfigSource>(
        "base definition",
        withGeneratorSource(
            "world:\n  sea_level: 10\n", "rigel:base", 2)));
    unidentifiedOverlay.addSource(std::make_unique<MemoryConfigSource>(
        "unidentified overlay source",
        "overlays:\n  - path: tuning.yaml\n",
        std::unordered_map<std::string, std::string>{
            {"tuning.yaml", "terrain:\n  surface_depth: 4\n"}
        }));
    CHECK_THROWS(unidentifiedOverlay.loadConfig());

    WorldConfigProvider overlayIdentity;
    overlayIdentity.addSource(std::make_unique<MemoryConfigSource>(
        "declaring definition",
        withGeneratorSource(
            "world:\n  sea_level: 10\n"
            "overlays:\n  - path: tuning.yaml\n",
            "rigel:declaring",
            3),
        std::unordered_map<std::string, std::string>{
            {
                "tuning.yaml",
                withGeneratorSource(
                    "terrain:\n  surface_depth: 4\n",
                    "rigel:overlay",
                    4)
            }
        }));
    CHECK_THROWS(overlayIdentity.loadConfig());

    WorldConfigProvider creationChoice;
    creationChoice.addSource(std::make_unique<MemoryConfigSource>(
        "base definition",
        withGeneratorSource(
            "world:\n  sea_level: 10\n", "rigel:base", 2)));
    creationChoice.addSource(std::make_unique<MemoryConfigSource>(
        "creation choice",
        "seed: 99\nstreaming:\n  view_distance_chunks: 8\n"));
    const WorldConfiguration config = creationChoice.loadConfig();
    CHECK_EQ(config.generatorSource.id, std::string("rigel:base"));
    CHECK_EQ(config.generatorSource.revision, 2u);
    CHECK_EQ(config.generation.seed, 99u);
    CHECK_EQ(config.streaming.viewDistanceChunks, 8);
}

TEST_CASE(WorldConfigProvider_OverlaySource) {
    Rigel::Test::TemporaryDirectory directory("rigel_world_config");
    const auto basePath = directory.path() / "base.yaml";
    const auto overlayPath = directory.path() / "overlay.yaml";
    {
        std::ofstream out(basePath);
        out << withGeneratorSource("");
        out << "flags:\n";
        out << "  smooth: true\n";
        out << "overlays:\n";
        out << "  - path: overlay.yaml\n";
        out << "    when: smooth\n";
        out << "terrain:\n";
        out << "  base_height: 1.0\n";
    }
    {
        std::ofstream out(overlayPath);
        out << "terrain:\n";
        out << "  base_height: 9.0\n";
    }

    WorldConfigProvider provider;
    provider.addSource(std::make_unique<FileConfigSource>(basePath.string()));
    WorldGenConfig config = provider.loadConfig().generation;

    CHECK_NEAR(config.terrain.baseHeight, 9.0f, 0.001f);
}

TEST_CASE(WorldConfigProvider_MissingRelativeOverlayIsFatal) {
    Rigel::Test::TemporaryDirectory directory("rigel_missing_relative_overlay");
    const auto basePath = directory.path() / "base.yaml";
    {
        std::ofstream out(basePath);
        out << "terrain:\n";
        out << "  base_height: 9.0\n";
        out << "overlays:\n";
        out << "  - path: missing.yaml\n";
    }

    WorldConfigProvider provider;
    provider.addSource(std::make_unique<FileConfigSource>(basePath.string()));

    std::string diagnostic;
    try {
        (void)provider.loadConfig();
    } catch (const std::runtime_error& error) {
        diagnostic = error.what();
    }
    CHECK_EQ(
        diagnostic,
        "Missing configuration overlay '" +
            (directory.path() / "missing.yaml").string() +
            "' declared by '" + basePath.string() + "'");
}

TEST_CASE(WorldConfigProvider_MissingEmbeddedOverlayIsFatal) {
    Rigel::Asset::AssetManager assets;
    assets.registerLoader(
        "raw",
        std::make_unique<FixedRawLoader>(
            "overlays:\n"
            "  - path: assets/config/definitely_missing_overlay.yaml\n"));
    assets.loadManifest("manifest.yaml");

    WorldConfigProvider provider;
    provider.addSource(std::make_unique<EmbeddedConfigSource>(
        assets, "raw/world_config"));

    std::string diagnostic;
    try {
        (void)provider.loadConfig();
    } catch (const std::runtime_error& error) {
        diagnostic = error.what();
    }
    CHECK_EQ(
        diagnostic,
        "Missing configuration overlay 'config/definitely_missing_overlay.yaml' "
        "declared by 'raw/world_config'");
}

TEST_CASE(WorldConfigProvider_HigherPrecedenceSourceOverridesLowerOverlay) {
    WorldConfigProvider provider;
    provider.addSource(std::make_unique<MemoryConfigSource>(
        "defaults",
        withGeneratorSource(
            "terrain:\n"
            "  base_height: 1.0\n"
            "streaming:\n"
            "  worker_threads: 1\n"
            "overlays:\n"
            "  - path: tuning.yaml\n"),
        std::unordered_map<std::string, std::string>{
            {
                "tuning.yaml",
                "terrain:\n"
                "  base_height: 2.0\n"
                "streaming:\n"
                "  worker_threads: 2\n"
            }
        }
    ));
    provider.addSource(std::make_unique<MemoryConfigSource>(
        "world",
        withGeneratorSource(
            "terrain:\n"
            "  base_height: 9.0\n"
            "streaming:\n"
            "  worker_threads: 3\n",
            "rigel:world",
            1)
    ));

    const WorldConfiguration config = provider.loadConfig();

    CHECK_NEAR(config.generation.terrain.baseHeight, 9.0f, 0.001f);
    CHECK_EQ(config.streaming.workerThreads, 3);
}

TEST_CASE(WorldConfigProvider_ValidatesCrossFieldsAfterAllSourcesMerge) {
    WorldConfigProvider provider;
    provider.addSource(std::make_unique<MemoryConfigSource>(
        "base",
        withGeneratorSource(
            "world:\n"
            "  min_y: 400\n"
            "streaming:\n"
            "  worker_threads: 64\n")
    ));
    provider.addSource(std::make_unique<MemoryConfigSource>(
        "override",
        withGeneratorSource(
            "world:\n"
            "  max_y: 500\n"
            "streaming:\n"
            "  io_threads: 0\n"
            "  load_worker_threads: 0\n",
            "rigel:override",
            1)
    ));

    const WorldConfiguration config = provider.loadConfig();
    CHECK_EQ(config.generation.world.minY, 400);
    CHECK_EQ(config.generation.world.maxY, 500);
    CHECK_EQ(config.streaming.workerThreads, 64);
    CHECK_EQ(config.streaming.ioThreads, 0);
    CHECK_EQ(config.streaming.loadWorkerThreads, 0);
}

TEST_CASE(WorldConfigProvider_ReportsInvalidFinalCrossFields) {
    WorldConfigProvider provider;
    provider.addSource(std::make_unique<MemoryConfigSource>(
        "base",
        "streaming:\n"
        "  worker_threads: 61\n"
        "  io_threads: 2\n"
        "  load_worker_threads: 2\n"
    ));

    std::string message;
    try {
        (void)provider.loadConfig();
    } catch (const std::invalid_argument& error) {
        message = error.what();
    }
    CHECK_EQ(
        message,
        "Invalid configuration value 'streaming.worker_threads' in "
        "'merged world configuration': combined worker_threads, io_threads, "
        "and load_worker_threads must not exceed 64"
    );
}

TEST_CASE(WorldConfigProvider_StreamingLoadDoesNotResolveGeneratorContent) {
    WorldConfigProvider provider;
    provider.addSource(std::make_unique<MemoryConfigSource>(
        "installed definition",
        "world:\n"
        "  min_y: 500\n"
        "  max_y: 0\n"
        "overlays:\n"
        "  - path: missing-definition-overlay.yaml\n"
        "streaming:\n"
        "  view_distance_chunks: 9\n"
    ));

    const StreamingConfig streaming = provider.loadStreamingConfig();
    CHECK_EQ(streaming.viewDistanceChunks, 9);
    CHECK_THROWS(provider.loadConfig());
}

TEST_CASE(WorldConfigProvider_SavedWorldReloadToleratesRemovedFileOverlay) {
    Rigel::Test::TemporaryDirectory directory("rigel_removed_world_overlay");
    const auto sourcePath = directory.path() / "world_generation.yaml";
    const auto overlayPath = directory.path() / "installed-overlay.yaml";
    const auto worldRoot = directory.path() / "world_0";
    {
        std::ofstream source(sourcePath);
        source <<
            "generator:\n"
            "  id: rigel:removed-overlay\n"
            "  source_revision: 27\n"
            "seed: 90817\n"
            "solid_block: base:stone_shale\n"
            "surface_block: base:grass\n"
            "world:\n"
            "  sea_level: 0\n"
            "biomes:\n"
            "  entries:\n"
            "    - name: land\n"
            "      surface:\n"
            "        - block: base:grass\n"
            "          depth: 1\n"
            "density_graph:\n"
            "  outputs:\n"
            "    base_density: ground\n"
            "  nodes:\n"
            "    - id: ground\n"
            "      type: constant\n"
            "      value: 0.75\n"
            "generation:\n"
            "  stages:\n"
            "    caves: false\n"
            "    structures: false\n"
            "flags:\n"
            "  installed_tuning: true\n"
            "overlays:\n"
            "  - path: installed-overlay.yaml\n"
            "    when: installed_tuning\n"
            "streaming:\n"
            "  view_distance_chunks: 9\n";
    }
    {
        std::ofstream overlay(overlayPath);
        overlay <<
            "world:\n"
            "  sea_level: 42\n"
            "streaming:\n"
            "  worker_threads: 4\n";
    }

    WorldConfigProvider provider;
    provider.addSource(std::make_unique<FileConfigSource>(sourcePath.string()));
    WorldConfiguration created = provider.loadConfig();
    CHECK_EQ(created.generation.world.seaLevel, 42);

    Rigel::Persistence::WorldSettings settings;
    settings.displayName = "Removed overlay reload";
    settings.seed = created.generation.seed;
    settings.generator.sourceId = created.generatorSource.id;
    settings.generator.sourceRevision = created.generatorSource.revision;
    settings.generator.definitionSchemaVersion =
        kGeneratorDefinitionSchemaVersion;
    settings.generator.semanticsVersion = kGeneratorSemanticsVersion;
    created.generation.world.version = kGeneratorSemanticsVersion;
    auto storage = std::make_shared<Rigel::Persistence::FilesystemBackend>();
    Rigel::Persistence::PersistenceContext context;
    context.rootPath = worldRoot.string();
    context.storage = storage;
    Rigel::Persistence::publishNewWorldGeneration(
        settings, created.generation, context);

    CHECK(std::filesystem::remove(overlayPath));
    const StreamingConfig streaming = provider.loadStreamingConfig();
    const Rigel::Persistence::SavedWorldGeneration saved =
        Rigel::Persistence::loadSavedWorldGeneration(context);

    CHECK_EQ(streaming.viewDistanceChunks, 9);
    CHECK_EQ(saved.settings, settings);
    CHECK_EQ(saved.definition.world.seaLevel, 42);
    CHECK_EQ(saved.definition.seed, settings.seed);
    CHECK_THROWS(provider.loadConfig());
}

TEST_CASE(WorldConfigProvider_StreamingLoadPreservesConditionalNestedOverlays) {
    WorldConfigProvider provider;
    provider.addSource(std::make_unique<MemoryConfigSource>(
        "installed definition",
        "world:\n"
        "  min_y: 500\n"
        "  max_y: 0\n"
        "flags:\n"
        "  tuning: true\n"
        "  disabled: false\n"
        "overlays:\n"
        "  - path: tuning.yaml\n"
        "    when: tuning\n"
        "  - path: disabled.yaml\n"
        "    when: disabled\n"
        "streaming:\n"
        "  view_distance_chunks: 7\n",
        std::unordered_map<std::string, std::string>{
            {
                "tuning.yaml",
                "flags:\n"
                "  nested: true\n"
                "overlays:\n"
                "  - path: nested.yaml\n"
                "    when: nested\n"
                "streaming:\n"
                "  worker_threads: 4\n"
            },
            {
                "nested.yaml",
                "streaming:\n"
                "  view_distance_chunks: 10\n"
                "  worker_threads: 5\n"
            },
            {
                "disabled.yaml",
                "streaming:\n"
                "  view_distance_chunks: 16\n"
            }
        }));
    provider.addSource(std::make_unique<MemoryConfigSource>(
        "per-world override",
        "overlays:\n"
        "  - path: per-world-tuning.yaml\n"
        "    when: tuning\n"
        "streaming:\n"
        "  view_distance_chunks: 11\n",
        std::unordered_map<std::string, std::string>{
            {
                "per-world-tuning.yaml",
                "streaming:\n"
                "  view_distance_chunks: 12\n"
                "  worker_threads: 6\n"
            }
        }));

    const StreamingConfig streaming = provider.loadStreamingConfig();
    CHECK_EQ(streaming.viewDistanceChunks, 12);
    CHECK_EQ(streaming.workerThreads, 6);
    CHECK_THROWS(provider.loadConfig());
}

TEST_CASE(WorldConfigProvider_ShippedDefinitionProducesNormalizedSnapshot) {
    Rigel::Asset::AssetManager assets;
    assets.loadManifest("manifest.yaml");
    const WorldConfiguration config =
        makeWorldConfigProvider(assets, 0).loadConfig();
    CHECK_EQ(config.generatorSource.id, std::string("rigel:default"));
    CHECK_EQ(config.generatorSource.revision, 1u);

    const std::string snapshot =
        serializeGeneratorSnapshot(config.generation);
    const WorldGenConfig loaded = parseGeneratorSnapshot(
        snapshot,
        kGeneratorDefinitionSchemaVersion,
        config.generation.seed,
        kGeneratorSemanticsVersion);

    CHECK(loaded.densityGraph.nodes.size() <
          config.generation.densityGraph.nodes.size());
    CHECK(snapshot.find("base_height") == std::string::npos);
    CHECK(snapshot.find("height_noise") == std::string::npos);
    CHECK(snapshot.find("generation:") == std::string::npos);
    CHECK_EQ(loaded.seed, config.generation.seed);
}

TEST_CASE(WorldConfigProvider_OverlayUsesDeclaringSource) {
    WorldConfigProvider provider;
    provider.addSource(std::make_unique<MemoryConfigSource>(
        "defaults",
        withGeneratorSource(
            "overlays:\n"
            "  - path: tuning.yaml\n",
            "rigel:defaults",
            3),
        std::unordered_map<std::string, std::string>{
            {"tuning.yaml", "terrain:\n  base_height: 2.0\n"}
        }
    ));
    provider.addSource(std::make_unique<MemoryConfigSource>(
        "world",
        withGeneratorSource(
            "overlays:\n"
            "  - path: tuning.yaml\n",
            "rigel:world",
            1),
        std::unordered_map<std::string, std::string>{
            {"tuning.yaml", "terrain:\n  base_height: 10.0\n"}
        }
    ));

    WorldGenConfig config = provider.loadConfig().generation;

    CHECK_NEAR(config.terrain.baseHeight, 10.0f, 0.001f);
}

TEST_CASE(WorldConfigProvider_AppliesNestedOverlaysAfterDeclaredOverlays) {
    WorldConfigProvider provider;
    provider.addSource(std::make_unique<MemoryConfigSource>(
        "defaults",
        withGeneratorSource(
            "terrain:\n"
            "  base_height: 1.0\n"
            "overlays:\n"
            "  - path: first.yaml\n"
            "  - path: second.yaml\n"),
        std::unordered_map<std::string, std::string>{
            {
                "first.yaml",
                "terrain:\n"
                "  base_height: 2.0\n"
                "overlays:\n"
                "  - path: nested.yaml\n"
            },
            {"second.yaml", "terrain:\n  base_height: 3.0\n"},
            {"nested.yaml", "terrain:\n  base_height: 4.0\n"}
        }
    ));

    WorldGenConfig config = provider.loadConfig().generation;

    CHECK_NEAR(config.terrain.baseHeight, 4.0f, 0.001f);
}

TEST_CASE(FileConfigSource_ResolvesOverlayRelativeToDeclaringFile) {
    Rigel::Test::TemporaryDirectory directory("rigel_config_relative_overlay");
    const auto sourceDir = directory.path() / "source";
    const auto workingDir = directory.path() / "working";
    std::filesystem::create_directories(sourceDir);
    std::filesystem::create_directories(workingDir);
    {
        std::ofstream out(sourceDir / "overlay.yaml");
        out << "terrain:\n  base_height: 9.0\n";
    }
    {
        std::ofstream out(workingDir / "overlay.yaml");
        out << "terrain:\n  base_height: 2.0\n";
    }

    std::optional<ConfigSourceResult> result;
    {
        CurrentPathGuard workingDirectory(workingDir);
        FileConfigSource source((sourceDir / "world_generation.yaml").string());
        result = source.loadPath("overlay.yaml");
    }

    CHECK(result.has_value());
    if (result) {
        CHECK(result->content.find("9.0") != std::string::npos);
        CHECK(result->content.find("2.0") == std::string::npos);
    }
}
