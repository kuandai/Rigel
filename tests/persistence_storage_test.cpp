#include "TestFramework.h"

#include "Rigel/Persistence/Storage.h"
#include "../src/persistence/AtomicFileCommit.h"

#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using namespace Rigel::Persistence;

namespace {

void writeFile(FilesystemBackend& storage, const std::filesystem::path& path, const std::vector<uint8_t>& data) {
    auto session = storage.openWrite(path.string());
    session->writer().writeBytes(data.data(), data.size());
    session->commit();
}

std::vector<uint8_t> readFile(FilesystemBackend& storage, const std::filesystem::path& path) {
    auto reader = storage.openRead(path.string());
    return reader->readAt(0, reader->size());
}

std::vector<std::filesystem::path> stagingFiles(const std::filesystem::path& path) {
    std::vector<std::filesystem::path> result;
    const auto prefix = path.filename().string() + ".tmp";
    for (const auto& entry : std::filesystem::directory_iterator(path.parent_path())) {
        if (entry.path().filename().string().starts_with(prefix)) {
            result.push_back(entry.path());
        }
    }
    return result;
}

std::filesystem::path onlyStagingFile(const std::filesystem::path& path) {
    auto files = stagingFiles(path);
    if (files.size() != 1) {
        throw Rigel::Test::TestFailure("Expected exactly one staging file");
    }
    return files.front();
}

void writeRawFile(const std::filesystem::path& path, const std::vector<uint8_t>& data) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    stream.close();
}

void checkRandomReadFailure(ByteReader& reader,
                            size_t offset,
                            size_t length,
                            const std::string& diagnostic) {
    const size_t position = reader.tell();
    try {
        static_cast<void>(reader.readAt(offset, length));
    } catch (const std::runtime_error& error) {
        CHECK_EQ(std::string(error.what()), diagnostic);
        CHECK_EQ(reader.tell(), position);
        return;
    } catch (const std::exception& error) {
        throw Rigel::Test::TestFailure(
            std::string("Unexpected random read exception: ") + error.what());
    }
    throw Rigel::Test::TestFailure("Expected random read to fail");
}

} // namespace

TEST_CASE(FilesystemByteReader_bounds_random_access_reads) {
    Rigel::Test::TemporaryDirectory directory("rigel_persistence_storage");
    FilesystemBackend storage;
    const auto path = directory.path() / "tiny.bin";
    const std::vector<uint8_t> fixture{10, 20, 30, 40};
    writeFile(storage, path, fixture);

    auto reader = storage.openRead(path.string());
    CHECK_EQ(reader->readAt(0, fixture.size()), fixture);
    CHECK_EQ(reader->readAt(fixture.size(), 0), std::vector<uint8_t>{});

    reader->seek(1);
    const std::string diagnostic =
        "Unexpected end of file while reading: " + path.string();
    checkRandomReadFailure(*reader, fixture.size() - 1, 2, diagnostic);
    checkRandomReadFailure(*reader, fixture.size() + 1, 0, diagnostic);
    checkRandomReadFailure(
        *reader, std::numeric_limits<size_t>::max(), 0, diagnostic);
    checkRandomReadFailure(
        *reader, 1, std::numeric_limits<size_t>::max(), diagnostic);
    checkRandomReadFailure(
        *reader,
        std::numeric_limits<size_t>::max(),
        std::numeric_limits<size_t>::max(),
        diagnostic);
    CHECK_EQ(reader->readU8(), static_cast<uint8_t>(20));
}

TEST_CASE(FilesystemByteReader_restores_position_after_physical_read_failure) {
#ifdef _WIN32
    throw Rigel::Test::TestSkip(
        "Windows does not permit truncating an open file");
#else
    Rigel::Test::TemporaryDirectory directory("rigel_persistence_storage");
    FilesystemBackend storage;
    const auto path = directory.path() / "tiny.bin";
    const std::vector<uint8_t> fixture{10, 20, 30, 40};
    writeFile(storage, path, fixture);

    auto reader = storage.openRead(path.string());
    reader->seek(1);
    std::filesystem::resize_file(path, 2);

    try {
        static_cast<void>(reader->readAt(2, 2));
    } catch (const StorageReadError& error) {
        CHECK_EQ(
            std::string(error.what()),
            "Failed to read bytes from: " + path.string());
        CHECK_EQ(reader->tell(), static_cast<size_t>(1));
        CHECK_EQ(reader->readU8(), static_cast<uint8_t>(20));
        return;
    } catch (const std::exception& error) {
        throw Rigel::Test::TestFailure(
            std::string("Unexpected random read exception: ") + error.what());
    }
    throw Rigel::Test::TestFailure("Expected random read to fail");
#endif
}

