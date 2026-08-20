# Shader Loading System

Shaders are assets: `AssetManager` records their manifest entries,
`ShaderLoader` loads embedded GLSL sources, `ShaderCompiler` compiles and links
them, and `ShaderAsset` owns the resulting OpenGL program.

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

Every shader entry declares one vertex source and one fragment source:

```yaml
assets:
  shaders:
    voxel:
      vertex: shaders/voxel.vert
      fragment: shaders/voxel.frag
```

Both fields must be present and nonempty. `ShaderLoader` validates the complete
entry before reading either resource, reports a missing stage with
`AssetLoadError`, and rejects fields other than `vertex` and `fragment`.

## Compile and Link Flow

`ShaderLoader::load()` follows one path for every shader program:

1. Validate the manifest fields and required source paths.
2. Read the vertex and fragment bytes through `LoadContext::loadResource()` and
   `ResourceRegistry`.
3. Pass both sources unchanged to `ShaderCompiler`.
4. Compile the sources as `GL_VERTEX_SHADER` and `GL_FRAGMENT_SHADER`.
5. Attach both stages, link the program, then detach and delete the stage
   objects.
6. Store the program in a `ShaderAsset`.

`ShaderCompileError` contains the asset ID, failing OpenGL stage, and compiler
log. `ShaderLinkError` contains the asset ID and linker log. Both derive from
`AssetLoadError`.

## GLSL Runtime Contract

Rigel creates an OpenGL 4.1 core context and uses GLSL 4.10. Every shipped
shader source begins with `#version 410 core`; the shader compiler does not
modify source text or supply a version directive.

A mechanical test walks all shader entries in the embedded manifest, verifies
that both referenced sources declare the core profile, and checks that the
declared GLSL version does not exceed the application runtime version. A
context-backed test also loads every shipped program and compiles and links it
when a compatible OpenGL context is available.

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

Voxel render layers share one main program and select layer behavior through
uniforms.

## Current Limitations

- Shader assets are loaded once and cached; there is no source hot reload.
- Shader source inclusion is not implemented.

---

## Related Docs

- `docs/AssetSystem.md`
- `docs/RenderingPipeline.md`
- `docs/EmbeddedAssets.md`
