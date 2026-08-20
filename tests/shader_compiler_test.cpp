#include "TestFramework.h"

#include "Rigel/Asset/AssetManager.h"
#include "Rigel/Asset/ShaderCompiler.h"
#include "Rigel/Asset/ShaderLoader.h"
#include "Rigel/Render/OpenGLRuntime.h"

#include <GLFW/glfw3.h>

#include <array>
#include <charconv>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace Rigel::Asset;

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

class HiddenOpenGLContext {
public:
    HiddenOpenGLContext() {
        if (!glfwInit()) {
            setGlfwError("GLFW initialization failed");
            return;
        }
        m_glfwInitialized = true;

        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, Rigel::Render::kOpenGLContextMajorVersion);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, Rigel::Render::kOpenGLContextMinorVersion);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

        m_window = glfwCreateWindow(64, 64, "Rigel shader validation", nullptr, nullptr);
        if (!m_window) {
            setGlfwError("OpenGL context creation failed");
            return;
        }

        glfwMakeContextCurrent(m_window);
        glewExperimental = GL_TRUE;
        const GLenum glewStatus = glewInit();
        if (glewStatus != GLEW_OK) {
            m_error = "GLEW initialization failed: ";
            m_error += reinterpret_cast<const char*>(glewGetErrorString(glewStatus));
            return;
        }

        // GLEW can leave GL_INVALID_ENUM set after probing a core context.
        glGetError();
        m_available = true;
    }

    ~HiddenOpenGLContext() {
        if (m_window) {
            glfwMakeContextCurrent(nullptr);
            glfwDestroyWindow(m_window);
        }
        if (m_glfwInitialized) {
            glfwTerminate();
        }
    }

    bool available() const { return m_available; }
    const std::string& error() const { return m_error; }

private:
    void setGlfwError(std::string prefix) {
        const char* description = nullptr;
        const int code = glfwGetError(&description);
        m_error = std::move(prefix) + " (GLFW error " + std::to_string(code);
        if (description) {
            m_error += ": ";
            m_error += description;
        }
        m_error += ")";
    }

    GLFWwindow* m_window = nullptr;
    bool m_glfwInitialized = false;
    bool m_available = false;
    std::string m_error;
};

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
    if (!context.available()) {
        SKIP_TEST(context.error());
    }

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
