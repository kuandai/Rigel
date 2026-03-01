#include "Rigel/Asset/AssetAudit.h"
#include "Rigel/Util/Ryml.h"
#include "ResourceRegistry.h"

#include <ryml.hpp>
#include <ryml_std.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

namespace Rigel::Asset {

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

void sortUnique(std::vector<std::string>& values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
}

std::string toGenericPath(const std::filesystem::path& path) {
    return path.generic_string();
}

std::string removeYamlSuffix(std::string_view value) {
    if (endsWith(value, ".yaml")) {
        return std::string(value.substr(0, value.size() - 5));
    }
    if (endsWith(value, ".yml")) {
        return std::string(value.substr(0, value.size() - 4));
    }
    return std::string(value);
}

std::string stripVariantSuffix(const std::string& id) {
    size_t bracketPos = id.find('[');
    if (bracketPos == std::string::npos) {
        return id;
    }
    return id.substr(0, bracketPos);
}

std::string escapeJson(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        switch (c) {
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
            out += c;
            break;
        }
    }
    return out;
}

void writeJsonArray(std::ostringstream& out, const std::vector<std::string>& values, int indent) {
    out << "[";
    if (!values.empty()) {
        out << "\n";
        for (size_t i = 0; i < values.size(); ++i) {
            out << std::string(static_cast<size_t>(indent), ' ')
                << "\"" << escapeJson(values[i]) << "\"";
            if (i + 1 < values.size()) {
                out << ",";
            }
            out << "\n";
        }
        out << std::string(static_cast<size_t>(indent - 2), ' ');
    }
    out << "]";
}

AssetAuditCategoryDiff diffCategory(const std::vector<std::string>& left,
                                    const std::vector<std::string>& right) {
    AssetAuditCategoryDiff diff;
    std::set_difference(left.begin(), left.end(),
                        right.begin(), right.end(),
                        std::back_inserter(diff.onlyInLeft));
    std::set_difference(right.begin(), right.end(),
                        left.begin(), left.end(),
                        std::back_inserter(diff.onlyInRight));
    return diff;
}

void normalizeInventory(AssetAuditInventory& inventory) {
    sortUnique(inventory.blockRoots);
    sortUnique(inventory.blockVariants);
    sortUnique(inventory.modelRefs);
    sortUnique(inventory.textureRefs);
    sortUnique(inventory.entityDefs);
    sortUnique(inventory.itemDefs);
    sortUnique(inventory.duplicateBlockVariants);
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
        // Fallback below.
    }
    return "base";
}

std::string rigelBlockIdFromPath(const std::string& path, const std::string& manifestNs) {
    auto data = ResourceRegistry::Get(path);
    ryml::Tree tree = ryml::parse_in_arena(
        ryml::to_csubstr(path.c_str()),
        ryml::csubstr(data.data(), data.size())
    );
    ryml::ConstNodeRef root = tree.rootref();

    std::string id;
    if (auto explicitId = nodeString(root, "id")) {
        id = *explicitId;
    } else if (auto explicitIdentifier = nodeString(root, "identifier")) {
        id = *explicitIdentifier;
    } else if (auto explicitName = nodeString(root, "name")) {
        id = *explicitName;
    } else {
        std::string_view view(path);
        if (startsWith(view, "blocks/")) {
            view.remove_prefix(std::string_view("blocks/").size());
        }
        id = removeYamlSuffix(view);
    }

    if (id.find(':') == std::string::npos && !manifestNs.empty()) {
        id = manifestNs + ":" + id;
    }

    return id;
}

std::string readFileText(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("Failed to open file: " + toGenericPath(path));
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

void collectPaths(const std::filesystem::path& root,
                  std::vector<std::filesystem::path>& out,
                  std::initializer_list<std::string_view> suffixes) {
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
    std::filesystem::path relative = std::filesystem::relative(path, base, ec);
    if (!ec && !relative.empty() && relative.native().front() != '.') {
        return toGenericPath(relative);
    }
    return toGenericPath(path);
}

} // namespace

