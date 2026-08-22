#include "TestFramework.h"

#include <filesystem>
#include <fstream>

TEST_CASE(TestRuntime_UsesDisposableWorkingDirectory) {
    const auto sourceDirectory = std::filesystem::weakly_canonical(
        std::filesystem::path(RIGEL_TEST_SOURCE_DIRECTORY));
    const auto workingDirectory = std::filesystem::weakly_canonical(
        std::filesystem::current_path());
    const auto relativePath = workingDirectory.lexically_relative(sourceDirectory);

    CHECK(relativePath.empty() || *relativePath.begin() == "..");
}

TEST_CASE(TemporaryDirectory_UsesUniquePathAndCleansAfterExceptions) {
    struct ExpectedException {};

    std::filesystem::path firstPath;
    try {
        Rigel::Test::TemporaryDirectory first("rigel_fixture_test");
        Rigel::Test::TemporaryDirectory second("rigel_fixture_test");
        firstPath = first.path();

        CHECK_NE(first.path(), second.path());
        std::ofstream(first.path() / "data.bin") << "test";
        CHECK(std::filesystem::exists(first.path() / "data.bin"));
        throw ExpectedException{};
    } catch (const ExpectedException&) {
    }

    CHECK(!std::filesystem::exists(firstPath));
}
