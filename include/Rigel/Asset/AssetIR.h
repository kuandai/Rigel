#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Rigel::Asset::IR {

using ExtensionMap = std::unordered_map<std::string, std::string>;

struct ModelRefIR {
    std::string identifier;
    std::string sourcePath;
    ExtensionMap extensions;
};

struct MaterialRefIR {
    std::string identifier;
    std::string sourcePath;
    ExtensionMap extensions;
};

struct TextureRefIR {
    std::string identifier;
    std::string sourcePath;
    ExtensionMap extensions;
};

struct BlockStateIR {
    std::string identifier;
    std::string rootIdentifier;
    std::string sourcePath;

    std::string model = "cube";
    std::string renderLayer = "opaque";
    bool isOpaque = true;
    bool isSolid = true;
    bool cullSameType = false;
    uint8_t emittedLight = 0;
    uint8_t lightAttenuation = 15;

    std::unordered_map<std::string, std::string> textures;
    ExtensionMap extensions;
};

struct BlockDefIR {
    std::string rootIdentifier;
    std::string sourcePath;
    std::vector<BlockStateIR> states;
    ExtensionMap extensions;
};

struct EntityDefIR {
    std::string identifier;
    std::string sourcePath;
    std::string modelRef;
    std::string animationRef;
    std::string renderMode;
    std::optional<std::array<float, 3>> renderOffset;
    std::optional<std::array<float, 3>> hitboxMin;
    std::optional<std::array<float, 3>> hitboxMax;
    ExtensionMap extensions;
};

struct ItemDefIR {
    std::string identifier;
    std::string sourcePath;
    std::string modelRef;
    std::string textureRef;
    std::string renderMode;
    ExtensionMap extensions;
};

enum class ValidationSeverity {
    Error,
    Warning
};

struct ValidationIssue {
    ValidationSeverity severity = ValidationSeverity::Error;
    std::string sourcePath;
    std::string identifier;
    std::string field;
    std::string message;
};

struct ValidationSummary {
    size_t errorCount = 0;
    size_t warningCount = 0;
    std::vector<ValidationIssue> samples;
};

struct IdentifierAliasIR {
    std::string domain;
    std::string canonicalIdentifier;
    std::string externalIdentifier;
    std::string sourcePath;
};

struct AssetGraphIR {
    std::vector<BlockDefIR> blocks;
    std::vector<ModelRefIR> models;
    std::vector<MaterialRefIR> materials;
    std::vector<TextureRefIR> textures;
    std::vector<EntityDefIR> entities;
    std::vector<ItemDefIR> items;
    std::vector<IdentifierAliasIR> aliases;
    std::vector<ValidationIssue> compilerDiagnostics;
};

AssetGraphIR compileRigelEmbedded();

AssetGraphIR compileCRFilesystem(const std::filesystem::path& root);

std::vector<ValidationIssue> validate(const AssetGraphIR& graph);
ValidationSummary summarizeValidationIssues(const std::vector<ValidationIssue>& issues,
                                           size_t sampleLimit = 16);

} // namespace Rigel::Asset::IR
