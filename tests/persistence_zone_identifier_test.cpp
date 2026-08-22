#include "TestFramework.h"

#include "Rigel/Persistence/Backends/CR/CRFormat.h"
#include "Rigel/Persistence/Backends/CR/CRPaths.h"
#include "Rigel/Persistence/Backends/Memory/MemoryFormat.h"
#include "Rigel/Persistence/PersistenceService.h"
#include "Rigel/Persistence/Storage.h"

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace Rigel::Persistence;

namespace {

class RejectingStorageBackend final : public StorageBackend {
public:
    std::unique_ptr<ByteReader> openRead(const std::string&) override {
        return reject<std::unique_ptr<ByteReader>>();
    }

    std::unique_ptr<AtomicWriteSession> openWrite(
        const std::string&) override {
        return reject<std::unique_ptr<AtomicWriteSession>>();
    }

    bool exists(const std::string&) override {
        return reject<bool>();
    }

    std::vector<std::string> list(const std::string&) override {
        return reject<std::vector<std::string>>();
    }

    void mkdirs(const std::string&) override {
        reject<void>();
    }

    void remove(const std::string&) override {
        reject<void>();
    }

    size_t callCount() const {
        return m_callCount;
    }

private:
    template <typename Result>
    Result reject() {
        ++m_callCount;
        throw std::runtime_error("unexpected storage operation");
    }

    size_t m_callCount = 0;
};

void registerFormats(FormatRegistry& registry) {
    registry.registerFormat(
        Backends::Memory::descriptor(),
        Backends::Memory::factory(),
        Backends::Memory::probe());
    registry.registerFormat(
        Backends::CR::descriptor(),
        Backends::CR::factory(),
        Backends::CR::probe());
}

PersistenceContext makeContext(const std::string& formatId,
                               const std::string& rootPath,
                               std::shared_ptr<StorageBackend> storage) {
    PersistenceContext context;
    context.preferredFormat = formatId;
    context.rootPath = rootPath;
    context.storage = std::move(storage);
    return context;
}

template <typename Operation>
void checkIdentifierError(Operation&& operation, const std::string& zoneId) {
    try {
        operation();
    } catch (const std::runtime_error& error) {
        const std::string diagnostic = error.what();
        CHECK(diagnostic.find("Persistence configuration error") !=
              std::string::npos);
        CHECK(diagnostic.find("zone identifier") != std::string::npos);
        CHECK(diagnostic.find(zoneId) != std::string::npos);
        return;
    }
    throw Rigel::Test::TestFailure("Expected zone identifier error");
}

const std::vector<std::string>& invalidZoneIds() {
    static const std::vector<std::string> ids{
        "../../escaped",
        "",
        ".",
        "..",
        "rigel:.",
        "rigel:..",
        "rigel/.default",
        "rigel/default",
        "rigel\\default",
        "/absolute",
        "\\absolute",
        "C:/outside",
        "C:\\outside",
        "c:outside",
        "//server/share",
        "\\\\server\\share",
        "rigel:%2e%2e",
        "Rigel:default",
        "rigel:default.",
        "con",
        "rigel:nul.txt",
        "parent:regions",
        "parent:entities",
        "parent:chunks",
        "parent:zone.meta",
        "parent:zoneinfo.json"
    };
    return ids;
}

} // namespace

TEST_CASE(Persistence_ZoneIdentifiersRejectUnsafeCrossPlatformForms) {
    Rigel::Test::TemporaryDirectory directory("rigel_zone_identifier");

    for (const std::string& formatId : {std::string("memory"), std::string("cr")}) {
        FormatRegistry registry;
        registerFormats(registry);
        PersistenceService service(registry);
        auto storage = std::make_shared<FilesystemBackend>();
        const auto formatDirectory = directory.path() / formatId;
        const auto root = formatDirectory / "world";
        auto context = makeContext(formatId, root.string(), storage);

        for (const auto& zoneId : invalidZoneIds()) {
            checkIdentifierError(
                [&]() {
                    service.saveZoneMetadata(
                        ZoneMetadata{zoneId, "Unsafe Zone"}, context);
                },
                zoneId);
        }

        CHECK(!std::filesystem::exists(root));
        CHECK(!std::filesystem::exists(formatDirectory / "escaped"));
    }
}

