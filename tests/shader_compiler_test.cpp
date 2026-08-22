#include "TestFramework.h"
#include "OpenGLFixture.h"

#include "Rigel/Asset/AssetManager.h"
#include "Rigel/Asset/ShaderCompiler.h"
#include "Rigel/Asset/ShaderLoader.h"

#include <array>
#include <charconv>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace Rigel::Asset;
using Rigel::Test::HiddenOpenGLContext;

namespace {

int readShaderVersion(const std::string& path, std::span<const char> data) {
    std::string_view source(data.data(), data.size());
    constexpr std::string_view prefix = "#version ";
    if (!source.starts_with(prefix)) {
        throw Rigel::Test::TestFailure(
            "Shader source '" + path + "' must begin with a #version declaration");
    }

    const size_t lineEnd = source.find('\n');
    std::string_view declaration = source.substr(0, lineEnd);
    if (declaration.ends_with('\r')) {
        declaration.remove_suffix(1);
    }
    declaration.remove_prefix(prefix.size());

    int version = 0;
    const auto result = std::from_chars(
        declaration.data(), declaration.data() + declaration.size(), version);
    if (result.ec != std::errc{} || result.ptr == declaration.data()) {
        throw Rigel::Test::TestFailure(
            "Shader source '" + path + "' has an invalid #version declaration");
    }

    declaration.remove_prefix(static_cast<size_t>(result.ptr - declaration.data()));
    while (declaration.starts_with(' ') || declaration.starts_with('\t')) {
        declaration.remove_prefix(1);
    }
    if (declaration != "core") {
        throw Rigel::Test::TestFailure(
            "Shader source '" + path + "' must declare the core GLSL profile");
    }
    return version;
}

std::vector<std::string> manifestShaderStagePaths(
    const AssetManager::AssetEntry& entry
) {
    constexpr std::array<std::string_view, 2> stageKeys = {
        "vertex", "fragment"
    };

    std::vector<std::string> paths;
    for (const std::string_view key : stageKeys) {
        if (const auto path = entry.getString(std::string(key))) {
            paths.push_back(*path);
        }
    }

    return paths;
}

template <typename LoadResource>
void validateManifestShaderStages(
    const AssetManager::AssetEntry& entry,
    int supportedVersion,
    std::unordered_set<std::string>& checkedPaths,
    LoadResource&& loadResource
) {
    for (const std::string& path : manifestShaderStagePaths(entry)) {
        if (!checkedPaths.insert(path).second) {
            continue;
        }

        const int version = readShaderVersion(path, loadResource(path));
        if (version > supportedVersion) {
            throw Rigel::Test::TestFailure(
                "Manifest-referenced shader stage '" + path + "' requires GLSL " +
                std::to_string(version) + ", but the runtime supports GLSL " +
                std::to_string(supportedVersion));
        }
    }
}

void checkShaderLoadError(const char* configSource, const std::string& expectedReason) {
    AssetManager assets;
    ryml::Tree configTree = ryml::parse_in_arena(configSource);
    const std::string shaderId = "shaders/incomplete";
    LoadContext context{shaderId, configTree.crootref(), assets};
    ShaderLoader loader;

    try {
        loader.load(context);
    } catch (const AssetLoadError& error) {
        CHECK_EQ(
            std::string(error.what()),
            "Failed to load asset 'shaders/incomplete': " + expectedReason);
        return;
    } catch (const std::exception& error) {
        throw Rigel::Test::TestFailure(
            "Expected AssetLoadError, received: " + std::string(error.what()));
    }

    throw Rigel::Test::TestFailure("Expected AssetLoadError");
}

const ShaderSource validShaderSource{
    "#version 410 core\n"
    "void main() {\n"
    "    gl_Position = vec4(0.0);\n"
    "}\n",
    "#version 410 core\n"
    "out vec4 color;\n"
    "void main() {\n"
    "    color = vec4(1.0);\n"
    "}\n"
};

void checkFragmentCompileFailure() {
    ShaderSource source = validShaderSource;
    source.fragment =
        "#version 410 core\n"
        "out vec4 color;\n"
        "void main() {\n"
        "    color = missing_value;\n"
        "}\n";

    try {
        const GLuint program = ShaderCompiler::compile(source, "fragment_failure");
        glDeleteProgram(program);
    } catch (const ShaderCompileError& error) {
        CHECK_EQ(error.stage(), GL_FRAGMENT_SHADER);
        CHECK_EQ(glGetError(), GL_NO_ERROR);
        return;
    } catch (const std::exception& error) {
        throw Rigel::Test::TestFailure(
            "Expected ShaderCompileError, received: " + std::string(error.what()));
    }

    throw Rigel::Test::TestFailure("Expected ShaderCompileError");
}

void checkLinkFailure() {
    const ShaderSource source{
        "#version 410 core\n"
        "out vec3 link_value;\n"
        "void main() {\n"
        "    link_value = vec3(1.0);\n"
        "    gl_Position = vec4(0.0);\n"
        "}\n",
        "#version 410 core\n"
        "in vec4 link_value;\n"
        "out vec4 color;\n"
        "void main() {\n"
        "    color = link_value;\n"
        "}\n"
    };

    try {
        const GLuint program = ShaderCompiler::compile(source, "link_failure");
        glDeleteProgram(program);
    } catch (const ShaderLinkError&) {
        CHECK_EQ(glGetError(), GL_NO_ERROR);
        return;
    } catch (const std::exception& error) {
        throw Rigel::Test::TestFailure(
            "Expected ShaderLinkError, received: " + std::string(error.what()));
    }

    throw Rigel::Test::TestFailure("Expected ShaderLinkError");
}

} // namespace

