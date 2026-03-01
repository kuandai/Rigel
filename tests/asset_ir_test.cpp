#include "Rigel/Asset/AssetIR.h"
#include "TestFramework.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace Rigel::Asset::IR;

namespace {

std::filesystem::path makeTempDir(const std::string& suffix) {
    auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    std::filesystem::path path = std::filesystem::temp_directory_path() /
                                 ("rigel_asset_ir_" + suffix + "_" + std::to_string(now));
    std::filesystem::create_directories(path);
    return path;
}

void writeTextFile(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << text;
    out.flush();
}

std::vector<std::string> collectStateIds(const AssetGraphIR& graph) {
    std::vector<std::string> ids;
    for (const auto& block : graph.blocks) {
        for (const auto& state : block.states) {
            ids.push_back(state.identifier);
        }
    }
    return ids;
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

TEST_CASE(AssetIR_CompileRigelEmbedded_ProducesBlocks) {
    AssetGraphIR graph = compileRigelEmbedded();
    CHECK(!graph.blocks.empty());
    CHECK(!collectStateIds(graph).empty());
}

TEST_CASE(AssetIR_CompileRigelEmbedded_DeterministicStateIds) {
    AssetGraphIR a = compileRigelEmbedded();
    AssetGraphIR b = compileRigelEmbedded();
    CHECK_EQ(collectStateIds(a), collectStateIds(b));
}

TEST_CASE(AssetIR_Validate_DetectsDuplicateBlockStateIdentifier) {
    AssetGraphIR graph;
    BlockDefIR defA;
    defA.rootIdentifier = "base:test";
    defA.sourcePath = "a";
    defA.states.push_back(BlockStateIR{
        .identifier = "base:test",
        .rootIdentifier = "base:test",
        .sourcePath = "a"
    });

    BlockDefIR defB;
    defB.rootIdentifier = "base:test2";
    defB.sourcePath = "b";
    defB.states.push_back(BlockStateIR{
        .identifier = "base:test",
        .rootIdentifier = "base:test2",
        .sourcePath = "b"
    });

    graph.blocks.push_back(std::move(defA));
    graph.blocks.push_back(std::move(defB));

    auto issues = validate(graph);
    bool sawDuplicate = false;
    for (const auto& issue : issues) {
        if (issue.severity == ValidationSeverity::Error &&
            issue.field == "identifier" &&
            issue.identifier == "base:test") {
            sawDuplicate = true;
            break;
        }
    }
    CHECK(sawDuplicate);
}

TEST_CASE(AssetIR_CompileCRFilesystem_CollectsStringIdsAndFallbacks) {
    const std::filesystem::path root = makeTempDir("cr");
    try {
        writeTextFile(root / "base" / "blocks" / "alpha.json", R"({
  "stringId": "base:alpha[type=full]"
})");
        writeTextFile(root / "base" / "blocks" / "fallback.json", R"({
  "noStringId": true
})");
        writeTextFile(root / "base" / "models" / "entities" / "thing.json", "{}");
        writeTextFile(root / "base" / "items" / "item_one.json", "{}");

        AssetGraphIR graph = compileCRFilesystem(root);
        auto ids = collectStateIds(graph);
        CHECK(contains(ids, "base:alpha[type=full]"));
        CHECK(contains(ids, "base:fallback"));
        CHECK(!graph.models.empty());
        CHECK(!graph.items.empty());
    } catch (...) {
        std::filesystem::remove_all(root);
        throw;
    }
    std::filesystem::remove_all(root);
}

