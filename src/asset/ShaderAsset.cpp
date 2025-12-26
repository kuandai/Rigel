#include "Rigel/Asset/Types.h"

#include <spdlog/spdlog.h>

namespace Rigel::Asset {

ShaderAsset::ShaderAsset(ShaderAsset&& other) noexcept
    : program(other.program)
    , m_uniformCache(std::move(other.m_uniformCache))
    , m_attributeCache(std::move(other.m_attributeCache))
{
    other.program = 0;
}

ShaderAsset& ShaderAsset::operator=(ShaderAsset&& other) noexcept {
    if (this != &other) {
        release();
        program = other.program;
        m_uniformCache = std::move(other.m_uniformCache);
        m_attributeCache = std::move(other.m_attributeCache);
        other.program = 0;
    }
    return *this;
}

ShaderAsset::~ShaderAsset() {
    release();
}

void ShaderAsset::release() {
    if (program != 0) {
        glDeleteProgram(program);
        program = 0;
    }
    m_uniformCache.clear();
    m_attributeCache.clear();
}

GLint ShaderAsset::uniform(const std::string& name) const {
    auto it = m_uniformCache.find(name);
    if (it != m_uniformCache.end()) {
        return it->second;
    }

    GLint location = glGetUniformLocation(program, name.c_str());
    m_uniformCache[name] = location;

    if (location == -1) {
        spdlog::warn("Uniform '{}' not found in shader program {}", name, program);
    }

    return location;
}

GLint ShaderAsset::attribute(const std::string& name) const {
    auto it = m_attributeCache.find(name);
    if (it != m_attributeCache.end()) {
        return it->second;
    }

    GLint location = glGetAttribLocation(program, name.c_str());
    m_attributeCache[name] = location;

    if (location == -1) {
        spdlog::warn("Attribute '{}' not found in shader program {}", name, program);
    }

    return location;
}

} // namespace Rigel::Asset
