#include "GalleryCapturePublication.h"

#include "TestFramework.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <fstream>
#include <set>
#include <system_error>
#include <utility>

#if defined(__linux__)
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace Rigel::Test {
namespace {

constexpr std::string_view CaptureManifestName = "capture-manifest.txt";

[[noreturn]] void fail(
    std::string_view operation,
    const std::filesystem::path& path,
    const std::error_code& error = {}) {
    std::string message(operation);
    message += ": ";
    message += path.string();
    if (error) {
        message += ": ";
        message += error.message();
    }
    throw TestFailure(std::move(message));
}

bool isCaptureNameCharacter(char character) {
    return (character >= 'a' && character <= 'z') ||
        (character >= 'A' && character <= 'Z') ||
        (character >= '0' && character <= '9') ||
        character == '_' || character == '-';
}

void validateCaptureNames(
    const std::vector<NamedGalleryFramebufferCapture>& captures) {
    if (captures.empty()) {
        throw TestFailure("Gallery capture set must not be empty");
    }

    std::set<std::string> names;
    for (const NamedGalleryFramebufferCapture& capture : captures) {
        if (capture.name.empty() ||
            !std::all_of(
                capture.name.begin(), capture.name.end(),
                isCaptureNameCharacter)) {
            throw TestFailure(
                "Gallery capture name contains unsupported characters: " +
                capture.name);
        }
        if (!names.insert(capture.name).second) {
            throw TestFailure(
                "Gallery capture name is duplicated: " + capture.name);
        }
    }
}

std::filesystem::path reserveSiblingDirectory(
    const std::filesystem::path& destination,
    std::string_view purpose) {
    static std::atomic<uint64_t> sequence{0};
    const uint64_t uniqueValue = static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count()) +
        sequence.fetch_add(1, std::memory_order_relaxed);

    for (uint64_t attempt = 0; attempt < 100; ++attempt) {
        const std::filesystem::path candidate =
            destination.parent_path() /
            (destination.filename().string() + "." + std::string(purpose) +
             "." + std::to_string(uniqueValue) + "." +
             std::to_string(attempt));
        std::error_code error;
        if (std::filesystem::create_directory(candidate, error)) {
            return candidate;
        }
        if (error && error != std::errc::file_exists) {
            fail("Failed to reserve gallery capture working directory",
                 candidate, error);
        }
    }

    fail("Failed to reserve a unique gallery capture working directory",
         destination);
}

void writeFramebufferCapture(
    const GalleryFramebufferCapture& capture,
    const std::filesystem::path& destination) {
    if (capture.width <= 0 || capture.height <= 0) {
        throw TestFailure(
            "Gallery framebuffer capture has invalid dimensions: " +
            destination.string());
    }
    const size_t expectedBytes = static_cast<size_t>(capture.width) *
        static_cast<size_t>(capture.height) * 4;
    if (capture.rgba.size() != expectedBytes) {
        throw TestFailure(
            "Gallery framebuffer capture has an invalid RGBA payload: " +
            destination.string());
    }

    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    output << "P6\n" << capture.width << ' ' << capture.height << "\n255\n";
    for (int y = capture.height - 1; y >= 0; --y) {
        for (int x = 0; x < capture.width; ++x) {
            const size_t offset = static_cast<size_t>(
                x + y * capture.width) * 4;
            output.write(
                reinterpret_cast<const char*>(capture.rgba.data() + offset),
                3);
        }
    }
    if (!output) {
        fail("Failed to write gallery framebuffer capture", destination);
    }
}

void writeCaptureManifest(
    const std::vector<NamedGalleryFramebufferCapture>& captures,
    const std::filesystem::path& destination) {
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    output << "format=rigel-gallery-capture-set-v1\n";
    output << "complete=true\n";
    output << "capture_count=" << captures.size() << '\n';
    for (const NamedGalleryFramebufferCapture& capture : captures) {
        output << "capture=" << capture.name << ".ppm"
               << " width=" << capture.framebuffer.width
               << " height=" << capture.framebuffer.height << '\n';
    }
    if (!output) {
        fail("Failed to write gallery capture manifest", destination);
    }
}

std::error_code atomicExchangeDirectories(
    const std::filesystem::path& first,
    const std::filesystem::path& second
) {
#if defined(__linux__)
    constexpr unsigned RenameExchange = 2U;
    if (::syscall(
            SYS_renameat2,
            AT_FDCWD,
            first.c_str(),
            AT_FDCWD,
            second.c_str(),
            RenameExchange) == 0) {
        return {};
    }
    return {errno, std::generic_category()};
#else
    (void)first;
    (void)second;
    return std::make_error_code(std::errc::operation_not_supported);
#endif
}

void removeWorkingDirectory(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::remove_all(path, error);
    if (error) {
        fail("Failed to remove gallery capture working directory", path,
             error);
    }
}

} // namespace

void publishGalleryCaptureSet(
    const std::vector<NamedGalleryFramebufferCapture>& captures,
    const std::filesystem::path& destination,
    const GalleryCaptureDirectoryExchange& directoryExchange) {
    validateCaptureNames(captures);
    if (destination.empty() || destination.filename().empty()) {
        throw TestFailure(
            "Gallery capture destination must name a directory");
    }

    std::error_code error;
    std::filesystem::create_directories(destination.parent_path(), error);
    if (error) {
        fail("Failed to create gallery capture parent directory",
             destination.parent_path(), error);
    }

    const std::filesystem::path staging =
        reserveSiblingDirectory(destination, "staging");
    try {
        for (const NamedGalleryFramebufferCapture& capture : captures) {
            writeFramebufferCapture(
                capture.framebuffer,
                staging / (capture.name + ".ppm"));
        }
        writeCaptureManifest(captures, staging / CaptureManifestName);

        error.clear();
        const bool replacing = std::filesystem::exists(destination, error);
        if (error) {
            fail("Failed to inspect gallery capture destination",
                 destination, error);
        }
        if (replacing) {
            error = directoryExchange
                ? directoryExchange(staging, destination)
                : atomicExchangeDirectories(staging, destination);
            if (error) {
                fail("Failed to atomically replace the gallery capture set",
                     destination, error);
            }
            removeWorkingDirectory(staging);
        } else {
            std::filesystem::rename(staging, destination, error);
            if (error) {
                fail("Failed to publish the complete gallery capture set",
                     destination, error);
            }
        }
    } catch (...) {
        removeWorkingDirectory(staging);
        throw;
    }
}

} // namespace Rigel::Test
