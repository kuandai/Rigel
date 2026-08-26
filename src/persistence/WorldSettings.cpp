#include "Rigel/Persistence/WorldSettings.h"

#include "Rigel/Persistence/Storage.h"
#include "Rigel/Util/Ryml.h"
#include "Rigel/Voxel/GeneratorSnapshot.h"

#include <ryml.hpp>
#include <ryml_std.hpp>

#include <charconv>
#include <cstddef>
#include <exception>
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
constexpr size_t kMaxWorldSettingsBytes = 16 * 1024;
constexpr size_t kMaxGeneratorSnapshotBytes = 4 * 1024 * 1024;

StorageBackend& storageFor(const PersistenceContext& context) {
    if (!context.storage) {
        throw std::invalid_argument(
            "World generation persistence requires a storage backend");
    }
    if (context.rootPath.empty()) {
        throw std::invalid_argument(
            "World generation persistence requires a world root path");
    }
    return *context.storage;
}

std::string childPath(const PersistenceContext& context,
                      std::string_view filename) {
    return context.rootPath + "/" + std::string(filename);
}

std::string quoted(std::string_view value) {
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
    static_cast<void>(quoted(settings.displayName));
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
    out += "schema_version: " + std::to_string(settings.schemaVersion) + "\n";
    out += "display_name: " + quoted(settings.displayName) + "\n";
    out += "seed: " + std::to_string(settings.seed) + "\n";
    out += "generator:\n";
    out += "  id: " + quoted(settings.generator.sourceId) + "\n";
    out += "  source_revision: " +
        std::to_string(settings.generator.sourceRevision) + "\n";
    out += "  definition_schema_version: " +
        std::to_string(settings.generator.definitionSchemaVersion) + "\n";
    out += "  semantics_version: " +
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
    requireMapKeys(
        root,
        "world",
        {"schema_version", "display_name", "seed", "generator"});
    const ryml::ConstNodeRef generator = root["generator"];
    requireMapKeys(
        generator,
        "world.generator",
        {"id", "source_revision", "definition_schema_version",
         "semantics_version"});

    WorldSettings settings;
    settings.schemaVersion = readUint32(
        root, "schema_version", "world.schema_version");
    settings.displayName = readString(
        root, "display_name", "world.display_name");
    settings.seed = readUint32(root, "seed", "world.seed");
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

void removeIfPresent(StorageBackend& storage, const std::string& path) {
    if (storage.exists(path)) {
        storage.remove(path);
    }
}

void rollbackNewWorld(StorageBackend& storage,
                      const PersistenceContext& context) {
    removeIfPresent(storage, childPath(context, kWorldSettingsFilename));
    removeIfPresent(storage, childPath(context, kGeneratorSnapshotFilename));
    removeIfPresent(storage, context.rootPath);
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

void publishNewWorldGeneration(const WorldSettings& settings,
                               const Voxel::WorldGenConfig& definition,
                               const PersistenceContext& context) {
    StorageBackend& storage = storageFor(context);
    validateSettings(settings);
    if (settings.seed != definition.seed) {
        throw std::invalid_argument(
            "World settings seed does not match the resolved generator input");
    }
    if (settings.generator.sourceRevision != definition.world.version) {
        throw std::invalid_argument(
            "World generator source revision does not match the resolved generator input");
    }
    const std::string snapshot = Voxel::serializeGeneratorSnapshot(definition);
    const std::string settingsDocument = serializeSettings(settings);

    if (storage.exists(context.rootPath)) {
        throw std::runtime_error(
            "Cannot create world because the save root already exists: " +
            context.rootPath);
    }

    try {
        writeDocument(
            storage,
            childPath(context, kGeneratorSnapshotFilename),
            snapshot);
        writeDocument(
            storage,
            childPath(context, kWorldSettingsFilename),
            settingsDocument);
    } catch (...) {
        const std::exception_ptr publicationFailure = std::current_exception();
        try {
            rollbackNewWorld(storage, context);
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
        settings.generator.sourceRevision);
    return saved;
}

} // namespace Rigel::Persistence
