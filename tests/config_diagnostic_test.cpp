#include "TestFramework.h"

#include "DeveloperDiagnostics.h"

#include <spdlog/logger.h>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <fstream>
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

void writeFile(const std::filesystem::path& path) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path);
    CHECK(output.good());
    output << "obsolete\n";
}

} // namespace

TEST_CASE(ConfigurationDiagnostics_ReportOnlyKnownObsoletePaths) {
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
    writeFile(directory.path() / "render.yaml");
    writeFile(directory.path() / "config/streaming.yaml");
    writeFile(directory.path() / "config/worlds/42/persistence.yaml");
    writeFile(directory.path() / "config/worlds/43/persistence.yaml");
    writeFile(directory.path() / "unrelated/no_carvers.yaml");

    LogCapture logs;
    Rigel::detail::warnAboutObsoleteConfiguration(
        directory.path(), 42);

    const std::string output = logs.output();
    CHECK(output.find("config/world_generation.yaml") != std::string::npos);
    CHECK(output.find(
              "config/worlds/42/worldgen_overlays/no_carvers.yaml") !=
          std::string::npos);
    CHECK(output.find(
              "assets/config/worldgen_overlays/no_carvers.yaml") !=
          std::string::npos);
    CHECK(output.find("render.yaml") != std::string::npos);
    CHECK(output.find("config/streaming.yaml") != std::string::npos);
    CHECK(output.find("config/worlds/42/persistence.yaml") !=
          std::string::npos);
    CHECK(output.find("user-preferences.yaml") != std::string::npos);
    CHECK(output.find("installed CR policy") != std::string::npos);
    CHECK(output.find("assets/manifest.yaml") != std::string::npos);
    CHECK(output.find("config/worlds/43") == std::string::npos);
    CHECK(output.find("unrelated/no_carvers.yaml") == std::string::npos);
}

TEST_CASE(ConfigurationDiagnostics_DoNotScanForUnknownOverlayPaths) {
    Rigel::Test::TemporaryDirectory directory(
        "rigel_bounded_configuration_diagnostics");
    writeFile(directory.path() / "custom/nested/no_carvers.yaml");

    LogCapture logs;
    Rigel::detail::warnAboutObsoleteConfiguration(
        directory.path(), 0);

    CHECK(logs.output().empty());
}
