#include "Rigel/Asset/AssetAudit.h"
#include "TestFramework.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

using namespace Rigel::Asset;

namespace {

std::filesystem::path makeTempDir(const std::string& suffix) {
    auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    std::filesystem::path path = std::filesystem::temp_directory_path() /
                                 ("rigel_asset_audit_" + suffix + "_" + std::to_string(now));
    std::filesystem::create_directories(path);
    return path;
}

void writeTextFile(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << text;
    out.flush();
}

bool contains(const std::vector<std::string>& values, const std::string& value) {
    for (const auto& it : values) {
        if (it == value) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE(AssetAudit_DiffDeterministicAndStable) {
    AssetAuditInventory left;
    left.source = "left";
    left.blockRoots = {"base:stone", "base:dirt"};
    left.blockVariants = {"base:stone", "base:dirt"};
    left.modelRefs = {"models/a.json"};
    left.textureRefs = {"textures/a.png"};
    left.entityDefs = {"entities/a.json"};

    AssetAuditInventory right;
    right.source = "right";
    right.blockRoots = {"base:stone", "base:grass"};
    right.blockVariants = {"base:stone", "base:grass[type=full]"};
    right.modelRefs = {"models/b.json"};
    right.textureRefs = {"textures/a.png", "textures/b.png"};
    right.entityDefs = {"entities/b.json"};

    AssetAuditDiff diffA = diffInventories(left, right);
    AssetAuditDiff diffB = diffInventories(left, right);

    std::string jsonA = toJson(diffA);
    std::string jsonB = toJson(diffB);

    CHECK_EQ(jsonA, jsonB);
    CHECK(contains(diffA.blockRoots.onlyInLeft, "base:dirt"));
    CHECK(contains(diffA.blockRoots.onlyInRight, "base:grass"));
    CHECK(contains(diffA.textureRefs.onlyInRight, "textures/b.png"));
}

TEST_CASE(AssetAudit_CRFilesystemDetectsDuplicateBlockVariants) {
    const std::filesystem::path root = makeTempDir("cr");
    try {
        writeTextFile(root / "base" / "blocks" / "stone_a.json", R"({
  "stringId": "base:stone"
})");
        writeTextFile(root / "base" / "blocks" / "stone_b.json", R"({
  "stringId": "base:stone"
})");
        writeTextFile(root / "base" / "blocks" / "grass.json", R"({
  "stringId": "base:grass[type=full]"
})");
        writeTextFile(root / "base" / "models" / "blocks" / "cube.json", "{}");
        writeTextFile(root / "base" / "textures" / "blocks" / "stone.png", "png");
        writeTextFile(root / "base" / "entities" / "demo.json", "{}");
        writeTextFile(root / "base" / "items" / "tool.json", "{}");

        CRFilesystemAuditSource source(root);
        AssetAuditInventory inv = source.collect();

        CHECK(contains(inv.blockVariants, "base:stone"));
        CHECK(contains(inv.blockVariants, "base:grass[type=full]"));
        CHECK(contains(inv.blockRoots, "base:grass"));
        CHECK(contains(inv.duplicateBlockVariants, "base:stone"));
        CHECK(contains(inv.modelRefs, "models/blocks/cube.json"));
        CHECK(contains(inv.textureRefs, "textures/blocks/stone.png"));
        CHECK(contains(inv.entityDefs, "entities/demo.json"));
        CHECK(contains(inv.itemDefs, "items/tool.json"));
    } catch (...) {
        std::filesystem::remove_all(root);
        throw;
    }
    std::filesystem::remove_all(root);
}

TEST_CASE(AssetAudit_RigelEmbeddedDeterministic) {
    RigelEmbeddedAuditSource source;
    AssetAuditInventory a = source.collect();
    AssetAuditInventory b = source.collect();

    AssetAuditDiff diffA = diffInventories(a, b);
    AssetAuditDiff diffB = diffInventories(a, b);

    CHECK_EQ(toJson(diffA), toJson(diffB));
    CHECK(diffA.blockRoots.onlyInLeft.empty());
    CHECK(diffA.blockRoots.onlyInRight.empty());
    CHECK(diffA.blockVariants.onlyInLeft.empty());
    CHECK(diffA.blockVariants.onlyInRight.empty());
}

