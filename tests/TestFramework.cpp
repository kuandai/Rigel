#include "TestFramework.h"

#include <atomic>
#include <chrono>
#include <system_error>

namespace Rigel::Test {

namespace {

bool isWithin(const std::filesystem::path& path,
              const std::filesystem::path& directory) {
    auto pathPart = path.begin();
    for (auto directoryPart = directory.begin();
         directoryPart != directory.end();
         ++directoryPart, ++pathPart) {
        if (pathPart == path.end() || *pathPart != *directoryPart) {
            return false;
        }
    }
    return true;
}

} // namespace

std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

void registerTest(const char* name, void (*fn)()) {
    registry().push_back(TestCase{name, fn});
}

TemporaryDirectory::TemporaryDirectory(std::string_view prefix) {
    static std::atomic<uint64_t> sequence{0};

    const auto sourceDirectory = std::filesystem::weakly_canonical(
        std::filesystem::path(RIGEL_TEST_SOURCE_DIRECTORY));
    const auto temporaryRoot = std::filesystem::weakly_canonical(
        std::filesystem::temp_directory_path());
    const auto uniqueValue =
        std::chrono::steady_clock::now().time_since_epoch().count() +
        sequence.fetch_add(1, std::memory_order_relaxed);

    for (uint64_t attempt = 0; attempt < 100; ++attempt) {
        auto candidate = temporaryRoot /
            (std::string(prefix) + "_" + std::to_string(uniqueValue) + "_" +
             std::to_string(attempt));
        candidate = std::filesystem::weakly_canonical(candidate);
        if (isWithin(candidate, sourceDirectory)) {
            throw std::runtime_error(
                "Temporary test directory would be inside the source tree: " +
                candidate.string());
        }

        std::error_code error;
        if (std::filesystem::create_directory(candidate, error)) {
            m_path = std::move(candidate);
            return;
        }
        if (error && error != std::errc::file_exists) {
            throw std::filesystem::filesystem_error(
                "Failed to create temporary test directory",
                candidate,
                error);
        }
    }

    throw std::runtime_error("Failed to allocate a unique temporary test directory");
}

TemporaryDirectory::~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(m_path, error);
}

} // namespace Rigel::Test
