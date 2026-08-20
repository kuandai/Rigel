#include "TestFramework.h"

#include "Rigel/Persistence/PersistenceConfig.h"
#include "Rigel/Voxel/WorldConfigProvider.h"

#include <spdlog/logger.h>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>

using namespace Rigel::Voxel;

namespace {

class LogCapture {
public:
    LogCapture()
        : m_previous(spdlog::default_logger())
        , m_logger(std::make_shared<spdlog::logger>(
              "config-diagnostic-test",
              std::make_shared<spdlog::sinks::ostream_sink_mt>(m_output))) {
        m_logger->set_level(spdlog::level::warn);
        m_logger->set_pattern("%v");
        spdlog::set_default_logger(m_logger);
    }

    ~LogCapture() {
        spdlog::set_default_logger(m_previous);
    }

    std::string output() {
        m_logger->flush();
        return m_output.str();
    }

private:
    std::ostringstream m_output;
    std::shared_ptr<spdlog::logger> m_previous;
    std::shared_ptr<spdlog::logger> m_logger;
};

class NamedConfigSource : public IConfigSource {
public:
    NamedConfigSource(std::string sourceName, std::string yaml)
        : m_sourceName(std::move(sourceName))
        , m_yaml(std::move(yaml))
    {}

    std::optional<std::string> load() const override {
        return m_yaml;
    }

    std::string name() const override {
        return m_sourceName;
    }

private:
    std::string m_sourceName;
    std::string m_yaml;
};

std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path);
    CHECK(input.good());
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()
    );
}

} // namespace

TEST_CASE(WorldGenConfig_ReportsUnknownKeyAndSource) {
    LogCapture logs;
    WorldGenConfig config;

    config.applyYaml(
        "world-settings.yaml",
        "terrain:\n"
        "  height_variaton: 20.0\n"
    );

    const std::string output = logs.output();
    CHECK(output.find("terrain.height_variaton") != std::string::npos);
    CHECK(output.find("world-settings.yaml") != std::string::npos);
}

TEST_CASE(RenderConfig_ReportsUnknownKeyAndSource) {
    LogCapture logs;
    ConfigProvider provider;
    provider.addSource(std::make_unique<NamedConfigSource>(
        "render-settings.yaml",
        "render:\n"
        "  shadow:\n"
        "    split_lamda: 0.5\n"
    ));

    provider.loadRenderConfig();

    const std::string output = logs.output();
    CHECK(output.find("render.shadow.split_lamda") != std::string::npos);
    CHECK(output.find("render-settings.yaml") != std::string::npos);
}

TEST_CASE(PersistenceConfig_ReportsUnknownKeyAndSource) {
    LogCapture logs;
    Rigel::Persistence::PersistenceConfig config;

    config.applyYaml(
        "persistence-settings.yaml",
        "persistence:\n"
        "  formt: cr\n"
    );

    const std::string output = logs.output();
    CHECK(output.find("persistence.formt") != std::string::npos);
    CHECK(output.find("persistence-settings.yaml") != std::string::npos);
}

TEST_CASE(Configuration_ValidKeysAreQuiet) {
    LogCapture logs;
    const std::filesystem::path configDirectory =
        std::filesystem::path(__FILE__).parent_path().parent_path() / "assets/config";
    const std::filesystem::path worldPath = configDirectory / "world_generation.yaml";
    const std::filesystem::path renderPath = configDirectory / "render.yaml";
    const std::filesystem::path persistencePath = configDirectory / "persistence.yaml";

    WorldGenConfig worldConfig;
    worldConfig.applyYaml(worldPath.string().c_str(), readFile(worldPath));

    ConfigProvider renderProvider;
    renderProvider.addSource(std::make_unique<NamedConfigSource>(
        renderPath.string(),
        readFile(renderPath)
    ));
    renderProvider.loadRenderConfig();

    Rigel::Persistence::PersistenceConfig persistenceConfig;
    persistenceConfig.applyYaml(
        persistencePath.string().c_str(),
        readFile(persistencePath)
    );

    CHECK(logs.output().empty());
}