TEST_CASE(FilesystemBackend_atomic_replace_writes_complete_replacement) {
    Rigel::Test::TemporaryDirectory directory("rigel_persistence_storage");
    FilesystemBackend storage;
    const auto path = directory.path() / "region.bin";
    const std::vector<uint8_t> previous{1, 2, 3, 4};
    const std::vector<uint8_t> replacement{5, 6, 7, 8, 9};

    writeFile(storage, path, previous);
    writeFile(storage, path, replacement);

    CHECK_EQ(readFile(storage, path), replacement);
    CHECK(stagingFiles(path).empty());
}

TEST_CASE(AtomicFileCommit_synchronizes_before_and_after_publication) {
    Rigel::Test::TemporaryDirectory directory("rigel_persistence_storage");
    FilesystemBackend storage;
    const auto path = directory.path() / "region.bin";
    const auto tempPath =
        std::filesystem::path(path.string() + ".tmp.owned");
    const std::vector<uint8_t> previous{1, 2, 3, 4};
    const std::vector<uint8_t> replacement{5, 6, 7, 8, 9};
    std::vector<std::string> operations;

    writeRawFile(path, previous);
    writeRawFile(tempPath, replacement);

    detail::commitAtomicFile(
        tempPath,
        path,
        [&operations]() {
            operations.push_back("close");
        },
        [&](const std::filesystem::path& synchronizedPath) {
            CHECK_EQ(synchronizedPath, tempPath);
            operations.push_back("sync file");
        },
        [&](const std::filesystem::path& replacementPath,
            const std::filesystem::path& destinationPath,
            std::error_code& error) {
            CHECK_EQ(replacementPath, tempPath);
            CHECK_EQ(destinationPath, path);
            CHECK_EQ(readFile(storage, path), previous);
            operations.push_back("replace");
            error.clear();
        },
        [&](const std::filesystem::path& directoryPath) {
            CHECK_EQ(directoryPath, path.parent_path());
            operations.push_back("sync directory");
        });

    CHECK_EQ(
        operations,
        (std::vector<std::string>{
            "close", "sync file", "replace", "sync directory"}));
}

TEST_CASE(AtomicFileCommit_file_sync_failure_preserves_destination) {
    Rigel::Test::TemporaryDirectory directory("rigel_persistence_storage");
    FilesystemBackend storage;
    const auto path = directory.path() / "region.bin";
    const auto tempPath =
        std::filesystem::path(path.string() + ".tmp.owned");
    const auto unownedTempPath =
        std::filesystem::path(path.string() + ".tmp.unowned");
    const std::vector<uint8_t> previous{1, 2, 3, 4};
    const std::vector<uint8_t> replacement{5, 6, 7, 8, 9};
    const std::vector<uint8_t> unowned{9, 8, 7};
    bool replacementAttempted = false;
    bool directorySyncAttempted = false;

    writeRawFile(path, previous);
    writeRawFile(tempPath, replacement);
    writeRawFile(unownedTempPath, unowned);

    CHECK_THROWS(detail::commitAtomicFile(
        tempPath,
        path,
        []() {},
        [](const std::filesystem::path&) {
            throw std::runtime_error("file synchronization failed");
        },
        [&](const std::filesystem::path&,
            const std::filesystem::path&,
            std::error_code&) {
            replacementAttempted = true;
        },
        [&](const std::filesystem::path&) {
            directorySyncAttempted = true;
        }));

    CHECK(!replacementAttempted);
    CHECK(!directorySyncAttempted);
    CHECK_EQ(readFile(storage, path), previous);
    CHECK(!std::filesystem::exists(tempPath));
    CHECK_EQ(readFile(storage, unownedTempPath), unowned);
}

TEST_CASE(AtomicFileCommit_directory_sync_failure_keeps_published_paths) {
    Rigel::Test::TemporaryDirectory directory("rigel_persistence_storage");
    FilesystemBackend storage;
    const auto path = directory.path() / "region.bin";
    const auto tempPath =
        std::filesystem::path(path.string() + ".tmp.owned");
    const std::vector<uint8_t> replacement{5, 6, 7, 8, 9};
    const std::vector<uint8_t> laterStagingData{9, 8, 7};

    writeRawFile(tempPath, replacement);

    CHECK_THROWS(detail::commitAtomicFile(
        tempPath,
        path,
        []() {},
        [](const std::filesystem::path&) {},
        [](const std::filesystem::path& replacementPath,
           const std::filesystem::path& destinationPath,
           std::error_code& error) {
            std::filesystem::rename(
                replacementPath, destinationPath, error);
        },
        [&](const std::filesystem::path&) {
            writeRawFile(tempPath, laterStagingData);
            throw std::runtime_error("directory synchronization failed");
        }));

    CHECK_EQ(readFile(storage, path), replacement);
    CHECK_EQ(readFile(storage, tempPath), laterStagingData);
}