TEST_CASE(Persistence_ZoneIdentifierValidationPrecedesStorageOperations) {
    for (const std::string& formatId : {std::string("memory"), std::string("cr")}) {
        FormatRegistry registry;
        registerFormats(registry);
        PersistenceService service(registry);
        auto storage = std::make_shared<RejectingStorageBackend>();
        auto context = makeContext(formatId, "configured/root", storage);
        auto format = service.openFormat(context);
        const std::string invalid = "../../outside";

        checkIdentifierError(
            [&]() {
                service.saveZoneMetadata(
                    ZoneMetadata{invalid, "Unsafe Zone"}, context);
            },
            invalid);
        const std::string conflicting = "parent:zone.meta";
        checkIdentifierError(
            [&]() {
                service.saveZoneMetadata(
                    ZoneMetadata{conflicting, "Conflicting Zone"}, context);
            },
            conflicting);

        WorldSnapshot world;
        world.metadata = WorldMetadata{"world", "World"};
        world.zones.push_back(ZoneMetadata{"rigel:valid", "Valid Zone"});
        world.zones.push_back(ZoneMetadata{invalid, "Unsafe Zone"});
        checkIdentifierError([&]() { service.saveWorld(world, context); }, invalid);

        checkIdentifierError(
            [&]() {
                format->zoneMetadataCodec().metadataPath(
                    ZoneKey{invalid}, context);
            },
            invalid);
        checkIdentifierError(
            [&]() {
                format->regionLayout().regionForChunk(
                    invalid, Rigel::Voxel::ChunkCoord{});
            },
            invalid);
        checkIdentifierError(
            [&]() {
                format->regionLayout().storageKeysForChunk(
                    invalid, Rigel::Voxel::ChunkCoord{});
            },
            invalid);
        checkIdentifierError(
            [&]() {
                format->regionLayout().spanForStorageKey(
                    ChunkKey{invalid, 0, 0, 0});
            },
            invalid);
        const RegionKey regionKey{invalid, 0, 0, 0};
        checkIdentifierError(
            [&]() { format->chunkContainer().regionExists(regionKey); },
            invalid);
        checkIdentifierError(
            [&]() {
                format->chunkContainer().saveRegion(
                    ChunkRegionSnapshot{regionKey, {}});
            },
            invalid);
        checkIdentifierError(
            [&]() { format->chunkContainer().loadRegion(regionKey); },
            invalid);
        checkIdentifierError(
            [&]() { format->chunkContainer().listRegions(invalid); },
            invalid);
        ChunkSnapshot invalidChunk;
        invalidChunk.key = ChunkKey{invalid, 0, 0, 0};
        checkIdentifierError(
            [&]() {
                format->chunkContainer().saveRegion(ChunkRegionSnapshot{
                    RegionKey{"rigel:default", 0, 0, 0}, {invalidChunk}});
            },
            invalid);
        const EntityRegionKey entityKey{invalid, 0, 0, 0};
        checkIdentifierError(
            [&]() {
                format->entityContainer().saveRegion(
                    EntityRegionSnapshot{entityKey, {}});
            },
            invalid);
        checkIdentifierError(
            [&]() { format->entityContainer().removeRegion(entityKey); },
            invalid);
        checkIdentifierError(
            [&]() { format->entityContainer().loadRegion(entityKey); },
            invalid);
        checkIdentifierError(
            [&]() { format->entityContainer().listRegions(invalid); },
            invalid);

        CHECK_EQ(storage->callCount(), 0u);
    }
}

TEST_CASE(Persistence_ValidNamespacedZoneIdentifierIsConfinedAndDistinct) {
    Rigel::Test::TemporaryDirectory directory("rigel_namespaced_zone");

    for (const std::string& formatId : {std::string("memory"), std::string("cr")}) {
        FormatRegistry registry;
        registerFormats(registry);
        PersistenceService service(registry);
        auto storage = std::make_shared<FilesystemBackend>();
        const auto root = directory.path() / formatId / "world";
        auto context = makeContext(formatId, root.string(), storage);
        const ZoneMetadata expected{"rigel:default", "Default Zone"};

        service.saveZoneMetadata(expected, context);
        CHECK_EQ(
            service.loadZoneMetadata(ZoneKey{expected.zoneId}, context).zoneId,
            expected.zoneId);

        const auto zoneRoot = root / "zones" / "rigel" / "default";
        CHECK(std::filesystem::exists(zoneRoot));
        CHECK(!std::filesystem::exists(root / "zones" / "rigel:default"));
    }

    PersistenceContext context;
    context.rootPath = (directory.path() / "cr-paths").string();
    CHECK_EQ(
        Backends::CR::CRPaths::zoneRoot("rigel:default", context),
        (directory.path() / "cr-paths" / "zones" / "rigel" / "default").string());
    checkIdentifierError(
        [&]() {
            Backends::CR::CRPaths::zoneRoot("rigel/default", context);
        },
        "rigel/default");
}

