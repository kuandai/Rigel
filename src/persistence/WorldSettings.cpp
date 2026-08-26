#include "Rigel/Persistence/WorldSettings.h"

#include "Rigel/Persistence/Storage.h"
#include "Rigel/Util/Ryml.h"
#include "Rigel/Voxel/GeneratorSnapshot.h"

#include <ryml.hpp>
#include <ryml_std.hpp>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace Rigel::Persistence {
namespace {

constexpr std::string_view kWorldSettingsFilename = "world-settings.yaml";
constexpr std::string_view kGeneratorSnapshotFilename =
    "generator-definition.yaml";
constexpr std::string_view kStagingOwnershipFilename =
    ".rigel-world-generation-stage";
constexpr std::string_view kCleanupOwnershipSuffix = ".rigel-cleanup";
constexpr size_t kMaxWorldSettingsBytes = 16 * 1024;
constexpr size_t kMaxGeneratorSnapshotBytes = 4 * 1024 * 1024;
constexpr size_t kWorldGenerationStagingSlotCount = 64;

std::filesystem::path worldRootPath(const PersistenceContext& context) {
    if (context.rootPath.empty()) {
        throw std::invalid_argument(
            "World generation persistence requires a world root path");
    }
    const std::filesystem::path root(context.rootPath);
    const std::filesystem::path name = root.filename();
    if (name.empty() || name == "." || name == "..") {
        throw std::invalid_argument(
            "World generation persistence requires a named world root path without a trailing separator");
    }
    return root;
}

StorageBackend& storageFor(const PersistenceContext& context) {
    if (!context.storage) {
        throw std::invalid_argument(
            "World generation persistence requires a storage backend");
    }
    static_cast<void>(worldRootPath(context));
    return *context.storage;
}

std::string childPath(const PersistenceContext& context,
                      std::string_view filename) {
    return context.rootPath + "/" + std::string(filename);
}

std::string quoteYamlString(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('"');
    for (const unsigned char byte : value) {
        switch (byte) {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (byte < 0x20 || byte == 0x7f) {
                throw std::invalid_argument(
                    "World settings strings cannot contain control characters");
            }
            out.push_back(static_cast<char>(byte));
            break;
        }
    }
    out.push_back('"');
    return out;
}

bool validGeneratorSourceId(std::string_view id) {
    const size_t separator = id.find(':');
    if (separator == std::string_view::npos || separator == 0 ||
        separator + 1 == id.size()) {
        return false;
    }
    for (const unsigned char byte : id) {
        if (byte <= 0x20 || byte == 0x7f) {
            return false;
        }
    }
    return true;
}

void validateSettings(const WorldSettings& settings) {
    if (settings.schemaVersion != kWorldSettingsSchemaVersion) {
        throw std::invalid_argument(
            "Unsupported world settings schema version: " +
            std::to_string(settings.schemaVersion));
    }
    if (settings.displayName.empty() || settings.displayName.size() > 256) {
        throw std::invalid_argument(
            "World display name must contain between 1 and 256 bytes");
    }
    static_cast<void>(quoteYamlString(settings.displayName));
    if (!validGeneratorSourceId(settings.generator.sourceId)) {
        throw std::invalid_argument(
            "World generator source ID must be a non-empty namespaced ID");
    }
    if (settings.generator.sourceRevision == 0) {
        throw std::invalid_argument(
            "World generator source revision must be greater than zero");
    }
    if (settings.generator.definitionSchemaVersion !=
        Voxel::kGeneratorDefinitionSchemaVersion) {
        throw std::invalid_argument(
            "Unsupported generator definition schema version: " +
            std::to_string(settings.generator.definitionSchemaVersion));
    }
    if (settings.generator.semanticsVersion !=
        Voxel::kGeneratorSemanticsVersion) {
        throw std::invalid_argument(
            "Unsupported generator semantics version: " +
            std::to_string(settings.generator.semanticsVersion));
    }
}

std::string serializeSettings(const WorldSettings& settings) {
    validateSettings(settings);
    std::string out;
    out += "world:\n";
    out += "  schema_version: " + std::to_string(settings.schemaVersion) + "\n";
    out += "  display_name: " + quoteYamlString(settings.displayName) + "\n";
    out += "  seed: " + std::to_string(settings.seed) + "\n";
    out += "  generator:\n";
    out += "    id: " + quoteYamlString(settings.generator.sourceId) + "\n";
    out += "    source_revision: " +
        std::to_string(settings.generator.sourceRevision) + "\n";
    out += "    definition_schema_version: " +
        std::to_string(settings.generator.definitionSchemaVersion) + "\n";
    out += "    semantics_version: " +
        std::to_string(settings.generator.semanticsVersion) + "\n";
    return out;
}

void requireMapKeys(ryml::ConstNodeRef node,
                    std::string_view path,
                    std::initializer_list<std::string_view> keys) {
    if (!node.readable() || !node.is_map()) {
        throw std::invalid_argument(
            "Saved world settings '" + std::string(path) +
            "' must be a mapping");
    }
    std::unordered_set<std::string_view> expected(keys.begin(), keys.end());
    for (const ryml::ConstNodeRef child : node.children()) {
        const std::string key = Util::toStdString(child.key());
        if (!expected.contains(key)) {
            throw std::invalid_argument(
                "Unknown saved world settings field '" +
                std::string(path) + "." + key + "'");
        }
    }
    for (const std::string_view key : keys) {
        if (!node.has_child(ryml::csubstr(key.data(), key.size()))) {
            throw std::invalid_argument(
                "Missing saved world settings field '" +
                std::string(path) + "." + std::string(key) + "'");
        }
    }
}

uint32_t readUint32(ryml::ConstNodeRef node,
                    std::string_view key,
                    std::string_view path) {
    const ryml::ConstNodeRef value = node[ryml::csubstr(key.data(), key.size())];
    if (!value.has_val()) {
        throw std::invalid_argument(
            "Saved world settings field '" + std::string(path) +
            "' must be an unsigned integer");
    }
    const std::string scalar = Util::toStdString(value.val());
    uint32_t result = 0;
    const auto parsed = std::from_chars(
        scalar.data(), scalar.data() + scalar.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != scalar.data() + scalar.size()) {
        throw std::invalid_argument(
            "Saved world settings field '" + std::string(path) +
            "' must be an unsigned integer");
    }
    return result;
}

std::string readString(ryml::ConstNodeRef node,
                       std::string_view key,
                       std::string_view path) {
    const ryml::ConstNodeRef value = node[ryml::csubstr(key.data(), key.size())];
    if (!value.has_val()) {
        throw std::invalid_argument(
            "Saved world settings field '" + std::string(path) +
            "' must be a string");
    }
    std::string result;
    value >> result;
    return result;
}

WorldSettings parseSettings(std::string_view yaml) {
    if (yaml.empty()) {
        throw std::invalid_argument("Saved world settings are empty");
    }
    ryml::Tree tree = ryml::parse_in_arena(
        "world-settings.yaml",
        ryml::csubstr(yaml.data(), yaml.size()));
    const ryml::ConstNodeRef root = tree.rootref();
    requireMapKeys(root, "document", {"world"});
    const ryml::ConstNodeRef world = root["world"];
    requireMapKeys(
        world,
        "world",
        {"schema_version", "display_name", "seed", "generator"});
    const ryml::ConstNodeRef generator = world["generator"];
    requireMapKeys(
        generator,
        "world.generator",
        {"id", "source_revision", "definition_schema_version",
         "semantics_version"});

    WorldSettings settings;
    settings.schemaVersion = readUint32(
        world, "schema_version", "world.schema_version");
    settings.displayName = readString(
        world, "display_name", "world.display_name");
    settings.seed = readUint32(world, "seed", "world.seed");
    settings.generator.sourceId = readString(
        generator, "id", "world.generator.id");
    settings.generator.sourceRevision = readUint32(
        generator, "source_revision", "world.generator.source_revision");
    settings.generator.definitionSchemaVersion = readUint32(
        generator,
        "definition_schema_version",
        "world.generator.definition_schema_version");
    settings.generator.semanticsVersion = readUint32(
        generator,
        "semantics_version",
        "world.generator.semantics_version");
    validateSettings(settings);
    if (serializeSettings(settings) != yaml) {
        throw std::invalid_argument(
            "Saved world settings are not in canonical form");
    }
    return settings;
}

std::string readDocument(StorageBackend& storage,
                         const std::string& path,
                         size_t maximumBytes,
                         std::string_view description) {
    auto reader = storage.openRead(path);
    if (reader->size() > maximumBytes) {
        throw std::runtime_error(
            std::string(description) + " exceeds the supported size limit");
    }
    const std::vector<uint8_t> bytes = reader->readAt(0, reader->size());
    return std::string(bytes.begin(), bytes.end());
}

void writeDocument(StorageBackend& storage,
                   const std::string& path,
                   std::string_view document) {
    auto session = storage.openWrite(path);
    if (!document.empty()) {
        session->writer().writeBytes(
            reinterpret_cast<const uint8_t*>(document.data()),
            document.size());
    }
    session->commit();
}

void removeEntryIfPresent(StorageBackend& storage, const std::string& path) {
    if (storage.entryKind(path) != StorageEntryKind::Missing) {
        storage.remove(path);
        if (storage.entryKind(path) != StorageEntryKind::Missing) {
            throw std::runtime_error(
                "Storage backend did not remove entry: " + path);
        }
    }
}

std::string stagingRoot(const PersistenceContext& context, size_t slot) {
    return context.rootPath + ".staging." + std::to_string(slot);
}

std::string normalizedPathIdentity(const std::filesystem::path& path) {
    return path.lexically_normal().generic_string();
}

void appendBoundPath(std::string& marker,
                     std::string_view field,
                     const std::filesystem::path& path) {
    const std::string identity = normalizedPathIdentity(path);
    marker += field;
    marker += "-bytes: ";
    marker += std::to_string(identity.size());
    marker += "\n";
    marker += identity;
    marker += "\n";
}

std::string stagingOwnershipMarker(
    const std::filesystem::path& worldRoot,
    const std::filesystem::path& stagedRoot) {
    std::string marker = "rigel-world-generation-staging\nversion: 1\n";
    appendBoundPath(marker, "world-root", worldRoot);
    appendBoundPath(marker, "staging-root", stagedRoot);
    return marker;
}

std::filesystem::path cleanupOwnershipPath(
    const std::filesystem::path& stagedRoot) {
    return std::filesystem::path(
        stagedRoot.string() + std::string(kCleanupOwnershipSuffix));
}

std::string cleanupOwnershipMarker(
    const std::filesystem::path& worldRoot,
    const std::filesystem::path& stagedRoot) {
    std::string marker =
        "rigel-world-generation-staging-cleanup\nversion: 1\n";
    appendBoundPath(marker, "world-root", worldRoot);
    appendBoundPath(marker, "staging-root", stagedRoot);
    return marker;
}

bool isWorldGenerationStagingName(std::string_view worldName,
                                  std::string_view entryName) {
    const std::string stagingPrefix =
        std::string(worldName) + ".staging.";
    if (!entryName.starts_with(stagingPrefix)) {
        return false;
    }

    const std::string_view suffix = entryName.substr(stagingPrefix.size());
    size_t slot = 0;
    const auto parsed =
        std::from_chars(suffix.data(), suffix.data() + suffix.size(), slot);
    return parsed.ec == std::errc{} &&
        parsed.ptr == suffix.data() + suffix.size() &&
        std::to_string(slot) == suffix &&
        slot < kWorldGenerationStagingSlotCount;
}

bool stagingNameFromCleanupName(std::string_view worldName,
                                std::string_view entryName,
                                std::string& stagingName) {
    if (!entryName.ends_with(kCleanupOwnershipSuffix)) {
        return false;
    }
    const std::string_view candidate = entryName.substr(
        0, entryName.size() - kCleanupOwnershipSuffix.size());
    if (!isWorldGenerationStagingName(worldName, candidate)) {
        return false;
    }
    stagingName = candidate;
    return true;
}

bool hasExactRegularFile(StorageBackend& storage,
                         const std::filesystem::path& path,
                         const std::string& expected) {
    try {
        if (storage.entryKind(path.string()) !=
            StorageEntryKind::RegularFile) {
            return false;
        }
        auto reader = storage.openRead(path.string());
        if (reader->size() != expected.size()) {
            return false;
        }
        const std::vector<uint8_t> bytes =
            reader->readAt(0, reader->size());
        return std::string(bytes.begin(), bytes.end()) == expected;
    } catch (...) {
        return false;
    }
}

bool hasValidStagingOwnershipMarker(
    StorageBackend& storage,
    const std::filesystem::path& worldRoot,
    const PersistenceContext& stagedContext) {
    try {
        if (storage.entryKind(stagedContext.rootPath) !=
            StorageEntryKind::Directory) {
            return false;
        }
        const std::filesystem::path markerPath =
            childPath(stagedContext, kStagingOwnershipFilename);
        const std::string expected = stagingOwnershipMarker(
            worldRoot, std::filesystem::path(stagedContext.rootPath));
        return hasExactRegularFile(storage, markerPath, expected);
    } catch (...) {
        return false;
    }
}

bool hasValidCleanupOwnershipMarker(
    StorageBackend& storage,
    const std::filesystem::path& worldRoot,
    const PersistenceContext& stagedContext) {
    const std::filesystem::path stagedRoot(stagedContext.rootPath);
    return hasExactRegularFile(
        storage,
        cleanupOwnershipPath(stagedRoot),
        cleanupOwnershipMarker(worldRoot, stagedRoot));
}

void ensureCleanupOwnershipMarker(
    StorageBackend& storage,
    const std::filesystem::path& worldRoot,
    const PersistenceContext& stagedContext) {
    const std::filesystem::path stagedRoot(stagedContext.rootPath);
    const std::filesystem::path markerPath =
        cleanupOwnershipPath(stagedRoot);
    const std::string marker = cleanupOwnershipMarker(worldRoot, stagedRoot);
    if (hasExactRegularFile(storage, markerPath, marker)) {
        return;
    }
    if (storage.entryKind(markerPath.string()) !=
        StorageEntryKind::Missing) {
        throw std::runtime_error(
            "Cannot clean staged world because its cleanup ownership path is occupied: " +
            markerPath.string());
    }
    if (!storage.createFileExclusive(markerPath.string(), marker) &&
        !hasExactRegularFile(storage, markerPath, marker)) {
        throw std::runtime_error(
            "Cannot clean staged world because cleanup ownership could not be reserved: " +
            markerPath.string());
    }
    if (!hasExactRegularFile(storage, markerPath, marker)) {
        throw std::runtime_error(
            "Cannot clean staged world because cleanup ownership was not committed: " +
            markerPath.string());
    }
}

bool reserveCleanupOwnershipMarker(
    StorageBackend& storage,
    const std::filesystem::path& worldRoot,
    const PersistenceContext& stagedContext) {
    const std::filesystem::path stagedRoot(stagedContext.rootPath);
    const std::filesystem::path markerPath =
        cleanupOwnershipPath(stagedRoot);
    const std::string marker = cleanupOwnershipMarker(worldRoot, stagedRoot);
    if (!storage.createFileExclusive(markerPath.string(), marker)) {
        return false;
    }
    if (!hasExactRegularFile(storage, markerPath, marker)) {
        throw std::runtime_error(
            "Cannot reserve staged-world cleanup ownership because the marker was not committed: " +
            markerPath.string());
    }
    return true;
}

void removeCleanupOwnershipMarker(
    StorageBackend& storage,
    const std::filesystem::path& worldRoot,
    const PersistenceContext& stagedContext) {
    const std::filesystem::path stagedRoot(stagedContext.rootPath);
    const std::filesystem::path cleanupPath =
        cleanupOwnershipPath(stagedRoot);
    if (!hasValidCleanupOwnershipMarker(
            storage, worldRoot, stagedContext)) {
        throw std::runtime_error(
            "Refusing to remove invalid staged-world cleanup ownership: " +
            cleanupPath.string());
    }
    removeEntryIfPresent(storage, cleanupPath.string());
}

void removeStagingDirectory(
    StorageBackend& storage,
    const PersistenceContext& stagedContext) {
    std::exception_ptr firstFailure;
    std::vector<std::string> entries;
    const std::filesystem::path stagedRoot(stagedContext.rootPath);
    const StorageEntryKind stagedKind =
        storage.entryKind(stagedContext.rootPath);
    if (stagedKind == StorageEntryKind::Directory) {
        try {
            entries = storage.list(stagedContext.rootPath);
        } catch (...) {
            firstFailure = std::current_exception();
        }
    } else if (stagedKind != StorageEntryKind::Missing) {
        firstFailure = std::make_exception_ptr(std::runtime_error(
            "Refusing to clean a staged world path that is not a directory: " +
            stagedContext.rootPath));
    }

    std::stable_sort(
        entries.begin(),
        entries.end(),
        [](const std::string& lhs, const std::string& rhs) {
            const bool lhsIsMarker =
                std::filesystem::path(lhs).filename() ==
                kStagingOwnershipFilename;
            const bool rhsIsMarker =
                std::filesystem::path(rhs).filename() ==
                kStagingOwnershipFilename;
            return lhsIsMarker < rhsIsMarker;
        });
    for (const std::string& path : entries) {
        try {
            const std::filesystem::path entry(path);
            if (normalizedPathIdentity(entry.parent_path()) !=
                normalizedPathIdentity(stagedRoot)) {
                throw std::runtime_error(
                    "Refusing to clean an entry outside the owned staging directory: " +
                    path);
            }
            removeEntryIfPresent(storage, path);
        } catch (...) {
            if (!firstFailure) {
                firstFailure = std::current_exception();
            }
        }
    }

    try {
        const StorageEntryKind currentKind =
            storage.entryKind(stagedContext.rootPath);
        if (currentKind == StorageEntryKind::Directory) {
            storage.remove(stagedContext.rootPath);
            if (storage.entryKind(stagedContext.rootPath) !=
                StorageEntryKind::Missing) {
                throw std::runtime_error(
                    "Storage backend did not remove staged world directory: " +
                    stagedContext.rootPath);
            }
        } else if (currentKind != StorageEntryKind::Missing) {
            throw std::runtime_error(
                "Refusing to remove a staged world path that is no longer a directory: " +
                stagedContext.rootPath);
        }
    } catch (...) {
        if (!firstFailure) {
            firstFailure = std::current_exception();
        }
    }
    if (firstFailure) {
        std::rethrow_exception(firstFailure);
    }
}

void removeCurrentStagingReservation(
    StorageBackend& storage,
    const PersistenceContext& stagedContext) {
    removeStagingDirectory(storage, stagedContext);
}

void removeStagingWorld(StorageBackend& storage,
                        const std::filesystem::path& worldRoot,
                        const PersistenceContext& stagedContext) {
    const StorageEntryKind stagedKind =
        storage.entryKind(stagedContext.rootPath);
    const bool hasStagingOwnership = hasValidStagingOwnershipMarker(
        storage, worldRoot, stagedContext);
    const bool hasCleanupOwnership = hasValidCleanupOwnershipMarker(
        storage, worldRoot, stagedContext);

    if (stagedKind == StorageEntryKind::Missing) {
        if (hasCleanupOwnership) {
            removeCleanupOwnershipMarker(
                storage, worldRoot, stagedContext);
        }
        return;
    }
    if (stagedKind != StorageEntryKind::Directory) {
        if (hasStagingOwnership || hasCleanupOwnership) {
            throw std::runtime_error(
                "Refusing to clean a staged world path that is not a directory: " +
                stagedContext.rootPath);
        }
        return;
    }
    if (!hasStagingOwnership) {
        // The external tombstone records an interrupted cleanup, but the
        // bounded slot may have been reused after publication. Only the
        // marker inside the current directory can authorize deleting it.
        return;
    }

    ensureCleanupOwnershipMarker(storage, worldRoot, stagedContext);

    removeStagingDirectory(storage, stagedContext);

    removeCleanupOwnershipMarker(
        storage, worldRoot, stagedContext);
}

std::vector<PersistenceContext> stagingRecoveryCandidates(
    const PersistenceContext& context,
    const std::filesystem::path& parent,
    std::string_view worldName,
    const std::vector<std::string>& entries) {
    std::vector<PersistenceContext> candidates;
    std::vector<std::string> identities;
    for (const std::string& entry : entries) {
        const std::filesystem::path entryPath(entry);
        const std::string entryName = entryPath.filename().string();
        std::string stagingName;
        if (isWorldGenerationStagingName(worldName, entryName)) {
            stagingName = entryName;
        } else if (!stagingNameFromCleanupName(
                       worldName, entryName, stagingName)) {
            continue;
        }

        const std::filesystem::path expectedEntry = parent / entryName;
        if (normalizedPathIdentity(entryPath) !=
            normalizedPathIdentity(expectedEntry)) {
            continue;
        }

        const std::filesystem::path stagedRoot = parent / stagingName;
        const std::string identity = normalizedPathIdentity(stagedRoot);
        if (std::find(identities.begin(), identities.end(), identity) !=
            identities.end()) {
            continue;
        }
        identities.push_back(identity);
        PersistenceContext stagedContext = context;
        stagedContext.rootPath = stagedRoot.string();
        candidates.push_back(std::move(stagedContext));
    }
    return candidates;
}

bool hasRecoverableStaging(
    StorageBackend& storage,
    const std::filesystem::path& worldRoot,
    const std::vector<PersistenceContext>& candidates) {
    for (const PersistenceContext& stagedContext : candidates) {
        if (hasValidStagingOwnershipMarker(
                storage, worldRoot, stagedContext) ||
            hasValidCleanupOwnershipMarker(
                storage, worldRoot, stagedContext)) {
            return true;
        }
    }
    return false;
}

void recoverAbandonedWorldGenerationStagingLocked(
    StorageBackend& storage,
    const PersistenceContext& context,
    const std::filesystem::path& worldRoot) {
    const std::string worldName = worldRoot.filename().string();
    std::filesystem::path parent = worldRoot.parent_path();
    if (parent.empty()) {
        parent = ".";
    }

    const std::vector<PersistenceContext> candidates =
        stagingRecoveryCandidates(
            context,
            parent,
            worldName,
            storage.list(parent.string()));
    std::exception_ptr firstFailure;
    for (const PersistenceContext& stagedContext : candidates) {
        try {
            removeStagingWorld(storage, worldRoot, stagedContext);
        } catch (...) {
            if (!firstFailure) {
                firstFailure = std::current_exception();
            }
        }
    }
    if (firstFailure) {
        std::rethrow_exception(firstFailure);
    }
}

} // namespace