TEST_CASE(AtomicFileRemoval_synchronizes_directory_after_removal) {
    Rigel::Test::TemporaryDirectory directory("rigel_persistence_storage");
    const auto path = directory.path() / "region.bin";
    std::vector<std::string> operations;
    writeRawFile(path, std::vector<uint8_t>{1, 2, 3, 4});

    detail::removeFileDurably(
        path,
        [&](const std::filesystem::path& removalPath) {
            CHECK_EQ(removalPath, path);
            operations.push_back("remove");
            return std::filesystem::remove(removalPath);
        },
        [&](const std::filesystem::path& directoryPath) {
            CHECK_EQ(directoryPath, path.parent_path());
            CHECK(!std::filesystem::exists(path));
            operations.push_back("sync directory");
        });

    CHECK_EQ(
        operations,
        (std::vector<std::string>{"remove", "sync directory"}));
}

TEST_CASE(AtomicFileRemoval_synchronizes_directory_when_path_is_absent) {
    Rigel::Test::TemporaryDirectory directory("rigel_persistence_storage");
    const auto path = directory.path() / "missing-region.bin";
    std::vector<std::string> operations;

    detail::removeFileDurably(
        path,
        [&](const std::filesystem::path& removalPath) {
            CHECK_EQ(removalPath, path);
            operations.push_back("remove");
            return std::filesystem::remove(removalPath);
        },
        [&](const std::filesystem::path& directoryPath) {
            CHECK_EQ(directoryPath, path.parent_path());
            operations.push_back("sync directory");
        });

    CHECK_EQ(
        operations,
        (std::vector<std::string>{"remove", "sync directory"}));
}

TEST_CASE(AtomicFileRemoval_reports_directory_sync_failure) {
    Rigel::Test::TemporaryDirectory directory("rigel_persistence_storage");
    const auto path = directory.path() / "region.bin";
    writeRawFile(path, std::vector<uint8_t>{1, 2, 3, 4});

    CHECK_THROWS(detail::removeFileDurably(
        path,
        [](const std::filesystem::path& removalPath) {
            return std::filesystem::remove(removalPath);
        },
        [](const std::filesystem::path&) {
            throw std::runtime_error("directory synchronization failed");
        }));

    CHECK(!std::filesystem::exists(path));
}

TEST_CASE(FilesystemBackend_failed_atomic_replace_preserves_existing_file) {
    Rigel::Test::TemporaryDirectory directory("rigel_persistence_storage");
    FilesystemBackend storage;
    const auto path = directory.path() / "region.bin";
    const auto tempPath = std::filesystem::path(path.string() + ".tmp.owned");
    const auto unownedTempPath = std::filesystem::path(path.string() + ".tmp");
    const std::vector<uint8_t> previous{1, 2, 3, 4};
    const std::vector<uint8_t> replacement{5, 6, 7, 8, 9};
    const std::vector<uint8_t> unownedData{9, 8, 7};

    writeFile(storage, path, previous);
    writeRawFile(tempPath, replacement);
    writeRawFile(unownedTempPath, unownedData);

    CHECK_THROWS(detail::commitAtomicFile(
        tempPath,
        path,
        []() {},
        [](const std::filesystem::path&) {},
        [](const std::filesystem::path&,
           const std::filesystem::path&,
           std::error_code& error) {
            error = std::make_error_code(std::errc::permission_denied);
        },
        [](const std::filesystem::path&) {}));

    CHECK_EQ(readFile(storage, path), previous);
    CHECK(!std::filesystem::exists(tempPath));
    CHECK_EQ(readFile(storage, unownedTempPath), unownedData);
}

