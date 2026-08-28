# Asset System

This document describes the current asset system as implemented in Rigel. It
focuses on the runtime behavior (what exists today), not future design goals.

## Overview

The asset system is built around `AssetManager` and an embedded asset registry:

- Assets are declared in a YAML manifest (`assets/manifest.yaml`).
- Manifest files are loaded from embedded resources (`ResourceRegistry`).
- Assets are loaded lazily on first access and cached by type + id.
- Loaders are registered per asset category (`textures`, `shaders`, etc.).
- The system is main-thread only and not thread-safe.

## Data Sources

### Embedded Resources (ResourceRegistry)

All asset files are embedded into the binary via the build system. The asset
manager reads bytes from `ResourceRegistry::Get(path)`.

See `docs/EmbeddedAssets.md` for details on the embedded registry.

### Relationship to Embedded Assets

`AssetManager` reads asset bytes from the embedded `ResourceRegistry`. The
embedded registry is responsible for packaging files into the binary, while
the asset system parses the manifest and loads typed assets from those bytes.

### Asset Manifest

The manifest defines asset IDs and their configuration. Example:

```yaml
namespace: base
assets:
  raw:
    lookup_table:
      path: data/lookup.bin
  textures:
    stone:
      path: textures/blocks/stone/stone_shale.png
      filter: nearest
  shaders:
    voxel:
      vertex: shaders/voxel.vert
      fragment: shaders/voxel.frag
```

Manifest constraints in the current implementation:
- Only a single manifest file is loaded (`manifest.yaml`).
- `imports:` is not implemented.
- All `path` values refer to embedded assets.

## Asset IDs

Asset IDs are always `category/name` (for example: `textures/stone`,
`shaders/voxel`, `raw/lookup_table`).

The manifest category determines which loader handles the asset.

## Built-in Categories and Loaders

| Category | Loader | Notes |
|----------|--------|------|
| `raw` | `RawLoader` | Loads embedded bytes into `RawAsset`. |
| `textures` | `TextureLoader` | Loads PNG data into OpenGL textures. |
| `shaders` | `ShaderLoader` | Loads + compiles GLSL programs. |
| `input` | `InputBindingsLoader` | Strictly parses symbolic keyboard/mouse binding tokens and lists. |
| `entity_models` | `EntityModelLoader` | Loads entity models (JSON/YAML). |
| `entity_anims` | `EntityAnimationSetLoader` | Loads entity animations. |
| `generator_definitions` | `GeneratorDefinitionLoader` | Strictly loads the complete named generator-definition set. |

The default loaders (`raw`, `textures`, `shaders`) are registered automatically
when `AssetManager::loadManifest()` is called. Other loaders must be registered
explicitly in application startup.

## Embedded Category Scanning

In addition to manifest entries, `AssetManager` scans embedded assets and
registers the following categories automatically:

- `entity_models`: `models/entities/*.json|*.yaml`
- `entity_anims`: `animations/entities/*.json|*.yaml`
- `textures`: `textures/**/*.png`

When scanning, it tries to read `id` or `name` fields from JSON/YAML files to
use as the asset name. If missing, the path (sans prefix/suffix) is used.

This allows entity assets and textures to be used without explicit manifest
entries.

## Load Flow and Caching

1. `AssetManager::loadManifest()` parses `manifest.yaml` from embedded assets.
2. It installs the built-in raw, texture, and shader loader independently for
   each category that has not already been replaced explicitly.
3. Each manifest entry becomes an `AssetEntry` containing the category and a
   copied YAML subtree (so config nodes stay valid after parsing).
4. `AssetManager::get<T>(id)`:
   - Checks the cache (keyed by type + id).
   - Requires and dispatches through the loader registered for the entry's
     category.
   - Caches the loaded asset and returns a `Handle<T>`.

The cache is type-specific: the same asset id can be loaded as different types
if requested incorrectly, which will fail with a type mismatch.

Generator declarations are replaced transactionally. A candidate manifest is
not made visible until its complete definition set and referenced resources
load and validate successfully. Duplicate declarations, malformed entries,
missing resources, or aggregate validation failures preserve the preceding
namespace, declarations, error state, and cached set; a later corrected load
can replace that state.

## Shader-Specific Behavior

Shader assets support:
- One required `vertex` source.
- One required `fragment` source.

The shader loader validates both paths before loading either source and rejects
other fields in shader entries.

## Error Handling

The asset system throws typed exceptions:
- `AssetNotFoundError`: ID not found in the manifest registry.
- `AssetLoadError`: Missing path, invalid config, or loader failure.
- `ShaderCompileError` / `ShaderLinkError`: GLSL compilation/link failures.

Loader errors are classified by their owning startup lifecycle. The voxel shader
is required and aborts view creation on failure. Voxel-shadow, entity-shader,
entity-shadow, and default-input assets are optional: absence and load failure
take the same documented fallback, emit one warning naming the asset id, and do
not publish partially initialized optional state. ImGui follows the same
optional startup policy at its application boundary.

## Threading + GL Context Requirements

- `AssetManager` is not thread-safe.
- Loaders are expected to run on the main thread.
- `TextureAsset` and `ShaderAsset` creation/destruction require a valid OpenGL
  context.

## Extending the Asset System

To add a new asset category:

1. Implement an `IAssetLoader` subclass.
2. Register it with `AssetManager::registerLoader(category, loader)`.
3. Add entries to the manifest under that category.

Loaders receive:
- The asset ID (`category/name`).
- The YAML config subtree for that asset.
- A reference to `AssetManager` (for loading dependencies).

## Limitations (Current State)

- No manifest imports or dependency resolution beyond manual loader calls.
- No hot reload.
- No filesystem override for embedded assets; changes require a rebuild.

---

## Related Docs

- `docs/EmbeddedAssets.md`
- `docs/ShaderSystem.md`
- `docs/EntitySystem.md`
- `docs/ConfigurationSystem.md`
