#include "TestFramework.h"

#include "Rigel/Persistence/Storage.h"

#include <chrono>
#include <filesystem>
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