TEST_CASE(FilesystemBackend_missing_atomic_staging_file_preserves_existing_file) {
#ifdef _WIN32
    throw Rigel::Test::TestSkip(
        "Windows does not permit removing an open staging file");
#else
    Rigel::Test::TemporaryDirectory directory("rigel_persistence_storage");
    FilesystemBackend storage;
    const auto path = directory.path() / "region.bin";
    const auto unownedTempPath = std::filesystem::path(path.string() + ".tmp");
    const std::vector<uint8_t> previous{1, 2, 3, 4};
    const std::vector<uint8_t> replacement{5, 6, 7, 8, 9};
    const std::vector<uint8_t> unownedData{9, 8, 7};

    writeFile(storage, path, previous);
    auto session = storage.openWrite(path.string());
    session->writer().writeBytes(replacement.data(), replacement.size());
    const auto ownedTempPath = onlyStagingFile(path);
    writeRawFile(unownedTempPath, unownedData);

    CHECK(std::filesystem::remove(ownedTempPath));
    CHECK_THROWS(session->commit());

    CHECK_EQ(readFile(storage, path), previous);
    CHECK(!std::filesystem::exists(ownedTempPath));
    CHECK_EQ(readFile(storage, unownedTempPath), unownedData);
#endif
}

TEST_CASE(FilesystemBackend_failed_atomic_write_removes_temporary_file) {
    Rigel::Test::TemporaryDirectory directory("rigel_persistence_storage");
    FilesystemBackend storage;
    const auto path = directory.path() / "occupied";
    const std::vector<uint8_t> replacement{5, 6, 7, 8, 9};

    std::filesystem::create_directories(path);
    writeFile(storage, path / "existing.bin", std::vector<uint8_t>{1, 2, 3, 4});

    auto session = storage.openWrite(path.string());
    session->writer().writeBytes(replacement.data(), replacement.size());
    const auto tempPath = onlyStagingFile(path);

    CHECK_THROWS(session->commit());

    CHECK(std::filesystem::is_directory(path));
    CHECK(!std::filesystem::exists(tempPath));
}

TEST_CASE(FilesystemBackend_flush_failure_preserves_existing_file) {
    Rigel::Test::TemporaryDirectory directory("rigel_persistence_storage");
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
        [](const std::filesystem::path&) {},
        [&replacementAttempted](const std::filesystem::path&,
                                const std::filesystem::path&,
                                std::error_code&) {
            replacementAttempted = true;
        },
        [](const std::filesystem::path&) {}));

    CHECK(!replacementAttempted);
    CHECK_EQ(readFile(storage, path), previous);
    CHECK(!std::filesystem::exists(tempPath));
}

TEST_CASE(FilesystemBackend_abandoned_atomic_write_removes_temporary_file) {
    Rigel::Test::TemporaryDirectory directory("rigel_persistence_storage");
    FilesystemBackend storage;
    const auto path = directory.path() / "region.bin";
    const std::vector<uint8_t> previous{1, 2, 3, 4};
    const std::vector<uint8_t> replacement{5, 6, 7, 8, 9};

    writeFile(storage, path, previous);
    {
        auto session = storage.openWrite(path.string());
        session->writer().writeBytes(replacement.data(), replacement.size());
    }

    CHECK_EQ(readFile(storage, path), previous);
    CHECK(stagingFiles(path).empty());
}

TEST_CASE(FilesystemBackend_destroying_committed_session_preserves_new_temporary_file) {
    Rigel::Test::TemporaryDirectory directory("rigel_persistence_storage");
    FilesystemBackend storage;
    const auto path = directory.path() / "region.bin";
    const std::vector<uint8_t> previous{1, 2, 3, 4};
    const std::vector<uint8_t> replacement{5, 6, 7, 8, 9};

    auto completedSession = storage.openWrite(path.string());
    completedSession->writer().writeBytes(previous.data(), previous.size());
    completedSession->commit();

    auto replacementSession = storage.openWrite(path.string());
    replacementSession->writer().writeBytes(replacement.data(), replacement.size());
    const auto tempPath = onlyStagingFile(path);
    completedSession.reset();

    CHECK(std::filesystem::exists(tempPath));
    replacementSession->commit();
    CHECK_EQ(readFile(storage, path), replacement);
}

TEST_CASE(FilesystemBackend_destroying_failed_session_preserves_new_temporary_file) {
    Rigel::Test::TemporaryDirectory directory("rigel_persistence_storage");
    FilesystemBackend storage;
    const auto path = directory.path() / "occupied";
    const std::vector<uint8_t> replacement{5, 6, 7, 8, 9};

    std::filesystem::create_directories(path);
    writeFile(storage, path / "existing.bin", std::vector<uint8_t>{1, 2, 3, 4});

    auto failedSession = storage.openWrite(path.string());
    failedSession->writer().writeU8(42);
    CHECK_THROWS(failedSession->commit());

    std::filesystem::remove_all(path);
    auto replacementSession = storage.openWrite(path.string());
    replacementSession->writer().writeBytes(replacement.data(), replacement.size());
    const auto tempPath = onlyStagingFile(path);
    failedSession.reset();

    CHECK(std::filesystem::exists(tempPath));
    replacementSession->commit();
    CHECK_EQ(readFile(storage, path), replacement);
}

