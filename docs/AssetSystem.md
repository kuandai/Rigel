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
    world_config:
      path: config/world_generation.yaml
  textures:
    stone:
      path: textures/blocks/stone/stone_shale.png
      filter: nearest
  shaders:
    voxel:
      vertex: shaders/voxel.vert
      fragment: shaders/voxel.frag
      defines:
        ENABLE_AO: true
```

Manifest constraints in the current implementation:
- `AssetManager::loadManifest()` can be called multiple times; entries are
  merged and later declarations override earlier IDs.
- `imports:` is not implemented.
- All `path` values refer to embedded assets.

## Asset IDs

Asset IDs are always `category/name` (for example: `textures/stone`,
`shaders/voxel`, `raw/world_config`).

The manifest category determines which loader handles the asset.

## Built-in Categories and Loaders

| Category | Loader | Notes |
|----------|--------|------|
| `raw` | `RawLoader` | Loads embedded bytes into `RawAsset`. |
| `textures` | `TextureLoader` | Loads PNG data into OpenGL textures. |
| `shaders` | `ShaderLoader` | Loads + compiles GLSL programs. |
| `input` | `InputBindingsLoader` | Parses input bindings from YAML. |
| `entity_models` | `EntityModelLoader` | Loads entity models (JSON/YAML). |
| `entity_anims` | `EntityAnimationSetLoader` | Loads entity animations. |

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
2. Each manifest entry becomes an `AssetEntry` containing the category and a
   copied YAML subtree (so config nodes stay valid after parsing).
3. `AssetManager::get<T>(id)`:
   - Checks the cache (keyed by type + id).
   - Uses the category loader if registered.
   - Falls back to built-in raw/texture loaders if no loader is registered.
   - Caches the loaded asset and returns a `Handle<T>`.

The cache is type-specific: the same asset id can be loaded as different types
if requested incorrectly, which will fail with a type mismatch.

## Shader-Specific Behavior

Shader assets support:
- Inheritance via `inherit:` (ShaderLoader merges parent + child config).
- Preprocessor defines via `defines:`.
- Fragment fallback when `fragment` is missing and `vertex` ends in `.vert`.

Both `AssetManager` and `ShaderLoader` attempt to supply a missing fragment
path by swapping `.vert` -> `.frag`.

## Error Handling

The asset system throws typed exceptions:
- `AssetNotFoundError`: ID not found in the manifest registry.
- `AssetLoadError`: Missing path, invalid config, or loader failure.
- `ShaderCompileError` / `ShaderLinkError`: GLSL compilation/link failures.

Most loader errors are fatal at call-site and should be handled by the caller.

## Asset Audit Tool

Rigel supports a headless audit mode for comparing embedded Rigel assets against
a CR asset dump without creating a window or OpenGL context.

Usage:

```bash
./build/bin/Rigel --asset-audit /path/to/cr/root --output /tmp/asset_audit.json
```

Alternative CR root source:

```bash
RIGEL_CR_ASSET_ROOT=/path/to/cr/root ./build/bin/Rigel --asset-audit
```

Current behavior:
- Left source: embedded Rigel assets (`ResourceRegistry`).
- Right source: CR filesystem tree (expects `<root>/base/...`, or `root` may be
  the `base` directory itself).
- Output: deterministic JSON report containing inventories and set-diffs for:
  - block roots
  - block variants
  - model refs
  - texture refs
  - entity defs
  - item defs
- Duplicate block variant identifiers are reported per source.

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