SavedWorldGenerationPresence inspectSavedWorldGeneration(
    const PersistenceContext& context) {
    StorageBackend& storage = storageFor(context);
    if (!storage.exists(context.rootPath)) {
        return SavedWorldGenerationPresence::Missing;
    }

    const bool hasSettings = storage.exists(
        childPath(context, kWorldSettingsFilename));
    const bool hasSnapshot = storage.exists(
        childPath(context, kGeneratorSnapshotFilename));
    if (hasSettings && hasSnapshot) {
        return SavedWorldGenerationPresence::Published;
    }
    return SavedWorldGenerationPresence::LegacyOrIncomplete;
}

void recoverAbandonedWorldGenerationStaging(
    const PersistenceContext& context) {
    StorageBackend& storage = storageFor(context);
    const std::filesystem::path worldRoot = worldRootPath(context);
    const std::string worldName = worldRoot.filename().string();

    std::filesystem::path parent = worldRoot.parent_path();
    if (parent.empty()) {
        parent = ".";
    }
    std::vector<PersistenceContext> candidates = stagingRecoveryCandidates(
        context,
        parent,
        worldName,
        storage.list(parent.string()));
    if (!hasRecoverableStaging(storage, worldRoot, candidates)) {
        return;
    }

    auto bootstrapLock =
        storage.lockWorldGenerationBootstrap(context.rootPath);
    recoverAbandonedWorldGenerationStagingLocked(
        storage, context, worldRoot);
}