AssetAuditInventory RigelEmbeddedAuditSource::collect() const {
    AssetAuditInventory inventory;
    inventory.source = "rigel:embedded";

    std::string manifestNs = rigelManifestNamespace();
    std::unordered_set<std::string> seenVariantIds;

    for (std::string_view pathView : ResourceRegistry::Paths()) {
        std::string path(pathView);

        if (startsWith(path, "blocks/") && endsWith(path, ".yaml")) {
            try {
                std::string id = rigelBlockIdFromPath(path, manifestNs);
                if (!seenVariantIds.insert(id).second) {
                    inventory.duplicateBlockVariants.push_back(id);
                }
                inventory.blockVariants.push_back(id);
                inventory.blockRoots.push_back(stripVariantSuffix(id));
            } catch (const std::exception& e) {
                spdlog::warn("AssetAudit: failed to parse block '{}': {}", path, e.what());
            }
            continue;
        }

        if (startsWith(path, "models/") &&
            hasAnySuffix(path, {".json", ".yaml", ".yml"})) {
            inventory.modelRefs.push_back(path);
        }

        if (startsWith(path, "textures/") &&
            hasAnySuffix(path, {".png", ".bmp", ".jpg", ".jpeg"})) {
            inventory.textureRefs.push_back(path);
        }

        if (startsWith(path, "models/entities/") &&
            hasAnySuffix(path, {".json", ".yaml", ".yml"})) {
            inventory.entityDefs.push_back(path);
        }

        if (startsWith(path, "items/") &&
            hasAnySuffix(path, {".json", ".yaml", ".yml"})) {
            inventory.itemDefs.push_back(path);
        }
    }

    normalizeInventory(inventory);
    return inventory;
}

CRFilesystemAuditSource::CRFilesystemAuditSource(std::filesystem::path root)
    : m_root(std::move(root)) {
}

AssetAuditInventory CRFilesystemAuditSource::collect() const {
    if (m_root.empty()) {
        throw std::runtime_error("CRFilesystemAuditSource: root path is empty");
    }

    AssetAuditInventory inventory;
    inventory.source = "cr:filesystem";

    std::filesystem::path baseRoot = m_root;
    if (std::filesystem::exists(m_root / "base")) {
        baseRoot = m_root / "base";
    }

    if (!std::filesystem::exists(baseRoot)) {
        throw std::runtime_error("CRFilesystemAuditSource: path not found: " + toGenericPath(baseRoot));
    }

    std::unordered_set<std::string> seenVariantIds;

    std::vector<std::filesystem::path> blockFiles;
    collectPaths(baseRoot / "blocks", blockFiles, {".json"});
    for (const auto& file : blockFiles) {
        std::string text;
        try {
            text = readFileText(file);
        } catch (const std::exception& e) {
            spdlog::warn("AssetAudit: failed to read '{}': {}", toGenericPath(file), e.what());
            continue;
        }

        auto ids = extractStringIds(text);
        if (ids.empty()) {
            std::string fallback = file.stem().string();
            ids.push_back("base:" + fallback);
        }
        for (const auto& id : ids) {
            if (!seenVariantIds.insert(id).second) {
                inventory.duplicateBlockVariants.push_back(id);
            }
            inventory.blockVariants.push_back(id);
            inventory.blockRoots.push_back(stripVariantSuffix(id));
        }
    }

    std::vector<std::filesystem::path> modelFiles;
    collectPaths(baseRoot / "models", modelFiles, {".json", ".yaml", ".yml"});
    for (const auto& file : modelFiles) {
        inventory.modelRefs.push_back(relativeOrAbsolute(file, baseRoot));
    }

    std::vector<std::filesystem::path> textureFiles;
    collectPaths(baseRoot / "textures", textureFiles, {".png", ".bmp", ".jpg", ".jpeg"});
    for (const auto& file : textureFiles) {
        inventory.textureRefs.push_back(relativeOrAbsolute(file, baseRoot));
    }

    std::vector<std::filesystem::path> entityFiles;
    collectPaths(baseRoot / "entities", entityFiles, {".json", ".yaml", ".yml"});
    for (const auto& file : entityFiles) {
        inventory.entityDefs.push_back(relativeOrAbsolute(file, baseRoot));
    }

    std::vector<std::filesystem::path> itemFiles;
    collectPaths(baseRoot / "items", itemFiles, {".json", ".yaml", ".yml"});
    for (const auto& file : itemFiles) {
        inventory.itemDefs.push_back(relativeOrAbsolute(file, baseRoot));
    }

    normalizeInventory(inventory);
    return inventory;
}

