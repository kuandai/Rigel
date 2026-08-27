#include "TestFramework.h"

#include "DeveloperDiagnostics.h"
#include "Rigel/Persistence/PersistenceConfig.h"

#include <spdlog/logger.h>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>

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

std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path);
    CHECK(input.good());
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()
    );
}

void writeFile(const std::filesystem::path& path) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path);
    CHECK(output.good());
    output << "obsolete\n";
}

} // namespace

TEST_CASE(ConfigurationDiagnostics_ReportOnlyKnownObsoleteGenerationPaths) {
    Rigel::Test::TemporaryDirectory directory(
        "rigel_obsolete_configuration_diagnostics");
    writeFile(directory.path() / "config/world_generation.yaml");
    writeFile(
        directory.path() /
        "config/worlds/42/worldgen_overlays/no_carvers.yaml");
    writeFile(
        directory.path() /
        "assets/config/worldgen_overlays/no_carvers.yaml");
    writeFile(directory.path() / "config/worlds/43/world_generation.yaml");
    writeFile(directory.path() / "unrelated/no_carvers.yaml");

    LogCapture logs;
    Rigel::detail::warnAboutObsoleteGenerationConfiguration(
        directory.path(), 42);

    const std::string output = logs.output();
    CHECK(output.find("config/world_generation.yaml") != std::string::npos);
    CHECK(output.find(
              "config/worlds/42/worldgen_overlays/no_carvers.yaml") !=
          std::string::npos);
    CHECK(output.find(
              "assets/config/worldgen_overlays/no_carvers.yaml") !=
          std::string::npos);
    CHECK(output.find("assets/manifest.yaml") != std::string::npos);
    CHECK(output.find("config/worlds/43") == std::string::npos);
    CHECK(output.find("unrelated/no_carvers.yaml") == std::string::npos);
}

TEST_CASE(ConfigurationDiagnostics_DoNotScanForUnknownOverlayPaths) {
    Rigel::Test::TemporaryDirectory directory(
        "rigel_bounded_configuration_diagnostics");
    writeFile(directory.path() / "custom/nested/no_carvers.yaml");

    LogCapture logs;
    Rigel::detail::warnAboutObsoleteGenerationConfiguration(
        directory.path(), 0);

    CHECK(logs.output().empty());
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
    const std::filesystem::path persistencePath = configDirectory / "persistence.yaml";

    Rigel::Persistence::PersistenceConfig persistenceConfig;
    persistenceConfig.applyYaml(
        persistencePath.string().c_str(),
        readFile(persistencePath)
    );

    CHECK(logs.output().empty());
}
