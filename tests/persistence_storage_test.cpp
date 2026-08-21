#include "TestFramework.h"

#include "Rigel/Persistence/Storage.h"
#include "../src/persistence/AtomicFileCommit.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace Rigel::Persistence;

namespace {

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
        m_path = std::filesystem::temp_directory_path() /
            ("rigel_persistence_storage_" + std::to_string(suffix));
        std::filesystem::create_directories(m_path);
    }

    ~TemporaryDirectory() {
        std::error_code ec;
        std::filesystem::remove_all(m_path, ec);
    }

    const std::filesystem::path& path() const {
        return m_path;
    }

private:
    std::filesystem::path m_path;
};

void writeFile(FilesystemBackend& storage, const std::filesystem::path& path, const std::vector<uint8_t>& data) {
    auto session = storage.openWrite(path.string(), AtomicWriteOptions{});
    session->writer().writeBytes(data.data(), data.size());
    session->commit();
}

std::vector<uint8_t> readFile(FilesystemBackend& storage, const std::filesystem::path& path) {
    auto reader = storage.openRead(path.string());
    return reader->readAt(0, reader->size());
}

void writeRawFile(const std::filesystem::path& path, const std::vector<uint8_t>& data) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    stream.close();
}

} // namespace

TEST_CASE(FilesystemBackend_atomic_replace_writes_complete_replacement) {
    TemporaryDirectory directory;
    FilesystemBackend storage;
    const auto path = directory.path() / "region.bin";
    const std::vector<uint8_t> previous{1, 2, 3, 4};
    const std::vector<uint8_t> replacement{5, 6, 7, 8, 9};

    writeFile(storage, path, previous);
    writeFile(storage, path, replacement);

    CHECK_EQ(readFile(storage, path), replacement);
    CHECK(!std::filesystem::exists(path.string() + ".tmp"));
}

TEST_CASE(FilesystemBackend_failed_atomic_replace_preserves_existing_file) {
    TemporaryDirectory directory;
    FilesystemBackend storage;
    const auto path = directory.path() / "region.bin";
    const auto tempPath = std::filesystem::path(path.string() + ".tmp");
    const std::vector<uint8_t> previous{1, 2, 3, 4};
    const std::vector<uint8_t> replacement{5, 6, 7, 8, 9};

    writeFile(storage, path, previous);
    auto session = storage.openWrite(path.string(), AtomicWriteOptions{});
    session->writer().writeBytes(replacement.data(), replacement.size());

    CHECK(std::filesystem::remove(tempPath));
    CHECK_THROWS(session->commit());

    CHECK_EQ(readFile(storage, path), previous);
    CHECK(!std::filesystem::exists(tempPath));
}

TEST_CASE(FilesystemBackend_failed_atomic_write_removes_temporary_file) {
    TemporaryDirectory directory;
    FilesystemBackend storage;
    const auto path = directory.path() / "occupied";
    const auto tempPath = std::filesystem::path(path.string() + ".tmp");
    const std::vector<uint8_t> replacement{5, 6, 7, 8, 9};

    std::filesystem::create_directories(path);
    writeFile(storage, path / "existing.bin", std::vector<uint8_t>{1, 2, 3, 4});

    auto session = storage.openWrite(path.string(), AtomicWriteOptions{});
    session->writer().writeBytes(replacement.data(), replacement.size());

    CHECK_THROWS(session->commit());

    CHECK(std::filesystem::is_directory(path));
    CHECK(!std::filesystem::exists(tempPath));
}

TEST_CASE(FilesystemBackend_flush_failure_preserves_existing_file) {
    TemporaryDirectory directory;
    FilesystemBackend storage;
    const auto path = directory.path() / "region.bin";
    const auto tempPath = std::filesystem::path(path.string() + ".tmp");
    const std::vector<uint8_t> previous{1, 2, 3, 4};
    const std::vector<uint8_t> replacement{5, 6, 7, 8, 9};
    bool replacementAttempted = false;

    writeFile(storage, path, previous);
    writeRawFile(tempPath, replacement);

    CHECK_THROWS(detail::commitAtomicFile(
        tempPath,
        path,
        []() {
            throw std::runtime_error("flush failed");
        },
        [&replacementAttempted](const std::filesystem::path&,
                                const std::filesystem::path&,
                                std::error_code&) {
            replacementAttempted = true;
        }));

    CHECK(!replacementAttempted);
    CHECK_EQ(readFile(storage, path), previous);
    CHECK(!std::filesystem::exists(tempPath));
}

TEST_CASE(FilesystemBackend_abandoned_atomic_write_removes_temporary_file) {
    TemporaryDirectory directory;
    FilesystemBackend storage;
    const auto path = directory.path() / "region.bin";
    const auto tempPath = std::filesystem::path(path.string() + ".tmp");
    const std::vector<uint8_t> previous{1, 2, 3, 4};
    const std::vector<uint8_t> replacement{5, 6, 7, 8, 9};

    writeFile(storage, path, previous);
    {
        auto session = storage.openWrite(path.string(), AtomicWriteOptions{});
        session->writer().writeBytes(replacement.data(), replacement.size());
    }

    CHECK_EQ(readFile(storage, path), previous);
    CHECK(!std::filesystem::exists(tempPath));
}

TEST_CASE(FilesystemBackend_destroying_committed_session_preserves_new_temporary_file) {
    TemporaryDirectory directory;
    FilesystemBackend storage;
    const auto path = directory.path() / "region.bin";
    const auto tempPath = std::filesystem::path(path.string() + ".tmp");
    const std::vector<uint8_t> previous{1, 2, 3, 4};
    const std::vector<uint8_t> replacement{5, 6, 7, 8, 9};

    auto completedSession = storage.openWrite(path.string(), AtomicWriteOptions{});
    completedSession->writer().writeBytes(previous.data(), previous.size());
    completedSession->commit();

    auto replacementSession = storage.openWrite(path.string(), AtomicWriteOptions{});
    replacementSession->writer().writeBytes(replacement.data(), replacement.size());
    completedSession.reset();

    CHECK(std::filesystem::exists(tempPath));
    replacementSession->commit();
    CHECK_EQ(readFile(storage, path), replacement);
}

TEST_CASE(FilesystemBackend_destroying_failed_session_preserves_new_temporary_file) {
    TemporaryDirectory directory;
    FilesystemBackend storage;
    const auto path = directory.path() / "occupied";
    const auto tempPath = std::filesystem::path(path.string() + ".tmp");
    const std::vector<uint8_t> replacement{5, 6, 7, 8, 9};

    std::filesystem::create_directories(path);
    writeFile(storage, path / "existing.bin", std::vector<uint8_t>{1, 2, 3, 4});

    auto failedSession = storage.openWrite(path.string(), AtomicWriteOptions{});
    failedSession->writer().writeU8(42);
    CHECK_THROWS(failedSession->commit());

    std::filesystem::remove_all(path);
    auto replacementSession = storage.openWrite(path.string(), AtomicWriteOptions{});
    replacementSession->writer().writeBytes(replacement.data(), replacement.size());
    failedSession.reset();

    CHECK(std::filesystem::exists(tempPath));
    replacementSession->commit();
    CHECK_EQ(readFile(storage, path), replacement);
}
