#include "Rigel/Asset/ShaderCompiler.h"
#include "Rigel/Asset/AssetManager.h"

#include <spdlog/spdlog.h>
#include <vector>

namespace Rigel::Asset {

std::string ShaderCompiler::preprocess(
    const std::string& source,
    const std::unordered_map<std::string, std::string>& defines
) {
    if (source.empty()) {
        return source;
    }

    std::string result;

    // Find #version line
    size_t versionEnd = source.find('\n');
    bool hasVersion = (source.size() >= 8 && source.substr(0, 8) == "#version");

    if (hasVersion && versionEnd != std::string::npos) {
        result = source.substr(0, versionEnd + 1);
    } else {
        // Default version if none specified
        result = "#version 410 core\n";
        versionEnd = 0;
    }

    // Inject defines after version line
    for (const auto& [name, value] : defines) {
        if (value == "true") {
            result += "#define " + name + " 1\n";
        } else if (value == "false") {
            result += "#define " + name + " 0\n";
        } else {
            result += "#define " + name + " " + value + "\n";
        }
    }

    // Append rest of source
    if (hasVersion && versionEnd != std::string::npos) {
        result += source.substr(versionEnd + 1);
    } else {
        result += source;
    }

    return result;
}

GLuint ShaderCompiler::compileStage(
    GLenum type,
    const std::string& source,
    const std::string& shaderId
) {
    GLuint shader = glCreateShader(type);

    const char* src = source.c_str();
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);

    checkCompileErrors(shader, type, shaderId);

    return shader;
}

void ShaderCompiler::checkCompileErrors(
    GLuint shader,
    GLenum stage,
    const std::string& shaderId
) {
    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);

    if (!success) {
        GLint length;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);

        std::string log(length, '\0');
        glGetShaderInfoLog(shader, length, nullptr, log.data());

        spdlog::error("{} shader '{}' compilation failed:\n{}",
                      stageToString(stage), shaderId, log);

        glDeleteShader(shader);
        throw ShaderCompileError(shaderId, stage, log);
    }
}

void ShaderCompiler::checkLinkErrors(GLuint program, const std::string& shaderId) {
    GLint success;
    glGetProgramiv(program, GL_LINK_STATUS, &success);

    if (!success) {
        GLint length;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);

        std::string log(length, '\0');
        glGetProgramInfoLog(program, length, nullptr, log.data());

        spdlog::error("Shader program '{}' linking failed:\n{}", shaderId, log);

        glDeleteProgram(program);
        throw ShaderLinkError(shaderId, log);
    }
}

const char* ShaderCompiler::stageToString(GLenum stage) {
    switch (stage) {
        case GL_VERTEX_SHADER:   return "Vertex";
        case GL_FRAGMENT_SHADER: return "Fragment";
        case GL_GEOMETRY_SHADER: return "Geometry";
        case GL_COMPUTE_SHADER:  return "Compute";
        default:                 return "Unknown";
    }
}

GLuint ShaderCompiler::compile(const ShaderSource& source, const std::string& shaderId) {
    std::vector<GLuint> shaders;

    // Determine if this is a compute shader or graphics pipeline
    bool isCompute = !source.compute.empty();

    if (isCompute) {
        // Compute shader pipeline
        std::string preprocessed = preprocess(source.compute, source.defines);
        shaders.push_back(compileStage(GL_COMPUTE_SHADER, preprocessed, shaderId));
    } else {
        // Graphics pipeline - vertex and fragment are required
        if (source.vertex.empty()) {
            throw AssetLoadError(shaderId, "Vertex shader source is required");
        }
        if (source.fragment.empty()) {
            throw AssetLoadError(shaderId, "Fragment shader source is required");
        }

        // Compile vertex shader
        std::string vertPreprocessed = preprocess(source.vertex, source.defines);
        shaders.push_back(compileStage(GL_VERTEX_SHADER, vertPreprocessed, shaderId));

        // Compile fragment shader
        std::string fragPreprocessed = preprocess(source.fragment, source.defines);
        shaders.push_back(compileStage(GL_FRAGMENT_SHADER, fragPreprocessed, shaderId));

        // Compile geometry shader if present
        if (!source.geometry.empty()) {
            std::string geomPreprocessed = preprocess(source.geometry, source.defines);
            shaders.push_back(compileStage(GL_GEOMETRY_SHADER, geomPreprocessed, shaderId));
        }
    }

    // Create and link program
    GLuint program = glCreateProgram();

    for (GLuint shader : shaders) {
        glAttachShader(program, shader);
    }

    glLinkProgram(program);
    checkLinkErrors(program, shaderId);

    // Detach and delete shader objects
    for (GLuint shader : shaders) {
        glDetachShader(program, shader);
        glDeleteShader(shader);
    }

    spdlog::debug("Compiled shader program '{}' (id={})", shaderId, program);

    return program;
}

} // namespace Rigel::Asset
