# Shader Loading System

This document describes the implemented manifest-to-OpenGL shader path.
Shaders are assets: `AssetManager` resolves their manifest entries,
`ShaderLoader` loads embedded GLSL sources, `ShaderCompiler` compiles and links
them, and `ShaderAsset` owns the resulting program.

## Registration and Loading

`AssetManager::loadManifest()` registers the built-in `raw`, `textures`, and
`shaders` loaders when no loaders have been registered yet. Manifest entries
are recorded during that call, but a shader is compiled lazily on the first
typed request:

```cpp
auto shader = assets.get<Asset::ShaderAsset>("shaders/voxel");
shader->bind();
```

The returned handle and underlying asset are cached by `AssetManager`. Clearing
the asset cache releases a shader program when no other handle retains it.
Loading, use, and final release of shader assets require the appropriate OpenGL
context on the calling thread.

## Manifest Format

A graphics shader declares a vertex and fragment source. A geometry source and
compile-time definitions are optional:

```yaml
assets:
  shaders:
    example:
      vertex: shaders/example.vert
      fragment: shaders/example.frag
      geometry: shaders/example.geom
      defines:
        USE_FOG: true
        SAMPLE_COUNT: 4
```

If `fragment` is absent and the vertex path ends in `.vert`, the asset path is
derived by replacing that suffix with `.frag`. Otherwise a missing vertex or
fragment source produces `AssetLoadError`.

The loader also accepts a standalone `compute` source instead of the graphics
stages. `ShaderCompiler` compiles it as `GL_COMPUTE_SHADER`, but the application
creates an OpenGL 4.1 core context and does not declare compute assets. Compute
shaders require OpenGL 4.3, so this path is not usable in the current runtime.

## Shader Inheritance

An entry can inherit source paths and definitions from another shader entry:

```yaml
assets:
  shaders:
    base:
      vertex: shaders/base.vert
      fragment: shaders/base.frag
      defines:
        USE_FOG: false

    fogged:
      inherit: shaders/base
      defines:
        USE_FOG: true
```

Resolution is recursive. The child overrides any explicitly supplied vertex,
fragment, geometry, or compute path. Definitions merge by name, with the child
value winning. `inherit` uses the full asset ID, such as `shaders/base`.

A missing parent produces `AssetLoadError`. Inheritance cycles are not detected
by the current resolver and must not appear in the manifest.

## Compile and Link Flow

For a graphics entry, `ShaderLoader::load()` performs this path:

1. Resolve inherited paths and definitions.
2. Read vertex, fragment, and optional geometry bytes through
   `LoadContext::loadResource()` and `ResourceRegistry`.
3. Preprocess each stage with the same definition map.
4. Compile each stage and attach it to a new OpenGL program.
5. Link the program, then detach and delete successfully compiled stage
   objects.
6. Store the program in a `ShaderAsset`.

`ShaderCompileError` contains the asset ID, failing OpenGL stage, and compiler
log. `ShaderLinkError` contains the asset ID and linker log. Both derive from
`AssetLoadError`.

## Definition Preprocessing

`ShaderCompiler::preprocess()` preserves a `#version` directive only when it
starts at the first byte of the source. It then inserts manifest definitions
immediately after that line. Lowercase string values `true` and `false` become
`1` and `0`; other values are inserted verbatim.

For example, `USE_FOG: true` and `SAMPLE_COUNT: 4` produce:

```glsl
#define USE_FOG 1
#define SAMPLE_COUNT 4
```

When the source does not begin with `#version`, preprocessing prepends
`#version 410 core`. Rigel's shipped shader sources also explicitly declare
`#version 410 core`.

The application, compiler fallback, and ImGui backend share an OpenGL 4.1 core
and GLSL 4.10 runtime contract. A mechanical test walks the shader entries in
the embedded manifest and checks each referenced stage declaration against that
supported GLSL version.

The preprocessor does not resolve `#include` directives.

## ShaderAsset Ownership and Lookups

`ShaderAsset` owns one OpenGL program and deletes it in `release()` or its
destructor. It is movable and non-copyable. `bind()` calls `glUseProgram()`.

Uniform and attribute locations are queried by name and cached independently:

```cpp
shader->bind();
GLint viewProjection = shader->uniform("u_viewProjection");
glUniformMatrix4fv(viewProjection, 1, GL_FALSE, matrixData);
```

A missing uniform or attribute returns `-1`, is cached, and produces a warning.
Renderers commonly cache frequently used locations again in renderer state so
they do not repeat even the asset-level map lookup during draw passes.

## Current Shader Assets

The shipped manifest declares graphics programs for:

- Voxel and entity main passes
- Voxel and entity shadow depth passes
- Transparent voxel shadow transmittance
- Temporal-AA resolve
- Chunk, entity-bounds, and frame-graph debug overlays

All current entries use vertex and fragment stages. Voxel render layers share
one main program and select layer behavior through uniforms rather than shader
inheritance variants.

## Current Limitations

- Shader assets are loaded once and cached; there is no source hot reload.
- The preprocessor supports definition injection but not includes.
- Inheritance cycles are not detected.
- The loader has a compute-source path, but the OpenGL 4.1 application context
  does not provide the OpenGL 4.3 compute-shader runtime.

---

## Related Docs

- `docs/AssetSystem.md`
- `docs/RenderingPipeline.md`
- `docs/EmbeddedAssets.md`
