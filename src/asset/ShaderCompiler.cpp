#include "Rigel/Asset/ShaderCompiler.h"
#include "Rigel/Asset/AssetManager.h"

#include <spdlog/spdlog.h>
#include <utility>

namespace Rigel::Asset {
namespace {

class ShaderObject {
public:
    explicit ShaderObject(GLenum type)
        : m_id(glCreateShader(type))
    {}

    ShaderObject(const ShaderObject&) = delete;
    ShaderObject& operator=(const ShaderObject&) = delete;

    ShaderObject(ShaderObject&& other) noexcept
        : m_id(std::exchange(other.m_id, 0))
    {}

    ~ShaderObject() {
        if (m_id != 0) {
            glDeleteShader(m_id);
        }
    }

    GLuint get() const { return m_id; }

private:
    GLuint m_id;
};

class ProgramObject {
public:
    ProgramObject()
        : m_id(glCreateProgram())
    {}

    ProgramObject(const ProgramObject&) = delete;
    ProgramObject& operator=(const ProgramObject&) = delete;

    ~ProgramObject() {
        if (m_id != 0) {
            glDeleteProgram(m_id);
        }
    }

    GLuint get() const { return m_id; }
    GLuint release() { return std::exchange(m_id, 0); }

private:
    GLuint m_id;
};

const char* stageToString(GLenum stage) {
    switch (stage) {
        case GL_VERTEX_SHADER:   return "Vertex";
        case GL_FRAGMENT_SHADER: return "Fragment";
        default:                 return "Unknown";
    }
}

void checkCompileErrors(
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

        throw ShaderCompileError(shaderId, stage, log);
    }
}

ShaderObject compileStage(
    GLenum type,
    const std::string& source,
    const std::string& shaderId
) {
    ShaderObject shader(type);

    const char* src = source.c_str();
    glShaderSource(shader.get(), 1, &src, nullptr);
    glCompileShader(shader.get());

    checkCompileErrors(shader.get(), type, shaderId);

    return shader;
}

void checkLinkErrors(GLuint program, const std::string& shaderId) {
    GLint success;
    glGetProgramiv(program, GL_LINK_STATUS, &success);

    if (!success) {
        GLint length;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);

        std::string log(length, '\0');
        glGetProgramInfoLog(program, length, nullptr, log.data());

        spdlog::error("Shader program '{}' linking failed:\n{}", shaderId, log);

        throw ShaderLinkError(shaderId, log);
    }
}

} // namespace

GLuint ShaderCompiler::compile(const ShaderSource& source, const std::string& shaderId) {
    if (source.vertex.empty()) {
        throw AssetLoadError(shaderId, "Vertex shader source is required");
    }
    if (source.fragment.empty()) {
        throw AssetLoadError(shaderId, "Fragment shader source is required");
    }

    ShaderObject vertex = compileStage(GL_VERTEX_SHADER, source.vertex, shaderId);
    ShaderObject fragment = compileStage(GL_FRAGMENT_SHADER, source.fragment, shaderId);

    ProgramObject program;

    glAttachShader(program.get(), vertex.get());
    glAttachShader(program.get(), fragment.get());

    glLinkProgram(program.get());
    checkLinkErrors(program.get(), shaderId);

    glDetachShader(program.get(), vertex.get());
    glDetachShader(program.get(), fragment.get());

    spdlog::debug("Compiled shader program '{}' (id={})", shaderId, program.get());

    return program.release();
}

} // namespace Rigel::Asset
