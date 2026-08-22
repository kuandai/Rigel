#pragma once

#include <cstddef>
#include <memory>
#include <sstream>
#include <string>
#include <utility>

#include <spdlog/logger.h>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

namespace Rigel::Test {

class LogCapture final {
public:
    explicit LogCapture(std::string loggerName)
        : m_previous(spdlog::default_logger())
        , m_logger(std::make_shared<spdlog::logger>(
              std::move(loggerName),
              std::make_shared<spdlog::sinks::ostream_sink_mt>(m_output))) {
        m_logger->set_level(spdlog::level::info);
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

inline size_t countOccurrences(
    const std::string& text,
    const std::string& needle) {
    size_t count = 0;
    size_t position = 0;
    while ((position = text.find(needle, position)) != std::string::npos) {
        ++count;
        position += needle.size();
    }
    return count;
}

} // namespace Rigel::Test