TEST_CASE(FilesystemBackend_overlapping_sessions_do_not_reuse_stale_staging_file) {
    Rigel::Test::TemporaryDirectory directory("rigel_persistence_storage");
    FilesystemBackend storage;
    const auto path = directory.path() / "region.bin";
    const auto stalePath = std::filesystem::path(path.string() + ".tmp");
    const std::vector<uint8_t> staleData{9, 8, 7};

    writeRawFile(stalePath, staleData);
    auto first = storage.openWrite(path.string());
    auto second = storage.openWrite(path.string());

    CHECK_EQ(stagingFiles(path).size(), 3u);
    CHECK_EQ(readFile(storage, stalePath), staleData);

    first->abort();
    second->abort();
    CHECK_EQ(readFile(storage, stalePath), staleData);
}

TEST_CASE(FilesystemBackend_commit_then_continued_write_and_abort_are_isolated) {
    Rigel::Test::TemporaryDirectory directory("rigel_persistence_storage");
    FilesystemBackend storage;
    const auto path = directory.path() / "region.bin";
    const std::vector<uint8_t> committed{1, 2, 3, 4};
    const std::vector<uint8_t> staged{5, 6};
    const std::vector<uint8_t> continued{7, 8};

    auto first = storage.openWrite(path.string());
    first->writer().writeBytes(committed.data(), committed.size());
    auto second = storage.openWrite(path.string());
    second->writer().writeBytes(staged.data(), staged.size());

    first->commit();
    second->writer().writeBytes(continued.data(), continued.size());
    second->abort();

    CHECK_EQ(readFile(storage, path), committed);
    CHECK(stagingFiles(path).empty());
}

TEST_CASE(FilesystemBackend_destroying_stale_session_preserves_active_session) {
    Rigel::Test::TemporaryDirectory directory("rigel_persistence_storage");
    FilesystemBackend storage;
    const auto path = directory.path() / "region.bin";
    const std::vector<uint8_t> replacement{5, 6, 7, 8};

    auto stale = storage.openWrite(path.string());
    stale->writer().writeU8(42);
    auto active = storage.openWrite(path.string());
    active->writer().writeBytes(replacement.data(), replacement.size());

    stale.reset();
    CHECK_EQ(stagingFiles(path).size(), 1u);
    active->commit();

    CHECK_EQ(readFile(storage, path), replacement);
}

TEST_CASE(FilesystemBackend_overlapping_sessions_publish_in_commit_order) {
    Rigel::Test::TemporaryDirectory directory("rigel_persistence_storage");
    FilesystemBackend storage;
    const std::vector<uint8_t> firstData{1, 2, 3};
    const std::vector<uint8_t> secondData{4, 5, 6};

    const auto secondWinsPath = directory.path() / "second-wins.bin";
    auto first = storage.openWrite(secondWinsPath.string());
    first->writer().writeBytes(firstData.data(), firstData.size());
    auto second = storage.openWrite(secondWinsPath.string());
    second->writer().writeBytes(secondData.data(), secondData.size());
    first->commit();
    second->commit();
    CHECK_EQ(readFile(storage, secondWinsPath), secondData);

    const auto firstWinsPath = directory.path() / "first-wins.bin";
    first = storage.openWrite(firstWinsPath.string());
    first->writer().writeBytes(firstData.data(), firstData.size());
    second = storage.openWrite(firstWinsPath.string());
    second->writer().writeBytes(secondData.data(), secondData.size());
    second->commit();
    first->commit();
    CHECK_EQ(readFile(storage, firstWinsPath), firstData);
}

TEST_CASE(FilesystemBackend_failed_session_does_not_disable_overlapping_session) {
    Rigel::Test::TemporaryDirectory directory("rigel_persistence_storage");
    FilesystemBackend storage;
    const auto path = directory.path() / "region.bin";
    const std::vector<uint8_t> replacement{5, 6, 7, 8};

    auto failing = storage.openWrite(path.string());
    failing->writer().writeU8(42);
    auto usable = storage.openWrite(path.string());
    usable->writer().writeBytes(replacement.data(), replacement.size());
    std::filesystem::create_directory(path);

    CHECK_THROWS(failing->commit());
    CHECK_EQ(stagingFiles(path).size(), 1u);

    std::filesystem::remove(path);
    usable->commit();
    CHECK_EQ(readFile(storage, path), replacement);
}
