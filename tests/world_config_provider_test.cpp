#include "TestFramework.h"

#include "Rigel/Asset/AssetManager.h"
#include "Rigel/Config/ConfigSource.h"
#include "Rigel/Voxel/WorldConfigBootstrap.h"
#include "Rigel/Voxel/WorldConfigProvider.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace {

class CurrentPathGuard final {
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

void writeConfig(const std::filesystem::path& path, std::string_view content) {
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path());
    }
    std::ofstream output(path);
    output << content;
}

class TrackingConfigSource final : public Rigel::Config::IConfigSource {
public:
    TrackingConfigSource(std::string sourceName, std::string yaml)
        : m_sourceName(std::move(sourceName))
        , m_yaml(std::move(yaml)) {
    }

    std::optional<std::string> load() const override {
        ++loadCalls;
        return m_yaml;
    }

    std::string name() const override { return m_sourceName; }

    mutable size_t loadCalls = 0;

private:
    std::string m_sourceName;
    std::string m_yaml;
};

} // namespace

TEST_CASE(WorldConfigProvider_streaming_ignores_legacy_generation_authoring) {
    Rigel::Voxel::WorldConfigProvider provider;
    auto source = std::make_unique<TrackingConfigSource>(
        "streaming.yaml",
        "generator: invalid\n"
        "flags: [not, a, mapping]\n"
        "overlays: definitely-not-a-sequence\n"
        "generation:\n"
        "  stages: invalid\n"
        "terrain: [legacy, data]\n"
        "streaming:\n"
        "  view_distance_chunks: 9\n"
        "  worker_threads: 3\n");
    TrackingConfigSource* tracked = source.get();
    provider.addSource(std::move(source));

    const Rigel::Voxel::StreamingConfig streaming =
        provider.loadStreamingConfig();

    CHECK_EQ(streaming.viewDistanceChunks, 6);
    CHECK_EQ(streaming.workerThreads, 3);
    CHECK_EQ(tracked->loadCalls, size_t{1});
}

TEST_CASE(WorldConfigProvider_streaming_layers_publish_only_valid_replacements) {
    Rigel::Voxel::WorldConfigProvider provider;
    provider.addSource(std::make_unique<TrackingConfigSource>(
        "base-streaming.yaml",
        "streaming:\n"
        "  view_distance_chunks: 8\n"
        "  worker_threads: 2\n"));
    provider.addSource(std::make_unique<TrackingConfigSource>(
        "invalid-streaming.yaml",
        "streaming:\n"
        "  view_distance_chunks: 10\n"
        "  worker_threads: 64\n"
        "  io_threads: 1\n"));

    CHECK_THROWS(provider.loadStreamingConfig());
}

TEST_CASE(WorldConfigProvider_streaming_validates_after_all_layers_merge) {
    Rigel::Voxel::WorldConfigProvider provider;
    provider.addSource(std::make_unique<TrackingConfigSource>(
        "base-streaming.yaml",
        "streaming:\n"
        "  worker_threads: 64\n"));
    provider.addSource(std::make_unique<TrackingConfigSource>(
        "override-streaming.yaml",
        "streaming:\n"
        "  io_threads: 0\n"
        "  load_worker_threads: 0\n"));

    const Rigel::Voxel::StreamingConfig streaming =
        provider.loadStreamingConfig();

    CHECK_EQ(streaming.workerThreads, 64);
    CHECK_EQ(streaming.ioThreads, 0);
    CHECK_EQ(streaming.loadWorkerThreads, 0);
}

TEST_CASE(WorldConfigBootstrap_uses_dedicated_shipped_streaming_asset) {
    Rigel::Asset::AssetManager assets;
    assets.loadManifest("manifest.yaml");

    CHECK(assets.exists("raw/streaming_config"));
    CHECK(!assets.exists("raw/world_config"));
    const Rigel::Voxel::StreamingConfig streaming =
        Rigel::Voxel::makeWorldConfigProvider(assets, 17)
            .loadStreamingConfig();
    CHECK_EQ(streaming.viewDistanceChunks, 6);
    CHECK_EQ(streaming.unloadDistanceChunks, 8);
}

TEST_CASE(WorldConfigBootstrap_reads_streaming_paths_only) {
    Rigel::Test::TemporaryDirectory directory(
        "rigel_streaming_source_names");
    CurrentPathGuard currentPath(directory.path());
    writeConfig("world_generation.yaml", "generation: [unterminated\n");
    writeConfig(
        "config/world_generation.yaml",
        "flags: [unterminated\n");
    writeConfig(
        "config/worlds/17/world_generation.yaml",
        "overlays: [unterminated\n");
    writeConfig(
        "streaming.yaml",
        "streaming:\n"
        "  view_distance_chunks: 9\n"
        "  worker_threads: 9\n");

    Rigel::Asset::AssetManager assets;
    assets.loadManifest("manifest.yaml");
    const Rigel::Voxel::StreamingConfig streaming =
        Rigel::Voxel::makeWorldConfigProvider(assets, 17)
            .loadStreamingConfig();

    CHECK_EQ(streaming.viewDistanceChunks, 6);
    CHECK_EQ(streaming.unloadDistanceChunks, 8);
    CHECK_EQ(streaming.workerThreads, 9);
}
