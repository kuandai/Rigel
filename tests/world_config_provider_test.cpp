#include "TestFramework.h"

#include "Rigel/Asset/AssetManager.h"
#include "Rigel/Config/ConfigSource.h"
#include "Rigel/Voxel/WorldConfigBootstrap.h"
#include "Rigel/Voxel/WorldConfigProvider.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace {

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

    CHECK_EQ(streaming.viewDistanceChunks, 9);
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

TEST_CASE(WorldConfigBootstrap_uses_dedicated_shipped_streaming_asset) {
    Rigel::Asset::AssetManager assets;
    assets.loadManifest("manifest.yaml");

    CHECK(assets.exists("raw/streaming_config"));
    CHECK(!assets.exists("raw/world_config"));
    const Rigel::Voxel::StreamingConfig streaming =
        Rigel::Voxel::makeWorldConfigProvider(assets, 17)
            .loadStreamingConfig();
    CHECK_EQ(streaming.viewDistanceChunks, 12);
    CHECK_EQ(streaming.unloadDistanceChunks, 13);
}