void publishNewWorldGeneration(const WorldSettings& settings,
                               const Voxel::WorldGenConfig& definition,
                               const PersistenceContext& context) {
    StorageBackend& storage = storageFor(context);
    validateSettings(settings);
    if (settings.seed != definition.seed) {
        throw std::invalid_argument(
            "World settings seed does not match the resolved generator input");
    }
    const std::string snapshot = Voxel::serializeGeneratorSnapshot(definition);
    const std::string settingsDocument = serializeSettings(settings);
    if (settingsDocument.size() > kMaxWorldSettingsBytes) {
        throw std::length_error(
            "World settings exceed the supported size limit");
    }
    if (snapshot.size() > kMaxGeneratorSnapshotBytes) {
        throw std::length_error(
            "Generator definition snapshot exceeds the supported size limit");
    }
    static_cast<void>(Voxel::parseGeneratorSnapshot(
        snapshot,
        settings.generator.definitionSchemaVersion,
        settings.seed,
        settings.generator.semanticsVersion));

    const std::filesystem::path worldRoot = worldRootPath(context);
    auto bootstrapLock =
        storage.lockWorldGenerationBootstrap(context.rootPath);
    recoverAbandonedWorldGenerationStagingLocked(
        storage, context, worldRoot);
    if (storage.exists(context.rootPath)) {
        throw std::runtime_error(
            "Cannot create world because the save root already exists: " +
            context.rootPath);
    }

    PersistenceContext stagedContext = context;
    bool reserved = false;
    for (size_t slot = 0;
         slot < kWorldGenerationStagingSlotCount;
         ++slot) {
        stagedContext.rootPath = stagingRoot(context, slot);
        if (storage.entryKind(stagedContext.rootPath) !=
            StorageEntryKind::Missing) {
            continue;
        }

        bool directoryCreated = false;
        try {
            directoryCreated =
                storage.createDirectoryExclusive(stagedContext.rootPath);
        } catch (...) {
            const std::exception_ptr reservationFailure =
                std::current_exception();
            try {
                if (storage.entryKind(stagedContext.rootPath) ==
                    StorageEntryKind::Directory) {
                    removeCurrentStagingReservation(
                        storage, stagedContext);
                }
            } catch (const std::exception& rollbackFailure) {
                throw std::runtime_error(
                    "Staging reservation failed and rollback could not "
                    "remove the newly reserved directory: " +
                    std::string(rollbackFailure.what()));
            }
            std::rethrow_exception(reservationFailure);
        }
        if (!directoryCreated) {
            continue;
        }

        try {
            writeDocument(
                storage,
                childPath(stagedContext, kStagingOwnershipFilename),
                stagingOwnershipMarker(
                    worldRoot,
                    std::filesystem::path(stagedContext.rootPath)));
            if (!hasValidStagingOwnershipMarker(
                    storage, worldRoot, stagedContext)) {
                throw std::runtime_error(
                    "Staging ownership marker was not committed: " +
                    stagedContext.rootPath);
            }
        } catch (...) {
            const std::exception_ptr markerFailure =
                std::current_exception();
            try {
                removeCurrentStagingReservation(
                    storage, stagedContext);
            } catch (const std::exception& rollbackFailure) {
                throw std::runtime_error(
                    "Staging ownership commit failed and rollback could not "
                    "remove the newly reserved directory: " +
                    std::string(rollbackFailure.what()));
            }
            std::rethrow_exception(markerFailure);
        }

        bool cleanupReserved = false;
        try {
            cleanupReserved = reserveCleanupOwnershipMarker(
                storage, worldRoot, stagedContext);
        } catch (...) {
            const std::exception_ptr cleanupReservationFailure =
                std::current_exception();
            try {
                removeStagingWorld(storage, worldRoot, stagedContext);
            } catch (const std::exception& rollbackFailure) {
                throw std::runtime_error(
                    "Cleanup ownership reservation failed and rollback could "
                    "not remove the staged save: " +
                    std::string(rollbackFailure.what()));
            }
            std::rethrow_exception(cleanupReservationFailure);
        }
        if (!cleanupReserved) {
            try {
                removeCurrentStagingReservation(
                    storage, stagedContext);
            } catch (const std::exception& rollbackFailure) {
                throw std::runtime_error(
                    "Cleanup ownership path is occupied and rollback could "
                    "not remove the staged save: " +
                    std::string(rollbackFailure.what()));
            }
            continue;
        }
        reserved = true;
        break;
    }
    if (!reserved) {
        throw std::runtime_error(
            "Cannot create world because all 64 bounded staging slots are "
            "occupied; inspect and remove only verified unowned remnants for: " +
            context.rootPath);
    }

    bool publicationAttempted = false;
    bool publicationCompleted = false;
    try {
        writeDocument(
            storage,
            childPath(stagedContext, kGeneratorSnapshotFilename),
            snapshot);
        writeDocument(
            storage,
            childPath(stagedContext, kWorldSettingsFilename),
            settingsDocument);
        publicationAttempted = true;
        storage.publishDirectory(stagedContext.rootPath, context.rootPath);
        publicationCompleted = true;
        removeCleanupOwnershipMarker(
            storage, worldRoot, stagedContext);
    } catch (...) {
        const std::exception_ptr publicationFailure = std::current_exception();
        if (publicationCompleted) {
            std::rethrow_exception(publicationFailure);
        }
        try {
            if (publicationAttempted &&
                storage.entryKind(context.rootPath) !=
                    StorageEntryKind::Missing) {
                removeCleanupOwnershipMarker(
                    storage, worldRoot, stagedContext);
            } else {
                removeStagingWorld(
                    storage,
                    worldRoot,
                    stagedContext);
            }
        } catch (const std::exception& rollbackFailure) {
            throw std::runtime_error(
                "World creation failed and rollback could not remove the staged save: " +
                std::string(rollbackFailure.what()));
        }
        std::rethrow_exception(publicationFailure);
    }
}

