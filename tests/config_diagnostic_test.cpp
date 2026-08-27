#include "TestFramework.h"

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

} // namespace

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
