#include "TestFramework.h"

#include "Rigel/Persistence/InMemoryStorage.h"

#include <atomic>
#include <chrono>
#include <future>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace Rigel::Persistence;

void writeText(
    InMemoryStorageBackend& storage,
    const std::string& path,
    const std::string& contents) {
    auto session = storage.openWrite(path);
    session->writer().writeBytes(
        reinterpret_cast<const uint8_t*>(contents.data()), contents.size());
    session->commit();
}

std::string readText(
    InMemoryStorageBackend& storage,
    const std::string& path) {
    auto reader = storage.openRead(path);
    const std::vector<uint8_t> bytes = reader->readAt(0, reader->size());
    return std::string(bytes.begin(), bytes.end());
}

} // namespace

TEST_CASE(InMemoryStorage_AtomicWritesPublishWholeSnapshotsOnly) {
    InMemoryStorageBackend storage;
    writeText(storage, "virtual/world/value.bin", "before");

    auto staged = storage.openWrite("virtual/world/value.bin");
    const std::string replacement = "after-publication";
    staged->writer().writeBytes(
        reinterpret_cast<const uint8_t*>(replacement.data()),
        replacement.size());
    auto oldReader = storage.openRead("virtual/world/value.bin");

    CHECK_EQ(readText(storage, "virtual/world/value.bin"), "before");
    staged->commit();
    CHECK_EQ(
        readText(storage, "virtual/world/value.bin"), replacement);
    const std::vector<uint8_t> oldBytes =
        oldReader->readAt(0, oldReader->size());
    CHECK_EQ(
        std::string(oldBytes.begin(), oldBytes.end()),
        std::string("before"));

    auto abandoned = storage.openWrite("virtual/world/value.bin");
    abandoned->writer().writeU8(0xff);
    abandoned->abort();
    CHECK_EQ(
        readText(storage, "virtual/world/value.bin"), replacement);
}

TEST_CASE(InMemoryStorage_PublishesDirectoryTreesWithoutReplacement) {
    InMemoryStorageBackend storage;
    CHECK(storage.createDirectoryExclusive("virtual/world.staging.0"));
    CHECK(!storage.createDirectoryExclusive("virtual/world.staging.0"));
    CHECK(storage.createFileExclusive(
        "virtual/world.staging.0/identity", "prepared"));
    CHECK(!storage.createFileExclusive(
        "virtual/world.staging.0/identity", "replacement"));
    writeText(
        storage,
        "virtual/world.staging.0/zones/default/region.mem",
        "chunk bytes");

    storage.publishDirectory(
        "virtual/world.staging.0", "virtual/world");

    CHECK_EQ(
        storage.entryKind("virtual/world"), StorageEntryKind::Directory);
    CHECK_EQ(
        readText(storage, "virtual/world/identity"), std::string("prepared"));
    CHECK_EQ(
        readText(storage, "virtual/world/zones/default/region.mem"),
        std::string("chunk bytes"));
    CHECK_EQ(
        storage.entryKind("virtual/world.staging.0"),
        StorageEntryKind::Missing);

    CHECK(storage.createDirectoryExclusive("virtual/other.staging"));
    CHECK_THROWS(storage.publishDirectory(
        "virtual/other.staging", "virtual/world"));
    CHECK_EQ(
        storage.entryKind("virtual/other.staging"),
        StorageEntryKind::Directory);
}

TEST_CASE(InMemoryStorage_ConcurrentFilesRemainIndependent) {
    InMemoryStorageBackend storage;
    constexpr size_t WriterCount = 8;
    std::vector<std::thread> writers;
    for (size_t index = 0; index < WriterCount; ++index) {
        writers.emplace_back([&, index] {
            writeText(
                storage,
                "parallel/file_" + std::to_string(index),
                std::string(index + 1, static_cast<char>('a' + index)));
        });
    }
    for (std::thread& writer : writers) {
        writer.join();
    }
    for (size_t index = 0; index < WriterCount; ++index) {
        CHECK_EQ(
            readText(storage, "parallel/file_" + std::to_string(index)),
            std::string(index + 1, static_cast<char>('a' + index)));
    }
}

TEST_CASE(InMemoryStorage_WorldBootstrapLocksSerializeByVirtualRoot) {
    InMemoryStorageBackend storage;
    auto first = storage.lockWorldGenerationBootstrap("virtual/world");
    std::promise<void> attempting;
    std::future<void> attemptingFuture = attempting.get_future();
    std::promise<void> acquired;
    std::future<void> acquiredFuture = acquired.get_future();
    std::thread contender([&] {
        attempting.set_value();
        auto second =
            storage.lockWorldGenerationBootstrap("virtual/world");
        acquired.set_value();
    });

    attemptingFuture.wait();
    CHECK_EQ(
        acquiredFuture.wait_for(std::chrono::milliseconds(20)),
        std::future_status::timeout);
    first.reset();
    CHECK_EQ(
        acquiredFuture.wait_for(std::chrono::seconds(1)),
        std::future_status::ready);
    contender.join();
}
