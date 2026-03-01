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

bool hasIssue(const std::vector<ValidationIssue>& issues,
              ValidationSeverity severity,
              const std::string& field,
              const std::string& needle) {
    for (const auto& issue : issues) {
        if (issue.severity != severity || issue.field != field) {
            continue;
        }
        if (issue.message.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool hasAlias(const AssetGraphIR& graph,
              const std::string& canonicalId,
              const std::string& externalId) {
    for (const auto& alias : graph.aliases) {
        if (alias.domain == "block" &&
            alias.canonicalIdentifier == canonicalId &&
            alias.externalIdentifier == externalId) {
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

TEST_CASE(AssetIR_CompileCRFilesystem_ParsesStateExpansionAndGenerators) {
    const std::filesystem::path root = makeTempDir("cr");
    try {
        writeTextFile(root / "base" / "blocks" / "alpha.json", R"({
  "stringId": "base:alpha",
  "defaultParams": {
    "kind": "solid",
    "axis": "y"
  },
  "defaultProperties": {
    "modelName": "base:models/default_alpha.json",
    "stateGenerators": ["base:rotate_axis"]
  },
  "blockStates": {
    "kind=solid": {},
    "kind=glass": {
      "isOpaque": false,
      "stateGenerators": ["base:missing_generator"]
    }
  }
})");
        writeTextFile(root / "base" / "block_state_generators" / "rotate_axis.json", R"({
  "generators": [
    {
      "stringId": "base:rotate_axis",
      "include": ["base:rotate_axis_x"]
    },
    {
      "stringId": "base:rotate_axis_x",
      "params": {"axis": "x"},
      "overrides": {
        "modelName": "base:models/alpha_rotated.json",
        "isOpaque": false
      }
    }
  ]
})");
        writeTextFile(root / "base" / "models" / "default_alpha.json", "{}");
        writeTextFile(root / "base" / "models" / "alpha_rotated.json", "{}");
        writeTextFile(root / "base" / "models" / "entities" / "thing.json", "{}");
        writeTextFile(root / "base" / "items" / "item_one.json", "{}");

        AssetGraphIR graph = compileCRFilesystem(root);
        auto issues = validate(graph);
        auto ids = collectStateIds(graph);
        CHECK_EQ(graph.blocks.size(), 1u);
        CHECK_EQ(ids.size(), 4u);
        CHECK(contains(ids, "base:alpha[axis=x,kind=solid]"));
        CHECK(contains(ids, "base:alpha[axis=y,kind=glass]"));
        CHECK(contains(ids, "base:alpha[axis=y,kind=solid]"));
        CHECK(contains(ids, "base:alpha[axis=x,kind=glass]"));
        CHECK(!graph.models.empty());
        CHECK(!graph.items.empty());
        CHECK(hasAlias(graph,
                       "base:alpha[axis=x,kind=solid]",
                       "base:alpha[kind=solid,axis=x]"));
        CHECK(hasIssue(issues,
                       ValidationSeverity::Warning,
                       "stateGenerators",
                       "Unsupported generator"));
    } catch (...) {
        std::filesystem::remove_all(root);
        throw;
    }
    std::filesystem::remove_all(root);
}

TEST_CASE(AssetIR_Validate_ReportsUnresolvedModelAndTextureRefs) {
    AssetGraphIR graph;
    graph.models.push_back(ModelRefIR{"models/blocks/existing.json", "models/blocks/existing.json", {}});
    graph.textures.push_back(TextureRefIR{"textures/blocks/existing.png", "textures/blocks/existing.png", {}});

    BlockDefIR block;
    block.rootIdentifier = "base:test";
    block.sourcePath = "blocks/test.json";
    BlockStateIR state;
    state.identifier = "base:test";
    state.rootIdentifier = "base:test";
    state.sourcePath = block.sourcePath;
    state.model = "base:models/blocks/missing.json";
    state.textures["default"] = "base:textures/blocks/missing.png";
    block.states.push_back(std::move(state));
    graph.blocks.push_back(std::move(block));

    auto issues = validate(graph);
    CHECK(hasIssue(issues, ValidationSeverity::Error, "model", "Unresolved model reference"));
    CHECK(hasIssue(issues, ValidationSeverity::Warning, "textures.default", "Unresolved texture reference"));
}

TEST_CASE(AssetIR_Validate_AcceptsNormalizedNamespacedRefs) {
    AssetGraphIR graph;
    graph.models.push_back(ModelRefIR{"models/blocks/example.json", "models/blocks/example.json", {}});
    graph.textures.push_back(TextureRefIR{"textures/blocks/example.png", "textures/blocks/example.png", {}});

    BlockDefIR block;
    block.rootIdentifier = "base:test";
    block.sourcePath = "blocks/test.json";
    BlockStateIR state;
    state.identifier = "base:test";
    state.rootIdentifier = "base:test";
    state.sourcePath = block.sourcePath;
    state.model = "base:models/blocks/example.json";
    state.textures["default"] = "base:textures/blocks/example.png";
    block.states.push_back(std::move(state));
    graph.blocks.push_back(std::move(block));

    auto issues = validate(graph);
    CHECK(!hasIssue(issues, ValidationSeverity::Error, "model", "Unresolved model reference"));
    CHECK(!hasIssue(issues, ValidationSeverity::Warning, "textures.default", "Unresolved texture reference"));
}

TEST_CASE(AssetIR_Validate_ReportsRenderLayerFlagMismatch) {
    AssetGraphIR graph;

    BlockDefIR block;
    block.rootIdentifier = "base:test";
    block.sourcePath = "blocks/test.json";

    BlockStateIR opaqueMismatch;
    opaqueMismatch.identifier = "base:test[mode=a]";
    opaqueMismatch.rootIdentifier = "base:test";
    opaqueMismatch.sourcePath = block.sourcePath;
    opaqueMismatch.renderLayer = "opaque";
    opaqueMismatch.isOpaque = false;

    BlockStateIR transparentMismatch;
    transparentMismatch.identifier = "base:test[mode=b]";
    transparentMismatch.rootIdentifier = "base:test";
    transparentMismatch.sourcePath = block.sourcePath;
    transparentMismatch.renderLayer = "transparent";
    transparentMismatch.isOpaque = true;

    block.states.push_back(std::move(opaqueMismatch));
    block.states.push_back(std::move(transparentMismatch));
    graph.blocks.push_back(std::move(block));

    auto issues = validate(graph);
    CHECK(hasIssue(issues, ValidationSeverity::Warning, "renderLayer", "Opaque render layer with non-opaque"));
    CHECK(hasIssue(issues, ValidationSeverity::Warning, "renderLayer", "Transparent/cutout render layer with opaque"));
}

TEST_CASE(AssetIR_CompileCRFilesystem_NormalizesModelRefsAndRenderLayer) {
    const std::filesystem::path root = makeTempDir("cr_normalize");
    try {
        writeTextFile(root / "base" / "blocks" / "gamma.json", R"({
  "stringId": "base:gamma",
  "defaultProperties": {
    "modelName": "./base:models/blocks/gamma.json",
    "renderLayer": "CuToUt",
    "isOpaque": false
  },
  "blockStates": {
    "state=default": {}
  }
})");
        writeTextFile(root / "base" / "models" / "blocks" / "gamma.json", "{}");

        AssetGraphIR graph = compileCRFilesystem(root);
        CHECK_EQ(graph.blocks.size(), 1u);
        CHECK_EQ(graph.blocks.front().states.size(), 1u);
        const BlockStateIR& state = graph.blocks.front().states.front();
        CHECK_EQ(state.model, "models/blocks/gamma.json");
        CHECK_EQ(state.renderLayer, "cutout");
    } catch (...) {
        std::filesystem::remove_all(root);
        throw;
    }
    std::filesystem::remove_all(root);
}

TEST_CASE(AssetIR_CompileCRFilesystem_CollectsTexturesDeterministically) {
    const std::filesystem::path root = makeTempDir("cr_textures");
    try {
        writeTextFile(root / "base" / "blocks" / "delta.json", R"({
  "stringId": "base:delta",
  "blockStates": { "state=default": {} }
})");
        writeTextFile(root / "base" / "textures" / "blocks" / "b.png", "");
        writeTextFile(root / "base" / "textures" / "blocks" / "a.png", "");

        AssetGraphIR graph = compileCRFilesystem(root);
        CHECK_EQ(graph.textures.size(), 2u);
        CHECK_EQ(graph.textures[0].identifier, "textures/blocks/a.png");
        CHECK_EQ(graph.textures[1].identifier, "textures/blocks/b.png");
    } catch (...) {
        std::filesystem::remove_all(root);
        throw;
    }
    std::filesystem::remove_all(root);
}

TEST_CASE(AssetIR_Validate_RigelEmbedded_HasNoCriticalModelResolutionErrors) {
    AssetGraphIR graph = compileRigelEmbedded();
    const auto issues = validate(graph);
    bool hasCriticalModelError = false;
    for (const auto& issue : issues) {
        if (issue.severity == ValidationSeverity::Error && issue.field == "model") {
            hasCriticalModelError = true;
            break;
        }
    }
    CHECK(!hasCriticalModelError);
}

TEST_CASE(AssetIR_CompileCRFilesystem_DeterministicExpansionAndAliases) {
    const std::filesystem::path root = makeTempDir("cr_deterministic");
    try {
        writeTextFile(root / "base" / "blocks" / "beta.json", R"({
  "stringId": "base:beta",
  "defaultParams": { "zeta": "0", "alpha": "1" },
  "blockStates": { "zeta=2": {} }
})");
        AssetGraphIR first = compileCRFilesystem(root);
        AssetGraphIR second = compileCRFilesystem(root);
        CHECK_EQ(collectStateIds(first), collectStateIds(second));
        CHECK_EQ(first.aliases.size(), second.aliases.size());
        for (size_t i = 0; i < first.aliases.size(); ++i) {
            CHECK_EQ(first.aliases[i].canonicalIdentifier, second.aliases[i].canonicalIdentifier);
            CHECK_EQ(first.aliases[i].externalIdentifier, second.aliases[i].externalIdentifier);
        }
    } catch (...) {
        std::filesystem::remove_all(root);
        throw;
    }
    std::filesystem::remove_all(root);
}