SavedWorldGeneration loadSavedWorldGeneration(
    const PersistenceContext& context) {
    StorageBackend& storage = storageFor(context);
    switch (inspectSavedWorldGeneration(context)) {
    case SavedWorldGenerationPresence::Missing:
        throw std::runtime_error(
            "World save does not exist: " + context.rootPath);
    case SavedWorldGenerationPresence::LegacyOrIncomplete:
        throw std::runtime_error(
            "World save is legacy, unknown, or incompletely published and cannot be loaded: " +
            context.rootPath);
    case SavedWorldGenerationPresence::Published:
        break;
    }

    const std::string settingsDocument = readDocument(
        storage,
        childPath(context, kWorldSettingsFilename),
        kMaxWorldSettingsBytes,
        "Saved world settings");
    const WorldSettings settings = parseSettings(settingsDocument);
    const std::string snapshot = readDocument(
        storage,
        childPath(context, kGeneratorSnapshotFilename),
        kMaxGeneratorSnapshotBytes,
        "Saved generator definition");

    SavedWorldGeneration saved;
    saved.settings = settings;
    saved.definition = Voxel::parseGeneratorSnapshot(
        snapshot,
        settings.generator.definitionSchemaVersion,
        settings.seed,
        settings.generator.semanticsVersion);
    return saved;
}

} // namespace Rigel::Persistence