TEST_CASE(ShaderLoader_MissingStagesFailClearly) {
    checkShaderLoadError(
        "fragment: shaders/voxel.frag\n",
        "Shader missing 'vertex' source");
    checkShaderLoadError(
        "vertex: shaders/not_present.vert\n",
        "Shader missing 'fragment' source");
}

TEST_CASE(ShaderLoader_RejectsUnsupportedConfiguration) {
    checkShaderLoadError(
        "vertex: shaders/voxel.vert\n"
        "fragment: shaders/voxel.frag\n"
        "options: unused\n",
        "Shader configuration contains unsupported field 'options'");
}

TEST_CASE(ShaderCompiler_ManifestShaderVersionsMatchRuntime) {
    CHECK_EQ(
        Rigel::Render::kSupportedGLSLVersion,
        Rigel::Render::kOpenGLContextMajorVersion * 100 +
            Rigel::Render::kOpenGLContextMinorVersion * 10);

    AssetManager assets;
    CHECK_NO_THROW(assets.loadManifest("manifest.yaml"));

    std::unordered_set<std::string> checkedPaths;

    assets.forEachInCategory("shaders", [&](const std::string& name,
                                               const AssetManager::AssetEntry& entry) {
        const std::string shaderId = "shaders/" + name;
        LoadContext context{shaderId, entry.config, assets};
        validateManifestShaderStages(
            entry,
            Rigel::Render::kSupportedGLSLVersion,
            checkedPaths,
            [&](const std::string& path) { return context.loadResource(path); });
    });

    CHECK(!checkedPaths.empty());
}

TEST_CASE(ShaderCompiler_ShippedProgramsCompileAndLink) {
    HiddenOpenGLContext context;
    context.require();

    AssetManager assets;
    CHECK_NO_THROW(assets.loadManifest("manifest.yaml"));

    size_t compiledPrograms = 0;
    assets.forEachInCategory("shaders", [&](const std::string& name,
                                               const AssetManager::AssetEntry&) {
        CHECK_NO_THROW(assets.get<ShaderAsset>("shaders/" + name));
        ++compiledPrograms;
    });
    CHECK(compiledPrograms > 0);
}

TEST_CASE(ShaderCompiler_HandlesCompileAndLinkOutcomes) {
    HiddenOpenGLContext context;
    context.require();

    checkFragmentCompileFailure();
    checkLinkFailure();

    ShaderAsset compiled;
    compiled.program = ShaderCompiler::compile(validShaderSource, "valid_program");
    CHECK_NE(compiled.program, 0u);
    CHECK_EQ(glIsProgram(compiled.program), GL_TRUE);

    GLint linkStatus = GL_FALSE;
    glGetProgramiv(compiled.program, GL_LINK_STATUS, &linkStatus);
    CHECK_EQ(linkStatus, GL_TRUE);

    GLint attachedShaders = -1;
    glGetProgramiv(compiled.program, GL_ATTACHED_SHADERS, &attachedShaders);
    CHECK_EQ(attachedShaders, 0);
    CHECK_EQ(glGetError(), GL_NO_ERROR);
}