TEST_CASE(Persistence_ZoneIdentifiersCannotAliasZoneStorageChildren) {
    Rigel::Test::TemporaryDirectory directory("rigel_zone_child_alias");

    for (const std::string& formatId : {std::string("memory"), std::string("cr")}) {
        FormatRegistry registry;
        registerFormats(registry);
        PersistenceService service(registry);
        auto storage = std::make_shared<FilesystemBackend>();
        const auto root = directory.path() / formatId;
        auto context = makeContext(formatId, root.string(), storage);

        const ZoneMetadata parent{"parent", "Parent Zone"};
        service.saveZoneMetadata(parent, context);

        for (const std::string& conflictingId : {
                 std::string("parent:regions"),
                 std::string("parent:entities"),
                 std::string("parent:chunks"),
                 std::string("parent:zone.meta"),
                 std::string("parent:zoneinfo.json")}) {
            checkIdentifierError(
                [&]() {
                    service.saveZoneMetadata(
                        ZoneMetadata{conflictingId, "Conflicting Zone"},
                        context);
                },
                conflictingId);
        }

        CHECK_EQ(
            service.loadZoneMetadata(ZoneKey{parent.zoneId}, context).zoneId,
            parent.zoneId);
    }
}

#ifndef _WIN32
TEST_CASE(MemoryFormat_LoadsNamespacedZonesFromPreviousStorageLayout) {
    Rigel::Test::TemporaryDirectory directory("rigel_memory_zone_layout");
    FormatRegistry registry;
    registerFormats(registry);
    PersistenceService service(registry);
    auto storage = std::make_shared<FilesystemBackend>();
    auto context = makeContext("memory", directory.path().string(), storage);
    const std::string zoneId = "rigel:default";
    const ZoneMetadata metadata{zoneId, "Default Zone"};

    ChunkSnapshot chunk;
    chunk.key = ChunkKey{zoneId, 1, 2, 3};
    chunk.data.span.chunkX = 1;
    chunk.data.span.chunkY = 2;
    chunk.data.span.chunkZ = 3;
    chunk.data.span.sizeX = 1;
    chunk.data.span.sizeY = 1;
    chunk.data.span.sizeZ = 1;
    chunk.data.blocks.push_back(Rigel::Voxel::BlockState{});
    const ChunkRegionSnapshot region{
        RegionKey{zoneId, 0, 0, 0}, {chunk}};

    EntityPersistedChunk entityChunk;
    entityChunk.coord = Rigel::Voxel::ChunkCoord{1, 2, 3};
    const EntityRegionSnapshot entities{
        EntityRegionKey{zoneId, 0, 0, 0}, {entityChunk}};

    service.saveZoneMetadata(metadata, context);
    service.saveRegion(region, context);
    service.saveEntities(entities, context);
    const auto canonicalRoot =
        directory.path() / "zones" / "rigel" / "default";
    const auto previousRoot =
        directory.path() / "zones" / "rigel:default";
    std::filesystem::rename(canonicalRoot, previousRoot);

    CHECK_EQ(service.loadZoneMetadata(ZoneKey{zoneId}, context), metadata);
    CHECK_EQ(service.loadRegion(region.key, context), region);
    CHECK_EQ(service.loadEntities(entities.key, context), entities);
    const ZoneMetadata updated{zoneId, "Updated Zone"};
    service.saveZoneMetadata(updated, context);
    CHECK_EQ(service.loadZoneMetadata(ZoneKey{zoneId}, context), updated);

    auto format = service.openFormat(context);
    format->entityContainer().removeRegion(entities.key);
    CHECK_EQ(service.loadEntities(entities.key, context),
             (EntityRegionSnapshot{entities.key, {}}));

    CHECK(std::filesystem::exists(previousRoot / "zone.meta"));
    CHECK(std::filesystem::exists(
        previousRoot / "regions" / "region_0_0_0.mem"));
    CHECK(!std::filesystem::exists(
        previousRoot / "entities" / "entityRegion_0_0_0.mem"));
    CHECK(!std::filesystem::exists(canonicalRoot));
}

TEST_CASE(MemoryFormat_RejectsSplitNamespacedZoneStorageLayouts) {
    Rigel::Test::TemporaryDirectory directory("rigel_memory_split_zone");
    FormatRegistry registry;
    registerFormats(registry);
    PersistenceService service(registry);
    auto storage = std::make_shared<FilesystemBackend>();
    auto context = makeContext("memory", directory.path().string(), storage);
    const std::string zoneId = "rigel:default";

    service.saveZoneMetadata(ZoneMetadata{zoneId, "Current"}, context);
    const auto canonicalPath =
        directory.path() / "zones" / "rigel" / "default" / "zone.meta";
    const auto previousPath =
        directory.path() / "zones" / "rigel:default" / "zone.meta";
    auto previousSession = storage->openWrite(previousPath.string());
    previousSession->commit();

    std::string diagnostic;
    try {
        service.saveZoneMetadata(ZoneMetadata{zoneId, "Replacement"}, context);
    } catch (const std::runtime_error& error) {
        diagnostic = error.what();
    }

    CHECK(diagnostic.find("MemoryFormat configuration error") !=
          std::string::npos);
    CHECK(diagnostic.find(zoneId) != std::string::npos);
    CHECK(diagnostic.find("both") != std::string::npos);
    CHECK(std::filesystem::exists(canonicalPath));
    CHECK(std::filesystem::exists(previousPath));
}
#endif
