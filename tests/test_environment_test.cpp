#include "TestFramework.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <vector>

TEST_CASE(TestRuntime_UsesDisposableWorkingDirectory) {
    const auto sourceDirectory = std::filesystem::weakly_canonical(
        std::filesystem::path(RIGEL_TEST_SOURCE_DIRECTORY));
    const auto workingDirectory = std::filesystem::weakly_canonical(
        std::filesystem::current_path());
    const auto relativePath = workingDirectory.lexically_relative(sourceDirectory);

    CHECK(relativePath.empty() || *relativePath.begin() == "..");
}

TEST_CASE(TemporaryDirectory_RepeatedAllocationsDoNotCollideAndCleanAfterExceptions) {
    struct ExpectedException {};

    std::set<std::filesystem::path> paths;
    try {
        std::vector<std::unique_ptr<Rigel::Test::TemporaryDirectory>> directories;
        for (int i = 0; i < 32; ++i) {
            auto directory = std::make_unique<Rigel::Test::TemporaryDirectory>(
                "rigel_fixture_test");
            CHECK(paths.insert(directory->path()).second);
            CHECK(std::filesystem::is_directory(directory->path()));
            directories.push_back(std::move(directory));
        }

        std::ofstream(directories.front()->path() / "data.bin") << "test";
        CHECK(std::filesystem::exists(directories.front()->path() / "data.bin"));
        throw ExpectedException{};
    } catch (const ExpectedException&) {
    }

    CHECK_EQ(paths.size(), static_cast<size_t>(32));
    for (const auto& path : paths) {
        CHECK(!std::filesystem::exists(path));
    }
}
