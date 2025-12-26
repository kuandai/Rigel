# Shader Loading System Design Document

This document describes a simple shader loading system for Rigel that compiles GLSL shaders, manages programs, and integrates with the existing asset system.

---

## Table of Contents

1. [Design Goals](#1-design-goals)
2. [Architecture Overview](#2-architecture-overview)
3. [Manifest Format](#3-manifest-format)
4. [Core Components](#4-core-components)
5. [Uniform Management](#5-uniform-management)
6. [Preprocessor System](#6-preprocessor-system)
7. [Error Handling](#7-error-handling)
8. [Usage Examples](#8-usage-examples)

---

## 1. Design Goals

| Priority | Goal |
|----------|------|
| **Primary** | Compile and link GLSL shaders into programs |
| **Primary** | Integrate with AssetManager for manifest-driven loading |
| **Primary** | Provide uniform location caching |
| **Secondary** | Support compile-time defines |
| **Secondary** | Shader inheritance for variants |
| **Tertiary** | Include directive support |

### Out of Scope (for now)

- Hot-reloading
- Shader reflection/introspection
- Uniform buffer objects (UBOs)
- Shader storage buffers (SSBOs)
- Binary shader caching

---

## 2. Architecture Overview

```
┌─────────────────────────────────────────────────────────────┐
│                     Application Code                         │
│                            │                                 │
│                            ▼                                 │
│  ┌─────────────────────────────────────────────────────┐    │
│  │                    AssetManager                      │    │
│  │         assets.get<ShaderAsset>("shaders/voxel")    │    │
│  └─────────────────────────────────────────────────────┘    │
│                            │                                 │
│                            ▼                                 │
│  ┌─────────────────────────────────────────────────────┐    │
│  │                   ShaderCompiler                     │    │
│  │  • Preprocessor      • Compilation     • Linking     │    │
│  └─────────────────────────────────────────────────────┘    │
│                            │                                 │
│              ┌─────────────┼─────────────┐                  │
│              ▼             ▼             ▼                   │
│       ┌──────────┐  ┌──────────┐  ┌──────────┐              │
│       │  Vertex  │  │ Fragment │  │ Geometry │              │
│       │  Shader  │  │  Shader  │  │  Shader  │              │
│       └──────────┘  └──────────┘  └──────────┘              │
└─────────────────────────────────────────────────────────────┘
```

### Dependencies

- `ResourceRegistry` - Raw shader source from embedded assets
- `AssetManager` - Manifest parsing and caching
- OpenGL 4.1+ - Shader compilation and linking

---

## 3. Manifest Format

Shaders are declared in the asset manifest under the `shaders` category:

### 3.1 Basic Shader

```yaml
assets:
  shaders:
    basic:
      vertex: shaders/basic.vert
      fragment: shaders/basic.frag
```

### 3.2 Full Specification

```yaml
assets:
  shaders:
    voxel:
      vertex: shaders/voxel.vert
      fragment: shaders/voxel.frag
      geometry: shaders/voxel.geom        # Optional
      defines:
        MAX_LIGHTS: 8
        ENABLE_AO: true
        TEXTURE_SIZE: 256

    # Compute shader
    light_propagate:
      compute: shaders/light_propagate.comp
      defines:
        WORKGROUP_SIZE: 8
```

### 3.3 Shader Inheritance

Create shader variants by inheriting from a base shader:

```yaml
assets:
  shaders:
    voxel_opaque:
      vertex: shaders/voxel.vert
      fragment: shaders/voxel_opaque.frag
      defines:
        ENABLE_AO: true

    voxel_transparent:
      inherit: shaders/voxel_opaque
      fragment: shaders/voxel_transparent.frag    # Override fragment
      defines:
        ENABLE_ALPHA_BLEND: true                  # Additional define
```

**Inheritance Rules:**
1. Child inherits all properties from parent
2. Explicit properties in child override parent
3. `defines` are merged (child values override conflicts)
4. Circular inheritance is an error

For detailed inheritance resolution implementation, see `AssetSystem.md` section 6.3.

---

## 4. Core Components

### 4.1 ShaderAsset

```cpp
namespace Rigel::Asset {

/// Compiled and linked OpenGL shader program
struct ShaderAsset {
    GLuint program = 0;

    ShaderAsset() = default;

    // Non-copyable (owns OpenGL resource)
    ShaderAsset(const ShaderAsset&) = delete;
    ShaderAsset& operator=(const ShaderAsset&) = delete;

    // Movable
    ShaderAsset(ShaderAsset&& other) noexcept;
    ShaderAsset& operator=(ShaderAsset&& other) noexcept;

    ~ShaderAsset();

    /// Bind this shader program
    void bind() const {
        glUseProgram(program);
    }

    /// Get uniform location (cached)
    GLint uniform(const std::string& name) const;

    /// Get attribute location
    GLint attribute(const std::string& name) const;

private:
    mutable std::unordered_map<std::string, GLint> m_uniformCache;
    mutable std::unordered_map<std::string, GLint> m_attributeCache;

    void release();
};

} // namespace Rigel::Asset
```

### 4.2 ShaderCompiler

Internal utility class for compilation:

```cpp
namespace Rigel::Asset {

class ShaderCompiler {
public:
    struct ShaderSource {
        std::string vertex;
        std::string fragment;
        std::string geometry;    // Optional
        std::string compute;     // For compute shaders
        std::unordered_map<std::string, std::string> defines;
    };

    /// Compile and link a shader program
    /// @throws ShaderCompileError on compilation failure
    /// @throws ShaderLinkError on linking failure
    static GLuint compile(const ShaderSource& source);

private:
    static GLuint compileStage(GLenum type, const std::string& source, const std::string& shaderId);
    static std::string preprocess(const std::string& source,
                                  const std::unordered_map<std::string, std::string>& defines);
    static void checkCompileErrors(GLuint shader, GLenum stage, const std::string& shaderId);
    static void checkLinkErrors(GLuint program, const std::string& shaderId);
};

} // namespace Rigel::Asset
```

### 4.3 Integration with AssetManager

Shaders integrate with AssetManager through the loader registration system (see `AssetSystem.md` section 10.1):

```cpp
// Register the shader loader during initialization
AssetManager assets;
assets.registerLoader("shaders", std::make_unique<ShaderLoader>());
assets.loadManifest("manifest.yaml");

// Load shaders like any other asset
auto shader = assets.get<ShaderAsset>("shaders/voxel");
```

**ShaderLoader Implementation:**

```cpp
class ShaderLoader : public IAssetLoader {
public:
    std::string_view category() const override { return "shaders"; }

    std::shared_ptr<AssetBase> load(const LoadContext& ctx) override {
        // Resolve inheritance if present (see AssetSystem.md section 6.3)
        ResolvedShaderConfig config = resolveConfig(ctx);

        // Load source files
        std::string vertSrc(ctx.loadResource(config.vertex));
        std::string fragSrc(ctx.loadResource(config.fragment));

        // Preprocess and compile
        auto shader = std::make_shared<ShaderAsset>();
        shader->program = ShaderCompiler::compile({
            .vertex = vertSrc,
            .fragment = fragSrc,
            .defines = config.defines
        });

        return shader;
    }

private:
    ResolvedShaderConfig resolveConfig(const LoadContext& ctx);
};
```

The loader handles:
- Multi-source file loading (vertex, fragment, geometry, compute)
- Inheritance resolution via `ctx.loadDependency<ShaderAsset>()`
- Nested `defines` map extraction
- Preprocessor define injection

---

## 5. Uniform Management

### 5.1 Cached Lookups

Uniform locations are cached on first access:

```cpp
GLint ShaderAsset::uniform(const std::string& name) const {
    auto it = m_uniformCache.find(name);
    if (it != m_uniformCache.end()) {
        return it->second;
    }

    GLint location = glGetUniformLocation(program, name.c_str());
    m_uniformCache[name] = location;

    if (location == -1) {
        spdlog::warn("Uniform '{}' not found in shader", name);
    }

    return location;
}
```

### 5.2 Usage Pattern

```cpp
auto shader = assets.get<ShaderAsset>("shaders/voxel");
shader->bind();

// Set uniforms using cached locations
glUniformMatrix4fv(shader->uniform("u_viewProjection"), 1, GL_FALSE, &vp[0][0]);
glUniform3fv(shader->uniform("u_chunkOffset"), 1, &offset[0]);
glUniform1f(shader->uniform("u_time"), worldTime);
glUniform1i(shader->uniform("u_textureAtlas"), 0);  // Texture unit
```

### 5.3 Common Uniform Names

| Uniform | Type | Description |
|---------|------|-------------|
| `u_model` | mat4 | Model matrix |
| `u_view` | mat4 | View matrix |
| `u_projection` | mat4 | Projection matrix |
| `u_viewProjection` | mat4 | Combined view-projection |
| `u_modelViewProjection` | mat4 | Combined MVP |
| `u_normalMatrix` | mat3 | Normal transformation |
| `u_cameraPosition` | vec3 | Camera world position |
| `u_time` | float | World time in seconds |

---

## 6. Preprocessor System

### 6.1 Define Injection

Defines from the manifest are injected after the `#version` directive:

```glsl
// Original source
#version 410 core

in vec3 a_position;
// ...

// After preprocessing with defines: { MAX_LIGHTS: 8, ENABLE_AO: true }
#version 410 core
#define MAX_LIGHTS 8
#define ENABLE_AO 1
// (ENABLE_AO: true becomes 1, false becomes 0)

in vec3 a_position;
// ...
```

### 6.2 Include Directive (Future)

Support for `#include` directives:

```glsl
#version 410 core

#include "common/lighting.glsl"
#include "common/noise.glsl"

void main() {
    // Use functions from includes
}
```

**Resolution:**
1. Paths are relative to `assets/shaders/`
2. Absolute paths start with `/`
3. Include guards are NOT automatic (use manual `#ifndef`/`#define`/`#endif` guards)

### 6.3 Preprocessing Implementation

```cpp
std::string ShaderCompiler::preprocess(
    const std::string& source,
    const std::unordered_map<std::string, std::string>& defines
) {
    std::string result;

    // Find #version line
    size_t versionEnd = source.find('\n');
    if (source.substr(0, 8) == "#version") {
        result = source.substr(0, versionEnd + 1);
    } else {
        result = "#version 410 core\n";
        versionEnd = 0;
    }

    // Inject defines
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
    result += source.substr(versionEnd + 1);

    return result;
}
```

---

## 7. Error Handling

### 7.1 Exception Types

```cpp
namespace Rigel::Asset {

/// Base class for shader errors
class ShaderError : public AssetLoadError {
public:
    ShaderError(const std::string& id, const std::string& message)
        : AssetLoadError(id, message) {}
};

/// Shader compilation failed
class ShaderCompileError : public ShaderError {
public:
    ShaderCompileError(const std::string& id,
                       GLenum stage,
                       const std::string& log)
        : ShaderError(id, formatMessage(stage, log))
        , m_stage(stage)
        , m_log(log)
    {}

    GLenum stage() const { return m_stage; }
    const std::string& log() const { return m_log; }

private:
    GLenum m_stage;
    std::string m_log;

    static std::string formatMessage(GLenum stage, const std::string& log);
};

/// Shader program linking failed
class ShaderLinkError : public ShaderError {
public:
    ShaderLinkError(const std::string& id, const std::string& log)
        : ShaderError(id, "Link failed: " + log)
        , m_log(log)
    {}

    const std::string& log() const { return m_log; }

private:
    std::string m_log;
};

} // namespace Rigel::Asset
```

### 7.2 Error Logging

```cpp
void ShaderCompiler::checkCompileErrors(GLuint shader, GLenum stage, const std::string& shaderId) {
    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);

    if (!success) {
        GLint length;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);

        std::string log(length, '\0');
        glGetShaderInfoLog(shader, length, nullptr, log.data());

        spdlog::error("Shader '{}' compilation failed:\n{}", shaderId, log);
        throw ShaderCompileError(shaderId, stage, log);
    }
}
```

---

## 8. Usage Examples

### 8.1 Basic Usage

```cpp
#include <Rigel/Asset/Assets.h>

// Load shader from manifest
auto shader = assets.get<ShaderAsset>("shaders/basic");

// Bind and set uniforms
shader->bind();
glUniformMatrix4fv(shader->uniform("u_mvp"), 1, GL_FALSE, &mvp[0][0]);

// Draw
glDrawArrays(GL_TRIANGLES, 0, vertexCount);
```

### 8.2 Voxel Rendering

```cpp
// Load opaque and transparent variants
auto opaqueShader = assets.get<ShaderAsset>("shaders/voxel_opaque");
auto transparentShader = assets.get<ShaderAsset>("shaders/voxel_transparent");

// Render opaque geometry
opaqueShader->bind();
glUniformMatrix4fv(opaqueShader->uniform("u_viewProjection"), 1, GL_FALSE, &vp[0][0]);
glUniform1f(opaqueShader->uniform("u_time"), worldTime);

for (const auto& chunk : opaqueChunks) {
    glUniform3fv(opaqueShader->uniform("u_chunkOffset"), 1, &chunk.offset[0]);
    chunk.draw();
}

// Render transparent geometry
glEnable(GL_BLEND);
glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

transparentShader->bind();
glUniformMatrix4fv(transparentShader->uniform("u_viewProjection"), 1, GL_FALSE, &vp[0][0]);
glUniform1f(transparentShader->uniform("u_time"), worldTime);

for (const auto& chunk : transparentChunks) {
    glUniform3fv(transparentShader->uniform("u_chunkOffset"), 1, &chunk.offset[0]);
    chunk.draw();
}
```

### 8.3 Compute Shader

```cpp
auto computeShader = assets.get<ShaderAsset>("shaders/light_propagate");

computeShader->bind();
glUniform1i(computeShader->uniform("u_lightVolume"), 0);

// Dispatch compute
glDispatchCompute(chunkSize / 8, chunkSize / 8, chunkSize / 8);
glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
```

---

## Appendix A: File Structure

```
include/Rigel/Asset/
├── Types.h          # Add ShaderAsset struct
└── ShaderCompiler.h # Compilation utilities (internal)

src/asset/
├── ShaderAsset.cpp    # ShaderAsset implementation
└── ShaderCompiler.cpp # Compilation implementation

assets/shaders/
├── basic.vert
├── basic.frag
├── voxel.vert
├── voxel_opaque.frag
├── voxel_transparent.frag
└── common/
    ├── lighting.glsl
    └── noise.glsl
```

---

## Appendix B: Sample Shader Files

### basic.vert

```glsl
#version 410 core

layout(location = 0) in vec3 a_position;
layout(location = 1) in vec2 a_texCoord;

out vec2 v_texCoord;

uniform mat4 u_mvp;

void main() {
    v_texCoord = a_texCoord;
    gl_Position = u_mvp * vec4(a_position, 1.0);
}
```

### basic.frag

```glsl
#version 410 core

in vec2 v_texCoord;

out vec4 fragColor;

uniform sampler2D u_texture;

void main() {
    fragColor = texture(u_texture, v_texCoord);
}
```

### voxel.vert

```glsl
#version 410 core

layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_texCoord;
layout(location = 3) in float a_textureIndex;
layout(location = 4) in float a_ao;

out vec3 v_worldPos;
out vec3 v_normal;
out vec2 v_texCoord;
out float v_textureIndex;
out float v_ao;

uniform mat4 u_viewProjection;
uniform vec3 u_chunkOffset;

void main() {
    vec3 worldPos = a_position + u_chunkOffset;

    v_worldPos = worldPos;
    v_normal = a_normal;
    v_texCoord = a_texCoord;
    v_textureIndex = a_textureIndex;
    v_ao = a_ao;

    gl_Position = u_viewProjection * vec4(worldPos, 1.0);
}
```
