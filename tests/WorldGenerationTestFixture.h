#pragma once

#include "Rigel/Persistence/Storage.h"
#include "Rigel/Persistence/PersistenceService.h"
#include "Rigel/Persistence/WorldSettings.h"
#include "Rigel/Voxel/GeneratorDefinition.h"
#include "GeneratorDefinitionTestRegistry.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

namespace Rigel::Test {

struct SavedWorldGenerationFixtureDocuments {
    std::string settings;
    std::string definition;
};

inline Persistence::WorldSettings savedWorldSettingsFixture(
    std::string displayName) {
    Persistence::WorldSettings settings;
    settings.displayName = std::move(displayName);
    settings.seed = 424242u;
    settings.generator.sourceId = "rigel:test_generator";
    settings.generator.sourceRevision = 1;
    settings.generator.definitionSchemaVersion =
        Voxel::kGeneratorDefinitionSchemaVersion;
    settings.generator.semanticsVersion =
        Voxel::kGeneratorSemanticsVersion;
    return settings;
}

inline Voxel::GeneratorDefinitionData savedGeneratorDefinitionFixture(
    const Persistence::WorldSettings& settings) {
    static_cast<void>(settings);
    Voxel::GeneratorDefinitionData definition = generatorDefinitionFixture();
    definition.densityGraph.nodes.front().value = 0.0f;
    return definition;
}

inline std::string quoteWorldSettingsFixtureString(std::string_view value) {
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
            out.push_back(static_cast<char>(byte));
            break;
        }
    }
    out.push_back('"');
    return out;
}

inline SavedWorldGenerationFixtureDocuments savedWorldGenerationFixtureDocuments(
    const Persistence::WorldSettings& settings,
    const Voxel::GeneratorDefinitionData& definition) {
    SavedWorldGenerationFixtureDocuments documents;
    documents.settings =
        "world:\n"
        "  schema_version: " + std::to_string(settings.schemaVersion) + "\n" +
        "  display_name: " +
        quoteWorldSettingsFixtureString(settings.displayName) + "\n" +
        "  seed: " + std::to_string(settings.seed) + "\n" +
        "  generator:\n"
        "    id: " +
        quoteWorldSettingsFixtureString(settings.generator.sourceId) + "\n" +
        "    source_revision: " +
        std::to_string(settings.generator.sourceRevision) + "\n" +
        "    definition_schema_version: " +
        std::to_string(settings.generator.definitionSchemaVersion) + "\n" +
        "    semantics_version: " +
        std::to_string(settings.generator.semanticsVersion) + "\n";
    documents.definition =
        Voxel::serializeGeneratorDefinitionSnapshot(definition);
    return documents;
}

inline SavedWorldGenerationFixtureDocuments savedWorldGenerationFixtureDocuments(
    const Persistence::WorldSettings& settings) {
    return savedWorldGenerationFixtureDocuments(
        settings, savedGeneratorDefinitionFixture(settings));
}

inline void writeWorldGenerationFixtureDocument(
    Persistence::StorageBackend& storage,
    const std::string& path,
    const std::string& document) {
    auto session = storage.openWrite(path);
    session->writer().writeBytes(
        reinterpret_cast<const uint8_t*>(document.data()), document.size());
    session->writer().flush();
    session->commit();
}

inline void installSavedWorldGenerationDocumentsFixture(
    Persistence::StorageBackend& storage,
    const std::string& rootPath,
    const Persistence::WorldSettings& settings,
    const Voxel::GeneratorDefinitionData& definition) {
    const SavedWorldGenerationFixtureDocuments documents =
        savedWorldGenerationFixtureDocuments(settings, definition);
    writeWorldGenerationFixtureDocument(
        storage,
        rootPath + "/world-settings.yaml",
        documents.settings);
    writeWorldGenerationFixtureDocument(
        storage,
        rootPath + "/generator-definition.yaml",
        documents.definition);
}

inline void installSavedWorldGenerationDocumentsFixture(
    Persistence::StorageBackend& storage,
    const std::string& rootPath,
    const Persistence::WorldSettings& settings) {
    installSavedWorldGenerationDocumentsFixture(
        storage,
        rootPath,
        settings,
        savedGeneratorDefinitionFixture(settings));
}

inline void installSavedWorldGenerationFixture(
    Persistence::PersistenceService& persistence,
    const Persistence::PersistenceContext& context,
    const Persistence::WorldSettings& settings,
    const Voxel::GeneratorDefinitionData& definition) {
    installSavedWorldGenerationDocumentsFixture(
        *context.storage, context.rootPath, settings, definition);
    persistence.saveWorldMetadata(
        Persistence::WorldMetadata{
            std::filesystem::path(context.rootPath).filename().string(),
            settings.displayName},
        context);
}

inline void installSavedWorldGenerationFixture(
    Persistence::PersistenceService& persistence,
    const Persistence::PersistenceContext& context,
    const Persistence::WorldSettings& settings) {
    installSavedWorldGenerationFixture(
        persistence,
        context,
        settings,
        savedGeneratorDefinitionFixture(settings));
}

} // namespace Rigel::Test
