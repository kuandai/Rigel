#include "TestFramework.h"

#include "Rigel/Persistence/Providers.h"
#include "Rigel/Persistence/Storage.h"
#include "Rigel/Voxel/WorldSet.h"

#include <stdexcept>

using namespace Rigel;

namespace {

class DummyStorage final : public Persistence::StorageBackend {
public:
    std::unique_ptr<Persistence::ByteReader> openRead(const std::string&) override {
        throw std::runtime_error("DummyStorage openRead");
    }

    std::unique_ptr<Persistence::AtomicWriteSession> openWrite(const std::string&) override {
        throw std::runtime_error("DummyStorage openWrite");
    }

    bool exists(const std::string&) override {
        return false;
    }

    void forEachEntry(
        const std::string&,
        const Persistence::StorageEntryVisitor&) override {
    }

    std::vector<std::string> list(const std::string&) override {
        return {};
    }

    void mkdirs(const std::string&) override {
    }

    void remove(const std::string&) override {
    }
};

class DummyProvider final : public Persistence::Provider {
public:
    explicit DummyProvider(int value) : value(value) {}
    int value;
};

} // namespace

TEST_CASE(WorldSet_PersistenceContextIncludesProviders) {
    Voxel::WorldSet worldSet;
    auto& world = worldSet.createWorld(Voxel::WorldSet::defaultWorldId());

    auto provider = std::make_shared<DummyProvider>(42);
    world.persistenceProviders().add("dummy", provider);

    worldSet.setPersistenceRoot("root");
    worldSet.setPersistenceStorage(std::make_shared<DummyStorage>());
    worldSet.setPersistencePreferredFormat("memory");

    auto ctx = worldSet.persistenceContext(world.id());
    CHECK_EQ(ctx.rootPath, std::string("root"));
    CHECK_EQ(ctx.preferredFormat, std::string("memory"));
    CHECK(ctx.providers != nullptr);
    CHECK(ctx.providers->findAs<DummyProvider>("dummy") == provider);

    worldSet.setPersistenceActiveFormat(world.id(), "cr");
    ctx = worldSet.persistenceContext(world.id());
    CHECK_EQ(ctx.preferredFormat, std::string("cr"));
}

TEST_CASE(StorageBackend_exclusive_directory_creation_defaults_to_rejection) {
    DummyStorage storage;
    CHECK_THROWS(storage.createDirectoryExclusive("staging"));
    CHECK_THROWS(storage.createFileExclusive("cleanup", "ownership"));
    CHECK_THROWS(storage.lockWorldGenerationBootstrap("world"));
}
