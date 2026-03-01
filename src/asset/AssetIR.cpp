#include "Rigel/Asset/AssetIR.h"
#include "ResourceRegistry.h"

#include <ryml.hpp>
#include <ryml_std.hpp>

#include <algorithm>
#include <fstream>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace Rigel::Asset::IR {

namespace {

bool startsWith(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() &&
           value.compare(0, prefix.size(), prefix) == 0;
}

bool endsWith(std::string_view value, std::string_view suffix) {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool hasAnySuffix(std::string_view value, std::initializer_list<std::string_view> suffixes) {
    for (std::string_view suffix : suffixes) {
        if (endsWith(value, suffix)) {
            return true;
        }
    }
    return false;
}

std::string toLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool isBuiltinBlockModel(std::string_view model) {
    return model == "cube" || model == "cross" || model == "slab";
}

std::string normalizeAssetReference(const std::string& raw) {
    std::string out = raw;
    if (out.empty()) {
        return out;
    }
    std::replace(out.begin(), out.end(), '\\', '/');
    while (startsWith(out, "./")) {
        out.erase(0, 2);
    }
    while (!out.empty() && out.front() == '/') {
        out.erase(out.begin());
    }
    size_t namespaceSep = out.find(':');
    if (namespaceSep != std::string::npos && namespaceSep + 1 < out.size()) {
        out = out.substr(namespaceSep + 1);
    }
    return out;
}

std::string normalizeModelReference(const std::string& model) {
    if (model.empty()) {
        return "cube";
    }
    std::string lowered = toLower(model);
    if (isBuiltinBlockModel(lowered)) {
        return lowered;
    }
    return normalizeAssetReference(model);
}

std::string normalizeRenderLayer(const std::string& layer, bool isOpaque) {
    std::string lowered = toLower(layer);
    if (lowered.empty()) {
        return isOpaque ? "opaque" : "transparent";
    }
    return lowered;
}

std::string stripVariantSuffix(const std::string& id) {
    size_t bracketPos = id.find('[');
    if (bracketPos == std::string::npos) {
        return id;
    }
    return id.substr(0, bracketPos);
}

std::string removeSuffix(std::string_view path, std::string_view suffix) {
    if (!endsWith(path, suffix)) {
        return std::string(path);
    }
    return std::string(path.substr(0, path.size() - suffix.size()));
}

std::optional<std::string> nodeString(ryml::ConstNodeRef node, const char* key) {
    if (!node.readable() || !node.has_child(ryml::to_csubstr(key))) {
        return std::nullopt;
    }
    std::string out;
    node[ryml::to_csubstr(key)] >> out;
    if (out.empty()) {
        return std::nullopt;
    }
    return out;
}

std::optional<bool> nodeBool(ryml::ConstNodeRef node, const char* key) {
    if (!node.readable() || !node.has_child(ryml::to_csubstr(key))) {
        return std::nullopt;
    }
    std::string out;
    node[ryml::to_csubstr(key)] >> out;
    if (out == "true" || out == "yes" || out == "1") {
        return true;
    }
    if (out == "false" || out == "no" || out == "0") {
        return false;
    }
    return std::nullopt;
}

std::optional<uint8_t> nodeU8(ryml::ConstNodeRef node, const char* key) {
    if (!node.readable() || !node.has_child(ryml::to_csubstr(key))) {
        return std::nullopt;
    }
    std::string out;
    node[ryml::to_csubstr(key)] >> out;
    try {
        int parsed = std::stoi(out);
        if (parsed < 0 || parsed > 255) {
            return std::nullopt;
        }
        return static_cast<uint8_t>(parsed);
    } catch (...) {
        return std::nullopt;
    }
}

std::string toGenericPath(const std::filesystem::path& path) {
    return path.generic_string();
}

void sortUnique(std::vector<std::string>& values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
}

std::string readFileText(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("Unable to read file: " + toGenericPath(path));
    }
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

std::vector<std::string> extractStringIds(const std::string& text) {
    static const std::regex stringIdPattern(R"re("stringId"\s*:\s*"([^"]+)")re");
    std::vector<std::string> ids;
    for (std::sregex_iterator it(text.begin(), text.end(), stringIdPattern), end;
         it != end;
         ++it) {
        if ((*it).size() >= 2) {
            ids.push_back((*it)[1].str());
        }
    }
    return ids;
}

std::string rigelManifestNamespace() {
    try {
        auto data = ResourceRegistry::Get("manifest.yaml");
        ryml::Tree tree = ryml::parse_in_arena(
            ryml::to_csubstr("manifest.yaml"),
            ryml::csubstr(data.data(), data.size())
        );
        ryml::ConstNodeRef root = tree.rootref();
        if (auto ns = nodeString(root, "namespace")) {
            return *ns;
        }
    } catch (const std::exception&) {
        // fallback below
    }
    return "base";
}

std::string blockIdFromEmbeddedPath(const std::string& path,
                                    ryml::ConstNodeRef root,
                                    const std::string& manifestNs) {
    std::string id;
    if (auto explicitId = nodeString(root, "id")) {
        id = *explicitId;
    } else if (auto explicitIdentifier = nodeString(root, "identifier")) {
        id = *explicitIdentifier;
    } else if (auto explicitName = nodeString(root, "name")) {
        id = *explicitName;
    } else {
        std::string_view relative(path);
        if (startsWith(relative, "blocks/")) {
            relative.remove_prefix(std::string_view("blocks/").size());
        }
        id = removeSuffix(relative, ".yaml");
    }

    if (id.find(':') == std::string::npos && !manifestNs.empty()) {
        id = manifestNs + ":" + id;
    }
    return id;
}

void collectFilesystemPaths(const std::filesystem::path& root,
                            std::vector<std::filesystem::path>& out,
                            std::initializer_list<std::string_view> suffixes) {
    out.clear();
    if (!std::filesystem::exists(root)) {
        return;
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        std::string path = toGenericPath(entry.path());
        if (!hasAnySuffix(path, suffixes)) {
            continue;
        }
        out.push_back(entry.path());
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        return toGenericPath(a) < toGenericPath(b);
    });
}

std::string relativeOrAbsolute(const std::filesystem::path& path,
                               const std::filesystem::path& base) {
    std::error_code ec;
    auto relative = std::filesystem::relative(path, base, ec);
    if (!ec) {
        return toGenericPath(relative);
    }
    return toGenericPath(path);
}

template <typename T>
void sortByIdentifier(std::vector<T>& values) {
    std::sort(values.begin(), values.end(), [](const T& a, const T& b) {
        if (a.identifier == b.identifier) {
            return a.sourcePath < b.sourcePath;
        }
        return a.identifier < b.identifier;
    });
}

struct OrderedParams {
    std::vector<std::pair<std::string, std::string>> ordered;
    std::unordered_map<std::string, size_t> lookup;
};

void setParam(OrderedParams& params, const std::string& key, const std::string& value) {
    if (key.empty()) {
        return;
    }
    auto it = params.lookup.find(key);
    if (it != params.lookup.end()) {
        params.ordered[it->second].second = value;
        return;
    }
    params.lookup.emplace(key, params.ordered.size());
    params.ordered.emplace_back(key, value);
}

OrderedParams parseParamString(const std::string& stateKey) {
    OrderedParams out;
    if (stateKey.empty()) {
        return out;
    }
    size_t start = 0;
    while (start < stateKey.size()) {
        size_t comma = stateKey.find(',', start);
        std::string_view part(stateKey.data() + start,
                              (comma == std::string::npos) ? stateKey.size() - start : comma - start);
        size_t eq = part.find('=');
        if (eq != std::string_view::npos && eq > 0 && eq + 1 < part.size()) {
            setParam(out,
                     std::string(part.substr(0, eq)),
                     std::string(part.substr(eq + 1)));
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return out;
}

OrderedParams parseParamsNode(ryml::ConstNodeRef node) {
    OrderedParams out;
    if (!node.readable() || !node.is_map()) {
        return out;
    }
    for (ryml::ConstNodeRef child : node.children()) {
        if (!child.has_key() || !child.has_val()) {
            continue;
        }
        std::string key(child.key().data(), child.key().size());
        std::string value;
        child >> value;
        setParam(out, key, value);
    }
    return out;
}

std::string paramsToString(const OrderedParams& params, bool canonicalOrder) {
    std::vector<std::pair<std::string, std::string>> entries = params.ordered;
    if (canonicalOrder) {
        std::sort(entries.begin(), entries.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
    }
    std::string out;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (i > 0) {
            out += ",";
        }
        out += entries[i].first;
        out += "=";
        out += entries[i].second;
    }
    return out;
}

std::string withParams(const std::string& rootId, const std::string& params) {
    if (params.empty()) {
        return rootId;
    }
    return rootId + "[" + params + "]";
}

std::string stripJsonCommentsAndTrailingCommas(const std::string& text) {
    std::string noComments;
    noComments.reserve(text.size());

    bool inString = false;
    bool escaping = false;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (inString) {
            noComments.push_back(c);
            if (escaping) {
                escaping = false;
            } else if (c == '\\') {
                escaping = true;
            } else if (c == '"') {
                inString = false;
            }
            continue;
        }

        if (c == '"') {
            inString = true;
            noComments.push_back(c);
            continue;
        }

        if (c == '/' && i + 1 < text.size()) {
            if (text[i + 1] == '/') {
                i += 2;
                while (i < text.size() && text[i] != '\n') {
                    ++i;
                }
                if (i < text.size()) {
                    noComments.push_back(text[i]);
                }
                continue;
            }
            if (text[i + 1] == '*') {
                i += 2;
                while (i + 1 < text.size() && !(text[i] == '*' && text[i + 1] == '/')) {
                    ++i;
                }
                ++i;
                continue;
            }
        }

        noComments.push_back(c);
    }

    std::string out;
    out.reserve(noComments.size());
    inString = false;
    escaping = false;
    for (size_t i = 0; i < noComments.size(); ++i) {
        char c = noComments[i];
        if (inString) {
            out.push_back(c);
            if (escaping) {
                escaping = false;
            } else if (c == '\\') {
                escaping = true;
            } else if (c == '"') {
                inString = false;
            }
            continue;
        }

        if (c == '"') {
            inString = true;
            out.push_back(c);
            continue;
        }

        if (c == ',') {
            size_t j = i + 1;
            while (j < noComments.size() &&
                   std::isspace(static_cast<unsigned char>(noComments[j])) != 0) {
                ++j;
            }
            if (j < noComments.size() &&
                (noComments[j] == '}' || noComments[j] == ']')) {
                continue;
            }
        }

        out.push_back(c);
    }
    return out;
}

ryml::Tree parseLenientJson(const std::filesystem::path& path, const std::string& text) {
    std::string sanitized = stripJsonCommentsAndTrailingCommas(text);
    return ryml::parse_in_arena(
        ryml::to_csubstr(path.generic_string().c_str()),
        ryml::to_csubstr(sanitized.c_str())
    );
}

std::optional<ryml::ConstNodeRef> getChildNode(ryml::ConstNodeRef primary,
                                                ryml::ConstNodeRef fallback,
                                                const char* key) {
    const ryml::csubstr k = ryml::to_csubstr(key);
    if (primary.readable() && primary.has_child(k)) {
        return primary[k];
    }
    if (fallback.readable() && fallback.has_child(k)) {
        return fallback[k];
    }
    return std::nullopt;
}

std::optional<bool> parseBoolNode(ryml::ConstNodeRef node) {
    if (!node.readable() || !node.has_val()) {
        return std::nullopt;
    }
    std::string value;
    node >> value;
    if (value == "true" || value == "1" || value == "yes") {
        return true;
    }
    if (value == "false" || value == "0" || value == "no") {
        return false;
    }
    return std::nullopt;
}

std::optional<int> parseIntNode(ryml::ConstNodeRef node) {
    if (!node.readable() || !node.has_val()) {
        return std::nullopt;
    }
    std::string value;
    node >> value;
    try {
        return std::stoi(value);
    } catch (...) {
        return std::nullopt;
    }
}

std::vector<std::string> parseStringListNode(ryml::ConstNodeRef node) {
    std::vector<std::string> out;
    if (!node.readable() || !node.is_seq()) {
        return out;
    }
    for (ryml::ConstNodeRef child : node.children()) {
        if (!child.has_val()) {
            continue;
        }
        std::string value;
        child >> value;
        if (!value.empty()) {
            out.push_back(value);
        }
    }
    return out;
}

std::string nodeScalarToString(ryml::ConstNodeRef node) {
    if (!node.readable() || !node.has_val()) {
        return {};
    }
    std::string value;
    node >> value;
    return value;
}

struct CRGeneratorDefinition {
    std::string identifier;
    std::string sourcePath;
    OrderedParams params;
    std::vector<std::string> includes;
    std::optional<std::string> modelName;
    std::optional<bool> isOpaque;
    std::optional<bool> isSolid;
    std::optional<uint8_t> lightAttenuation;
    std::optional<std::string> renderLayer;
    std::vector<std::string> nestedStateGenerators;
    ExtensionMap extensions;
};

bool hasGeneratorConcreteState(const CRGeneratorDefinition& def) {
    return !def.params.ordered.empty() ||
           def.modelName.has_value() ||
           def.isOpaque.has_value() ||
           def.isSolid.has_value() ||
           def.lightAttenuation.has_value() ||
           def.renderLayer.has_value() ||
           !def.extensions.empty();
}

void collectGenerators(const std::filesystem::path& baseRoot,
                       std::unordered_map<std::string, CRGeneratorDefinition>& out,
                       std::vector<ValidationIssue>& diagnostics) {
    std::vector<std::filesystem::path> files;
    collectFilesystemPaths(baseRoot / "block_state_generators", files, {".json"});
    for (const auto& file : files) {
        const std::string sourcePath = relativeOrAbsolute(file, baseRoot);
        std::string text = readFileText(file);
        ryml::Tree tree;
        try {
            tree = parseLenientJson(file, text);
        } catch (const std::exception& e) {
            diagnostics.push_back(ValidationIssue{
                .severity = ValidationSeverity::Warning,
                .sourcePath = sourcePath,
                .identifier = "",
                .field = "block_state_generators",
                .message = std::string("Failed to parse generator file: ") + e.what()
            });
            continue;
        }
        ryml::ConstNodeRef root = tree.rootref();
        if (!root.has_child("generators")) {
            continue;
        }
        ryml::ConstNodeRef generatorsNode = root["generators"];
        if (!generatorsNode.is_seq()) {
            continue;
        }
        for (ryml::ConstNodeRef entry : generatorsNode.children()) {
            if (!entry.is_map()) {
                continue;
            }
            auto idOpt = nodeString(entry, "stringId");
            if (!idOpt || idOpt->empty()) {
                diagnostics.push_back(ValidationIssue{
                    .severity = ValidationSeverity::Warning,
                    .sourcePath = sourcePath,
                    .identifier = "",
                    .field = "generator.stringId",
                    .message = "Generator entry missing stringId"
                });
                continue;
            }

            CRGeneratorDefinition def;
            def.identifier = *idOpt;
            def.sourcePath = sourcePath;

            if (entry.has_child("include")) {
                def.includes = parseStringListNode(entry["include"]);
            }
            if (entry.has_child("params")) {
                def.params = parseParamsNode(entry["params"]);
            }
            if (auto model = nodeString(entry, "modelName")) {
                def.modelName = *model;
            }

            if (entry.has_child("overrides")) {
                ryml::ConstNodeRef overrides = entry["overrides"];
                if (auto model = nodeString(overrides, "modelName")) {
                    def.modelName = *model;
                }
                if (auto node = getChildNode(overrides, ryml::ConstNodeRef(), "isOpaque")) {
                    if (auto parsed = parseBoolNode(*node)) {
                        def.isOpaque = *parsed;
                    }
                }
                if (auto node = getChildNode(overrides, ryml::ConstNodeRef(), "isSolid")) {
                    if (auto parsed = parseBoolNode(*node)) {
                        def.isSolid = *parsed;
                    }
                }
                if (!def.isSolid.has_value()) {
                    if (auto node = getChildNode(overrides, ryml::ConstNodeRef(), "walkThrough")) {
                        if (auto parsed = parseBoolNode(*node)) {
                            def.isSolid = !(*parsed);
                        }
                    }
                }
                if (auto node = getChildNode(overrides, ryml::ConstNodeRef(), "lightAttenuation")) {
                    if (auto parsed = parseIntNode(*node)) {
                        if (*parsed >= 0 && *parsed <= 255) {
                            def.lightAttenuation = static_cast<uint8_t>(*parsed);
                        }
                    }
                }
                if (auto layer = nodeString(overrides, "renderLayer")) {
                    def.renderLayer = *layer;
                }
                if (overrides.has_child("stateGenerators")) {
                    def.nestedStateGenerators = parseStringListNode(overrides["stateGenerators"]);
                }

                static const std::set<std::string> knownOverrideFields = {
                    "modelName", "isOpaque", "isSolid", "walkThrough", "lightAttenuation",
                    "renderLayer", "stateGenerators"
                };
                for (ryml::ConstNodeRef child : overrides.children()) {
                    std::string key(child.key().data(), child.key().size());
                    if (knownOverrideFields.find(key) != knownOverrideFields.end()) {
                        continue;
                    }
                    if (child.has_val()) {
                        std::string value;
                        child >> value;
                        def.extensions["override." + key] = value;
                    }
                }
            }

            auto [it, inserted] = out.emplace(def.identifier, std::move(def));
            if (!inserted) {
                diagnostics.push_back(ValidationIssue{
                    .severity = ValidationSeverity::Warning,
                    .sourcePath = sourcePath,
                    .identifier = *idOpt,
                    .field = "generator.stringId",
                    .message = "Duplicate generator identifier; keeping first definition from " + it->second.sourcePath
                });
            }
        }
    }
}

struct CRStateTemplate {
    BlockStateIR state;
    OrderedParams params;
    std::vector<std::string> stateGenerators;
};

void applyGenerator(const CRGeneratorDefinition& def, CRStateTemplate& state) {
    for (const auto& [key, value] : def.params.ordered) {
        setParam(state.params, key, value);
    }
    if (def.modelName) {
        state.state.model = *def.modelName;
    }
    if (def.isOpaque) {
        state.state.isOpaque = *def.isOpaque;
    }
    if (def.isSolid) {
        state.state.isSolid = *def.isSolid;
    }
    if (def.lightAttenuation) {
        state.state.lightAttenuation = *def.lightAttenuation;
    }
    if (def.renderLayer) {
        state.state.renderLayer = *def.renderLayer;
    }
    for (const auto& [key, value] : def.extensions) {
        state.state.extensions["cr.generator." + key] = value;
    }
    if (!def.nestedStateGenerators.empty()) {
        state.stateGenerators.insert(state.stateGenerators.end(),
                                     def.nestedStateGenerators.begin(),
                                     def.nestedStateGenerators.end());
    }
}

void expandStateWithGenerator(const std::string& generatorId,
                              const CRStateTemplate& input,
                              const std::unordered_map<std::string, CRGeneratorDefinition>& generators,
                              std::vector<CRStateTemplate>& out,
                              std::vector<ValidationIssue>& diagnostics,
                              std::unordered_set<std::string>& recursionStack,
                              const std::string& sourcePath,
                              const std::string& blockId) {
    auto it = generators.find(generatorId);
    if (it == generators.end()) {
        diagnostics.push_back(ValidationIssue{
            .severity = ValidationSeverity::Warning,
            .sourcePath = sourcePath,
            .identifier = blockId,
            .field = "stateGenerators",
            .message = "Unsupported generator '" + generatorId + "'"
        });
        return;
    }

    if (!recursionStack.insert(generatorId).second) {
        diagnostics.push_back(ValidationIssue{
            .severity = ValidationSeverity::Warning,
            .sourcePath = sourcePath,
            .identifier = blockId,
            .field = "stateGenerators",
            .message = "Generator include cycle detected at '" + generatorId + "'"
        });
        return;
    }

    const CRGeneratorDefinition& def = it->second;
    CRStateTemplate current = input;
    applyGenerator(def, current);

    if (hasGeneratorConcreteState(def)) {
        out.push_back(current);
    }
    if (def.includes.empty() && !hasGeneratorConcreteState(def)) {
        diagnostics.push_back(ValidationIssue{
            .severity = ValidationSeverity::Warning,
            .sourcePath = def.sourcePath,
            .identifier = generatorId,
            .field = "include",
            .message = "Generator has no concrete state changes and no include entries"
        });
    }

    for (const std::string& includeId : def.includes) {
        expandStateWithGenerator(includeId,
                                 current,
                                 generators,
                                 out,
                                 diagnostics,
                                 recursionStack,
                                 sourcePath,
                                 blockId);
    }

    recursionStack.erase(generatorId);
}

std::vector<std::string> mergedStateGenerators(ryml::ConstNodeRef defaultProps,
                                               ryml::ConstNodeRef stateProps) {
    std::vector<std::string> out;
    if (defaultProps.readable() && defaultProps.has_child("stateGenerators")) {
        auto defaults = parseStringListNode(defaultProps["stateGenerators"]);
        out.insert(out.end(), defaults.begin(), defaults.end());
    }
    if (stateProps.readable() && stateProps.has_child("stateGenerators")) {
        auto state = parseStringListNode(stateProps["stateGenerators"]);
        out.insert(out.end(), state.begin(), state.end());
    }
    return out;
}

void registerExpandedState(const std::string& rootId,
                           const std::string& sourcePath,
                           CRStateTemplate candidate,
                           std::unordered_map<std::string, BlockStateIR>& canonicalStates,
                           std::vector<IdentifierAliasIR>& aliases,
                           std::vector<ValidationIssue>& diagnostics) {
    const std::string canonicalParams = paramsToString(candidate.params, true);
    const std::string externalParams = paramsToString(candidate.params, false);
    const std::string canonicalId = withParams(rootId, canonicalParams);
    const std::string externalId = withParams(rootId, externalParams);
    candidate.state.identifier = canonicalId;
    candidate.state.rootIdentifier = rootId;
    candidate.state.sourcePath = sourcePath;
    candidate.state.model = normalizeModelReference(candidate.state.model);
    for (auto& [_, textureRef] : candidate.state.textures) {
        textureRef = normalizeAssetReference(textureRef);
    }
    candidate.state.extensions["source_format"] = "cr";
    candidate.state.extensions["cr.external_identifier"] = externalId;
    candidate.state.renderLayer = normalizeRenderLayer(candidate.state.renderLayer, candidate.state.isOpaque);

    auto it = canonicalStates.find(canonicalId);
    if (it != canonicalStates.end()) {
        const bool same = it->second.model == candidate.state.model &&
                          it->second.renderLayer == candidate.state.renderLayer &&
                          it->second.isOpaque == candidate.state.isOpaque &&
                          it->second.isSolid == candidate.state.isSolid &&
                          it->second.lightAttenuation == candidate.state.lightAttenuation;
        if (!same) {
            diagnostics.push_back(ValidationIssue{
                .severity = ValidationSeverity::Warning,
                .sourcePath = sourcePath,
                .identifier = canonicalId,
                .field = "blockStates/stateGenerators",
                .message = "Conflicting overrides collapsed into the same canonical state identifier"
            });
        }
        return;
    }
    canonicalStates.emplace(canonicalId, std::move(candidate.state));

    if (canonicalId != externalId) {
        aliases.push_back(IdentifierAliasIR{
            .domain = "block",
            .canonicalIdentifier = canonicalId,
            .externalIdentifier = externalId,
            .sourcePath = sourcePath
        });
    }
}

} // namespace

AssetGraphIR compileRigelEmbedded() {
    AssetGraphIR graph;
    std::string ns = rigelManifestNamespace();

    std::vector<std::string> blockPaths;
    for (std::string_view path : ResourceRegistry::Paths()) {
        if (startsWith(path, "blocks/") && endsWith(path, ".yaml")) {
            blockPaths.emplace_back(path);
        }
    }
    std::sort(blockPaths.begin(), blockPaths.end());

    std::unordered_map<std::string, size_t> rootToIndex;
    for (const std::string& path : blockPaths) {
        auto data = ResourceRegistry::Get(path);
        ryml::Tree tree = ryml::parse_in_arena(
            ryml::to_csubstr(path.c_str()),
            ryml::csubstr(data.data(), data.size())
        );
        ryml::ConstNodeRef root = tree.rootref();

        BlockStateIR state;
        state.identifier = blockIdFromEmbeddedPath(path, root, ns);
        state.rootIdentifier = stripVariantSuffix(state.identifier);
        state.sourcePath = path;

        if (auto model = nodeString(root, "model")) {
            state.model = *model;
        }
        if (auto layer = nodeString(root, "layer")) {
            state.renderLayer = *layer;
        }
        if (auto opaque = nodeBool(root, "opaque")) {
            state.isOpaque = *opaque;
        }
        if (auto solid = nodeBool(root, "solid")) {
            state.isSolid = *solid;
        }
        if (auto cullSameType = nodeBool(root, "cull_same_type")) {
            state.cullSameType = *cullSameType;
        }
        if (auto emitted = nodeU8(root, "emits_light")) {
            state.emittedLight = *emitted;
        }
        if (auto attenuation = nodeU8(root, "light_attenuation")) {
            state.lightAttenuation = *attenuation;
        }

        static const std::set<std::string> knownRootFields = {
            "id", "identifier", "name", "model", "layer", "opaque", "solid",
            "cull_same_type", "emits_light", "light_attenuation", "textures"
        };
        for (ryml::ConstNodeRef child : root.children()) {
            std::string key(child.key().data(), child.key().size());
            if (knownRootFields.find(key) != knownRootFields.end()) {
                continue;
            }
            if (child.has_val()) {
                std::string value;
                child >> value;
                state.extensions[key] = value;
            }
        }

        if (root.has_child("textures")) {
            ryml::ConstNodeRef textures = root["textures"];
            for (ryml::ConstNodeRef texNode : textures.children()) {
                std::string key(texNode.key().data(), texNode.key().size());
                std::string value;
                texNode >> value;
                state.textures[key] = normalizeAssetReference(value);
            }
        }

        state.model = normalizeModelReference(state.model);
        state.renderLayer = normalizeRenderLayer(state.renderLayer, state.isOpaque);

        auto it = rootToIndex.find(state.rootIdentifier);
        if (it == rootToIndex.end()) {
            BlockDefIR blockDef;
            blockDef.rootIdentifier = state.rootIdentifier;
            blockDef.sourcePath = state.sourcePath;
            blockDef.states.push_back(std::move(state));
            graph.blocks.push_back(std::move(blockDef));
            rootToIndex.emplace(graph.blocks.back().rootIdentifier, graph.blocks.size() - 1);
        } else {
            graph.blocks[it->second].states.push_back(std::move(state));
        }
    }

    for (auto& block : graph.blocks) {
        std::sort(block.states.begin(), block.states.end(), [](const BlockStateIR& a, const BlockStateIR& b) {
            return a.identifier < b.identifier;
        });
    }
    std::sort(graph.blocks.begin(), graph.blocks.end(), [](const BlockDefIR& a, const BlockDefIR& b) {
        return a.rootIdentifier < b.rootIdentifier;
    });

    for (std::string_view path : ResourceRegistry::Paths()) {
        if (startsWith(path, "models/entities/") && hasAnySuffix(path, {".json", ".yaml", ".yml"})) {
            graph.entities.push_back(EntityDefIR{std::string(path), std::string(path), {}});
        } else if (startsWith(path, "models/") && hasAnySuffix(path, {".json", ".yaml", ".yml"})) {
            graph.models.push_back(ModelRefIR{std::string(path), std::string(path), {}});
        } else if (startsWith(path, "materials/") && hasAnySuffix(path, {".json", ".yaml", ".yml"})) {
            graph.materials.push_back(MaterialRefIR{std::string(path), std::string(path), {}});
        } else if (startsWith(path, "textures/") &&
                   hasAnySuffix(path, {".png", ".bmp", ".jpg", ".jpeg", ".tga"})) {
            graph.textures.push_back(TextureRefIR{std::string(path), std::string(path), {}});
        } else if (startsWith(path, "items/") && hasAnySuffix(path, {".json", ".yaml", ".yml"})) {
            graph.items.push_back(ItemDefIR{std::string(path), std::string(path), {}});
        }
    }
    sortByIdentifier(graph.models);
    sortByIdentifier(graph.materials);
    sortByIdentifier(graph.textures);
    sortByIdentifier(graph.entities);
    sortByIdentifier(graph.items);

    return graph;
}

AssetGraphIR compileCRFilesystem(const std::filesystem::path& root) {
    AssetGraphIR graph;

    std::filesystem::path baseRoot = root;
    if (std::filesystem::exists(root / "base")) {
        baseRoot = root / "base";
    }

    std::unordered_map<std::string, CRGeneratorDefinition> generators;
    collectGenerators(baseRoot, generators, graph.compilerDiagnostics);

    std::vector<std::filesystem::path> blockFiles;
    collectFilesystemPaths(baseRoot / "blocks", blockFiles, {".json"});
    for (const auto& file : blockFiles) {
        const std::string sourcePath = relativeOrAbsolute(file, baseRoot);
        std::string text;
        try {
            text = readFileText(file);
        } catch (const std::exception& e) {
            graph.compilerDiagnostics.push_back(ValidationIssue{
                .severity = ValidationSeverity::Warning,
                .sourcePath = sourcePath,
                .identifier = "",
                .field = "read",
                .message = std::string("Failed to read block file: ") + e.what()
            });
            continue;
        }
        ryml::Tree tree;
        try {
            tree = parseLenientJson(file, text);
        } catch (const std::exception& e) {
            graph.compilerDiagnostics.push_back(ValidationIssue{
                .severity = ValidationSeverity::Warning,
                .sourcePath = sourcePath,
                .identifier = "",
                .field = "parse",
                .message = std::string("Failed to parse block file: ") + e.what()
            });
            continue;
        }

        ryml::ConstNodeRef rootNode = tree.rootref();
        std::string rootId = nodeString(rootNode, "stringId").value_or("");
        if (rootId.empty()) {
            rootId = "base:" + file.stem().string();
            graph.compilerDiagnostics.push_back(ValidationIssue{
                .severity = ValidationSeverity::Warning,
                .sourcePath = sourcePath,
                .identifier = rootId,
                .field = "stringId",
                .message = "Block missing stringId; using filename fallback"
            });
        }

        ryml::ConstNodeRef defaultParamsNode =
            rootNode.has_child("defaultParams") ? rootNode["defaultParams"] : ryml::ConstNodeRef();
        ryml::ConstNodeRef defaultPropsNode =
            rootNode.has_child("defaultProperties") ? rootNode["defaultProperties"] : ryml::ConstNodeRef();

        OrderedParams defaultParams = parseParamsNode(defaultParamsNode);

        BlockDefIR blockDef;
        blockDef.rootIdentifier = rootId;
        blockDef.sourcePath = sourcePath;
        blockDef.extensions["source_format"] = "cr";

        std::unordered_map<std::string, BlockStateIR> canonicalStates;
        std::vector<std::pair<std::string, ryml::ConstNodeRef>> stateEntries;

        if (rootNode.has_child("blockStates")) {
            ryml::ConstNodeRef blockStatesNode = rootNode["blockStates"];
            for (ryml::ConstNodeRef stateNode : blockStatesNode.children()) {
                std::string stateKey(stateNode.key().data(), stateNode.key().size());
                stateEntries.emplace_back(stateKey, stateNode);
            }
            std::sort(stateEntries.begin(),
                      stateEntries.end(),
                      [](const auto& a, const auto& b) { return a.first < b.first; });
        } else {
            graph.compilerDiagnostics.push_back(ValidationIssue{
                .severity = ValidationSeverity::Warning,
                .sourcePath = sourcePath,
                .identifier = rootId,
                .field = "blockStates",
                .message = "Block has no blockStates map; generating implicit default state"
            });
            stateEntries.emplace_back("", ryml::ConstNodeRef());
        }

        for (const auto& [stateKey, stateNode] : stateEntries) {
            OrderedParams params = defaultParams;
            OrderedParams parsedStateParams = parseParamString(stateKey);
            for (const auto& [key, value] : parsedStateParams.ordered) {
                setParam(params, key, value);
            }

            BlockStateIR state;
            state.rootIdentifier = rootId;
            state.sourcePath = sourcePath;
            state.model = "cube";
            state.renderLayer = "opaque";
            state.isOpaque = true;
            state.isSolid = true;
            state.cullSameType = false;
            state.emittedLight = 0;
            state.lightAttenuation = 15;
            state.extensions["source_format"] = "cr";

            if (auto model = getChildNode(stateNode, defaultPropsNode, "modelName")) {
                std::string modelName = nodeScalarToString(*model);
                if (!modelName.empty()) {
                    state.model = modelName;
                }
            }
            if (auto layer = getChildNode(stateNode, defaultPropsNode, "renderLayer")) {
                std::string renderLayer = nodeScalarToString(*layer);
                if (!renderLayer.empty()) {
                    state.renderLayer = renderLayer;
                }
            }
            if (auto opaque = getChildNode(stateNode, defaultPropsNode, "isOpaque")) {
                if (auto parsed = parseBoolNode(*opaque)) {
                    state.isOpaque = *parsed;
                }
            }
            if (auto solid = getChildNode(stateNode, defaultPropsNode, "isSolid")) {
                if (auto parsed = parseBoolNode(*solid)) {
                    state.isSolid = *parsed;
                }
            } else if (auto walkThrough = getChildNode(stateNode, defaultPropsNode, "walkThrough")) {
                if (auto parsed = parseBoolNode(*walkThrough)) {
                    state.isSolid = !(*parsed);
                }
            }
            if (auto attenuation = getChildNode(stateNode, defaultPropsNode, "lightAttenuation")) {
                if (auto parsed = parseIntNode(*attenuation)) {
                    if (*parsed >= 0 && *parsed <= 255) {
                        state.lightAttenuation = static_cast<uint8_t>(*parsed);
                    }
                }
            }
            if (!state.isOpaque && state.renderLayer == "opaque") {
                state.renderLayer = "transparent";
            }

            CRStateTemplate baseTemplate;
            baseTemplate.state = state;
            baseTemplate.params = params;
            baseTemplate.stateGenerators = mergedStateGenerators(defaultPropsNode, stateNode);

            registerExpandedState(rootId,
                                  sourcePath,
                                  baseTemplate,
                                  canonicalStates,
                                  graph.aliases,
                                  graph.compilerDiagnostics);

            for (const std::string& generatorId : baseTemplate.stateGenerators) {
                std::vector<CRStateTemplate> expanded;
                std::unordered_set<std::string> recursionStack;
                expandStateWithGenerator(generatorId,
                                         baseTemplate,
                                         generators,
                                         expanded,
                                         graph.compilerDiagnostics,
                                         recursionStack,
                                         sourcePath,
                                         rootId);
                for (auto& variant : expanded) {
                    registerExpandedState(rootId,
                                          sourcePath,
                                          std::move(variant),
                                          canonicalStates,
                                          graph.aliases,
                                          graph.compilerDiagnostics);
                }
            }
        }

        for (auto& [id, state] : canonicalStates) {
            blockDef.states.push_back(std::move(state));
        }
        std::sort(blockDef.states.begin(), blockDef.states.end(),
                  [](const BlockStateIR& a, const BlockStateIR& b) {
                      return a.identifier < b.identifier;
                  });
        graph.blocks.push_back(std::move(blockDef));
    }

    std::sort(graph.blocks.begin(), graph.blocks.end(), [](const BlockDefIR& a, const BlockDefIR& b) {
        return a.rootIdentifier < b.rootIdentifier;
    });
    std::sort(graph.aliases.begin(), graph.aliases.end(), [](const IdentifierAliasIR& a, const IdentifierAliasIR& b) {
        if (a.domain != b.domain) {
            return a.domain < b.domain;
        }
        if (a.canonicalIdentifier != b.canonicalIdentifier) {
            return a.canonicalIdentifier < b.canonicalIdentifier;
        }
        if (a.externalIdentifier != b.externalIdentifier) {
            return a.externalIdentifier < b.externalIdentifier;
        }
        return a.sourcePath < b.sourcePath;
    });
    std::sort(graph.compilerDiagnostics.begin(),
              graph.compilerDiagnostics.end(),
              [](const ValidationIssue& a, const ValidationIssue& b) {
                  if (a.sourcePath != b.sourcePath) {
                      return a.sourcePath < b.sourcePath;
                  }
                  if (a.identifier != b.identifier) {
                      return a.identifier < b.identifier;
                  }
                  if (a.field != b.field) {
                      return a.field < b.field;
                  }
                  return a.message < b.message;
              });

    std::vector<std::filesystem::path> files;
    collectFilesystemPaths(baseRoot / "models", files, {".json", ".yaml", ".yml"});
    for (const auto& file : files) {
        std::string id = relativeOrAbsolute(file, baseRoot);
        graph.models.push_back(ModelRefIR{id, id, {{"source_format", "cr"}}});
    }
    collectFilesystemPaths(baseRoot / "materials", files, {".json", ".yaml", ".yml"});
    for (const auto& file : files) {
        std::string id = relativeOrAbsolute(file, baseRoot);
        graph.materials.push_back(MaterialRefIR{id, id, {{"source_format", "cr"}}});
    }
    collectFilesystemPaths(baseRoot / "textures", files, {".png", ".bmp", ".jpg", ".jpeg", ".tga"});
    for (const auto& file : files) {
        std::string id = relativeOrAbsolute(file, baseRoot);
        graph.textures.push_back(TextureRefIR{id, id, {{"source_format", "cr"}}});
    }
    collectFilesystemPaths(baseRoot / "entities", files, {".json", ".yaml", ".yml"});
    for (const auto& file : files) {
        std::string id = relativeOrAbsolute(file, baseRoot);
        graph.entities.push_back(EntityDefIR{id, id, {{"source_format", "cr"}}});
    }
    collectFilesystemPaths(baseRoot / "items", files, {".json", ".yaml", ".yml"});
    for (const auto& file : files) {
        std::string id = relativeOrAbsolute(file, baseRoot);
        graph.items.push_back(ItemDefIR{id, id, {{"source_format", "cr"}}});
    }
    sortByIdentifier(graph.models);
    sortByIdentifier(graph.materials);
    sortByIdentifier(graph.textures);
    sortByIdentifier(graph.entities);
    sortByIdentifier(graph.items);

    return graph;
}

std::vector<ValidationIssue> validate(const AssetGraphIR& graph) {
    std::vector<ValidationIssue> issues = graph.compilerDiagnostics;

    auto add = [&](ValidationSeverity severity,
                   std::string sourcePath,
                   std::string identifier,
                   std::string field,
                   std::string message) {
        issues.push_back(ValidationIssue{
            .severity = severity,
            .sourcePath = std::move(sourcePath),
            .identifier = std::move(identifier),
            .field = std::move(field),
            .message = std::move(message)
        });
    };

    std::unordered_map<std::string, std::string> blockVariantOwners;
    for (const auto& block : graph.blocks) {
        if (block.rootIdentifier.empty()) {
            add(ValidationSeverity::Error, block.sourcePath, "", "rootIdentifier",
                "Block root identifier is empty");
        }
        if (block.states.empty()) {
            add(ValidationSeverity::Warning, block.sourcePath, block.rootIdentifier, "states",
                "Block definition has no states");
        }
        for (const auto& state : block.states) {
            if (state.identifier.empty()) {
                add(ValidationSeverity::Error, state.sourcePath, "", "identifier",
                    "Block state identifier is empty");
                continue;
            }
            auto [it, inserted] = blockVariantOwners.emplace(state.identifier, state.sourcePath);
            if (!inserted) {
                add(ValidationSeverity::Error, state.sourcePath, state.identifier, "identifier",
                    "Duplicate block state identifier also seen in " + it->second);
            }
            std::string expectedRoot = stripVariantSuffix(state.identifier);
            if (expectedRoot != state.rootIdentifier) {
                add(ValidationSeverity::Error, state.sourcePath, state.identifier, "rootIdentifier",
                    "State root '" + state.rootIdentifier + "' does not match identifier-derived root '" +
                        expectedRoot + "'");
            }
            if (state.renderLayer != "opaque" && state.renderLayer != "cutout" &&
                state.renderLayer != "transparent" && state.renderLayer != "emissive") {
                add(ValidationSeverity::Warning, state.sourcePath, state.identifier, "renderLayer",
                    "Unknown render layer '" + state.renderLayer + "'");
            }
            if (state.model == "cube" && state.textures.empty()) {
                add(ValidationSeverity::Warning, state.sourcePath, state.identifier, "textures",
                    "Cube model has no textures configured");
            }
        }
    }

    auto checkDuplicateSimpleIds = [&](const auto& values, const char* fieldName) {
        std::unordered_map<std::string, std::string> owners;
        for (const auto& value : values) {
            if (value.identifier.empty()) {
                add(ValidationSeverity::Error, value.sourcePath, "", fieldName, "Identifier is empty");
                continue;
            }
            auto [it, inserted] = owners.emplace(value.identifier, value.sourcePath);
            if (!inserted) {
                add(ValidationSeverity::Error, value.sourcePath, value.identifier, fieldName,
                    std::string("Duplicate identifier also seen in ") + it->second);
            }
        }
    };

    checkDuplicateSimpleIds(graph.models, "model.identifier");
    checkDuplicateSimpleIds(graph.materials, "material.identifier");
    checkDuplicateSimpleIds(graph.textures, "texture.identifier");
    checkDuplicateSimpleIds(graph.entities, "entity.identifier");
    checkDuplicateSimpleIds(graph.items, "item.identifier");

    std::unordered_set<std::string> availableModels;
    availableModels.reserve(graph.models.size());
    for (const auto& model : graph.models) {
        availableModels.insert(normalizeAssetReference(model.identifier));
    }

    std::unordered_set<std::string> availableTextures;
    availableTextures.reserve(graph.textures.size());
    for (const auto& texture : graph.textures) {
        availableTextures.insert(normalizeAssetReference(texture.identifier));
    }

    for (const auto& block : graph.blocks) {
        for (const auto& state : block.states) {
            const std::string normalizedModel = normalizeModelReference(state.model);
            if (!normalizedModel.empty() &&
                !isBuiltinBlockModel(normalizedModel) &&
                availableModels.find(normalizedModel) == availableModels.end()) {
                add(ValidationSeverity::Error,
                    state.sourcePath,
                    state.identifier,
                    "model",
                    "Unresolved model reference '" + state.model +
                        "' (normalized to '" + normalizedModel + "')");
            }

            if (state.renderLayer == "opaque" && !state.isOpaque) {
                add(ValidationSeverity::Warning,
                    state.sourcePath,
                    state.identifier,
                    "renderLayer",
                    "Opaque render layer with non-opaque block flags may cause unexpected sorting");
            }
            if ((state.renderLayer == "transparent" || state.renderLayer == "cutout") && state.isOpaque) {
                add(ValidationSeverity::Warning,
                    state.sourcePath,
                    state.identifier,
                    "renderLayer",
                    "Transparent/cutout render layer with opaque block flags may cause culling mismatches");
            }

            for (const auto& [face, textureRefRaw] : state.textures) {
                const std::string textureRef = normalizeAssetReference(textureRefRaw);
                if (textureRef.empty()) {
                    continue;
                }
                if (availableTextures.find(textureRef) == availableTextures.end()) {
                    add(ValidationSeverity::Warning,
                        state.sourcePath,
                        state.identifier,
                        "textures." + face,
                        "Unresolved texture reference '" + textureRefRaw +
                            "' (normalized to '" + textureRef + "')");
                }
            }
        }
    }

    std::unordered_map<std::string, std::string> externalToCanonical;
    std::unordered_map<std::string, std::string> canonicalToExternal;
    for (const auto& alias : graph.aliases) {
        if (alias.domain.empty()) {
            add(ValidationSeverity::Error,
                alias.sourcePath,
                alias.canonicalIdentifier,
                "aliases.domain",
                "Alias domain is empty");
            continue;
        }
        if (alias.canonicalIdentifier.empty() || alias.externalIdentifier.empty()) {
            add(ValidationSeverity::Error,
                alias.sourcePath,
                alias.canonicalIdentifier,
                "aliases.identifier",
                "Alias canonical/external identifier cannot be empty");
            continue;
        }

        const std::string externalKey = alias.domain + "|" + alias.externalIdentifier;
        const std::string canonicalKey = alias.domain + "|" + alias.canonicalIdentifier;
        auto [extIt, extInserted] = externalToCanonical.emplace(externalKey, alias.canonicalIdentifier);
        if (!extInserted && extIt->second != alias.canonicalIdentifier) {
            add(ValidationSeverity::Error,
                alias.sourcePath,
                alias.externalIdentifier,
                "aliases.external",
                "External identifier maps to multiple canonical identifiers");
        }
        auto [canonIt, canonInserted] = canonicalToExternal.emplace(canonicalKey, alias.externalIdentifier);
        if (!canonInserted && canonIt->second != alias.externalIdentifier) {
            add(ValidationSeverity::Error,
                alias.sourcePath,
                alias.canonicalIdentifier,
                "aliases.canonical",
                "Canonical identifier maps to multiple external identifiers");
        }
    }

    return issues;
}

} // namespace Rigel::Asset::IR
