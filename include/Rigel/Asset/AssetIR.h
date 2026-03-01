#pragma once

#include <cstdint>
#include <filesystem>
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
    ExtensionMap extensions;
};

struct ItemDefIR {
    std::string identifier;
    std::string sourcePath;
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
    std::vector<EntityDefIR> entities;
    std::vector<ItemDefIR> items;
    std::vector<IdentifierAliasIR> aliases;
    std::vector<ValidationIssue> compilerDiagnostics;
};

AssetGraphIR compileRigelEmbedded();

AssetGraphIR compileCRFilesystem(const std::filesystem::path& root);

std::vector<ValidationIssue> validate(const AssetGraphIR& graph);

} // namespace Rigel::Asset::IR
