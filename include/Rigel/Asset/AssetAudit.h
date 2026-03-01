#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Rigel::Asset {

struct AssetAuditInventory {
    std::string source;

    std::vector<std::string> blockRoots;
    std::vector<std::string> blockVariants;
    std::vector<std::string> modelRefs;
    std::vector<std::string> textureRefs;
    std::vector<std::string> entityDefs;
    std::vector<std::string> itemDefs;

    std::vector<std::string> duplicateBlockVariants;
};

class IAssetAuditSource {
public:
    virtual ~IAssetAuditSource() = default;
    virtual AssetAuditInventory collect() const = 0;
};

class RigelEmbeddedAuditSource final : public IAssetAuditSource {
public:
    AssetAuditInventory collect() const override;
};

class CRFilesystemAuditSource final : public IAssetAuditSource {
public:
    explicit CRFilesystemAuditSource(std::filesystem::path root);

    AssetAuditInventory collect() const override;

private:
    std::filesystem::path m_root;
};

struct AssetAuditCategoryDiff {
    std::vector<std::string> onlyInLeft;
    std::vector<std::string> onlyInRight;
};

struct AssetAuditDiff {
    AssetAuditInventory left;
    AssetAuditInventory right;

    AssetAuditCategoryDiff blockRoots;
    AssetAuditCategoryDiff blockVariants;
    AssetAuditCategoryDiff modelRefs;
    AssetAuditCategoryDiff textureRefs;
    AssetAuditCategoryDiff entityDefs;
    AssetAuditCategoryDiff itemDefs;
};

AssetAuditDiff diffInventories(AssetAuditInventory left, AssetAuditInventory right);

std::string toJson(const AssetAuditDiff& diff);

int runAssetAuditTool(const std::filesystem::path& crRoot,
                      const std::optional<std::filesystem::path>& outputPath = std::nullopt);

} // namespace Rigel::Asset

