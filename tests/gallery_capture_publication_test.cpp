#include "GalleryCapturePublication.h"
#include "TestFramework.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <utility>

namespace {

Rigel::Test::NamedGalleryFramebufferCapture makeCapture(
    std::string name,
    uint8_t color) {
    return {
        std::move(name),
        {
            2,
            2,
            {
                color, 0, 0, 255,
                0, color, 0, 255,
                0, 0, color, 255,
                color, color, color, 255,
            },
            {},
        },
    };
}

std::map<std::string, std::string> readCaptureSet(
    const std::filesystem::path& directory) {
    std::map<std::string, std::string> files;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(directory)) {
        CHECK(entry.is_regular_file());
        std::ifstream input(entry.path(), std::ios::binary);
        files.emplace(
            entry.path().filename().string(),
            std::string(
                std::istreambuf_iterator<char>(input),
                std::istreambuf_iterator<char>()));
        CHECK(!input.bad());
    }
    return files;
}

} // namespace

TEST_CASE(GalleryCapturePublication_LateCaptureFailurePreservesPriorCompleteSet) {
    Rigel::Test::TemporaryDirectory directory(
        "rigel_gallery_capture_publication");
    const std::filesystem::path destination =
        directory.path() / "published";
    Rigel::Test::publishGalleryCaptureSet(
        {makeCapture("old_cube", 31), makeCapture("old_slab", 63)},
        destination);
    const auto previous = readCaptureSet(destination);
    CHECK_EQ(previous.size(), static_cast<size_t>(3));
    CHECK(previous.contains("capture-manifest.txt"));
    CHECK_NE(
        previous.at("capture-manifest.txt").find("complete=true\n"),
        std::string::npos);

    auto invalidLateCapture = makeCapture("new_stair", 127);
    invalidLateCapture.framebuffer.rgba.pop_back();
    bool failed = false;
    try {
        Rigel::Test::publishGalleryCaptureSet(
            {makeCapture("new_cube", 95), std::move(invalidLateCapture)},
            destination);
    } catch (const Rigel::Test::TestFailure&) {
        failed = true;
    }
    CHECK(failed);

    CHECK_EQ(readCaptureSet(destination), previous);
    Rigel::Test::publishGalleryCaptureSet(
        {makeCapture("new_cube", 95), makeCapture("new_stair", 127)},
        destination);
    const auto replacement = readCaptureSet(destination);
    CHECK_EQ(replacement.size(), static_cast<size_t>(3));
    CHECK(!replacement.contains("old_cube.ppm"));
    CHECK(!replacement.contains("old_slab.ppm"));
    CHECK(replacement.contains("new_cube.ppm"));
    CHECK(replacement.contains("new_stair.ppm"));

    size_t rootEntries = 0;
    for ([[maybe_unused]] const auto& entry :
         std::filesystem::directory_iterator(directory.path())) {
        ++rootEntries;
    }
    CHECK_EQ(rootEntries, static_cast<size_t>(1));
}