AssetAuditDiff diffInventories(AssetAuditInventory left, AssetAuditInventory right) {
    normalizeInventory(left);
    normalizeInventory(right);

    AssetAuditDiff diff;
    diff.left = std::move(left);
    diff.right = std::move(right);

    diff.blockRoots = diffCategory(diff.left.blockRoots, diff.right.blockRoots);
    diff.blockVariants = diffCategory(diff.left.blockVariants, diff.right.blockVariants);
    diff.modelRefs = diffCategory(diff.left.modelRefs, diff.right.modelRefs);
    diff.textureRefs = diffCategory(diff.left.textureRefs, diff.right.textureRefs);
    diff.entityDefs = diffCategory(diff.left.entityDefs, diff.right.entityDefs);
    diff.itemDefs = diffCategory(diff.left.itemDefs, diff.right.itemDefs);

    return diff;
}

std::string toJson(const AssetAuditDiff& diff) {
    auto writeInventory = [](std::ostringstream& out, const char* key, const AssetAuditInventory& inv) {
        out << "  \"" << key << "\": {\n";
        out << "    \"source\": \"" << escapeJson(inv.source) << "\",\n";
        out << "    \"blockRoots\": ";
        writeJsonArray(out, inv.blockRoots, 6);
        out << ",\n";
        out << "    \"blockVariants\": ";
        writeJsonArray(out, inv.blockVariants, 6);
        out << ",\n";
        out << "    \"modelRefs\": ";
        writeJsonArray(out, inv.modelRefs, 6);
        out << ",\n";
        out << "    \"textureRefs\": ";
        writeJsonArray(out, inv.textureRefs, 6);
        out << ",\n";
        out << "    \"entityDefs\": ";
        writeJsonArray(out, inv.entityDefs, 6);
        out << ",\n";
        out << "    \"itemDefs\": ";
        writeJsonArray(out, inv.itemDefs, 6);
        out << ",\n";
        out << "    \"duplicateBlockVariants\": ";
        writeJsonArray(out, inv.duplicateBlockVariants, 6);
        out << "\n";
        out << "  }";
    };

    auto writeCategoryDiff = [](std::ostringstream& out, const char* key, const AssetAuditCategoryDiff& category) {
        out << "    \"" << key << "\": {\n";
        out << "      \"onlyInLeft\": ";
        writeJsonArray(out, category.onlyInLeft, 8);
        out << ",\n";
        out << "      \"onlyInRight\": ";
        writeJsonArray(out, category.onlyInRight, 8);
        out << "\n";
        out << "    }";
    };

    std::ostringstream out;
    out << "{\n";
    writeInventory(out, "left", diff.left);
    out << ",\n";
    writeInventory(out, "right", diff.right);
    out << ",\n";
    out << "  \"diff\": {\n";
    writeCategoryDiff(out, "blockRoots", diff.blockRoots);
    out << ",\n";
    writeCategoryDiff(out, "blockVariants", diff.blockVariants);
    out << ",\n";
    writeCategoryDiff(out, "modelRefs", diff.modelRefs);
    out << ",\n";
    writeCategoryDiff(out, "textureRefs", diff.textureRefs);
    out << ",\n";
    writeCategoryDiff(out, "entityDefs", diff.entityDefs);
    out << ",\n";
    writeCategoryDiff(out, "itemDefs", diff.itemDefs);
    out << "\n";
    out << "  }\n";
    out << "}\n";
    return out.str();
}

int runAssetAuditTool(const std::filesystem::path& crRoot,
                      const std::optional<std::filesystem::path>& outputPath) {
    RigelEmbeddedAuditSource rigelSource;
    CRFilesystemAuditSource crSource(crRoot);

    AssetAuditInventory left = rigelSource.collect();
    AssetAuditInventory right = crSource.collect();
    AssetAuditDiff diff = diffInventories(std::move(left), std::move(right));
    std::string report = toJson(diff);

    if (outputPath) {
        std::filesystem::path parent = outputPath->parent_path();
        if (!parent.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(parent, ec);
            if (ec) {
                spdlog::error("Asset audit failed to create output directory '{}': {}",
                              toGenericPath(parent), ec.message());
                return 2;
            }
        }
        std::ofstream out(*outputPath, std::ios::binary);
        if (!out) {
            spdlog::error("Asset audit failed to write report: {}", toGenericPath(*outputPath));
            return 2;
        }
        out << report;
        out.flush();
        spdlog::info("Asset audit report written to {}", toGenericPath(*outputPath));
    } else {
        std::fwrite(report.data(), 1, report.size(), stdout);
    }

    spdlog::info("Asset audit complete: Rigel block roots={}, CR block roots={}",
                 diff.left.blockRoots.size(),
                 diff.right.blockRoots.size());
    return 0;
}

} // namespace Rigel::Asset
