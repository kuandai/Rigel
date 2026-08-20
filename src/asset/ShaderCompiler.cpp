#include "Rigel/Asset/ShaderCompiler.h"
#include "Rigel/Asset/AssetManager.h"

#include <spdlog/spdlog.h>
#include <vector>

namespace Rigel::Asset {

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
        default:                 return "Unknown";
    }
}

GLuint ShaderCompiler::compile(const ShaderSource& source, const std::string& shaderId) {
    if (source.vertex.empty()) {
        throw AssetLoadError(shaderId, "Vertex shader source is required");
    }
    if (source.fragment.empty()) {
        throw AssetLoadError(shaderId, "Fragment shader source is required");
    }

    std::vector<GLuint> shaders;
    shaders.push_back(compileStage(GL_VERTEX_SHADER, source.vertex, shaderId));
    shaders.push_back(compileStage(GL_FRAGMENT_SHADER, source.fragment, shaderId));

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
