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

The build composes two physical roots into one logical registry:

- tracked `assets/` contains Rigel-authored content;
- ignored `.rigel/assets/` contains deterministic derivatives generated from a
  developer-provided Cosmic Reach JAR.

Paths are relative to their physical root, so both roots can supply logical
paths such as `shaders/voxel.vert` or `blocks/base__dirt.yaml`. A duplicate
logical path is a configuration error rather than an override. The asset
manager reads the resulting bytes from `ResourceRegistry::Get(path)`.

The JAR, generated assets, and provenance file are never Git-owned. See
`docs/AssetOwnership.md` for the preparation and provenance contract.

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
entries. `BlockLoader` separately discovers normalized
`models/blocks/*.yaml` resources before `blocks/*.yaml` resources. It validates
the complete reusable-model, block-definition, and referenced-texture set
before publishing the block registry. Failed aggregate loads also discard atlas
entries added by that load. Blocks bind their own texture paths to immutable
model texture slots. The importer resolves source model parents and texture
aliases before publication. Texture-only children share one normalized cuboid
asset and retain block-local texture bindings; runtime code never reads the
source model format. Bounds and inflation are converted to cell units without
clamping, and only declared faces are emitted with their UV, AO, culling, and
shading metadata. Before publication, generated-tree validation parses every
normalized model and rejects duplicate resource or model identities, malformed
primitives, unresolved models or texture slots, invalid orientations,
block/model identifier collisions, and missing texture resources. Explicit
plane geometry and incompatible texture dimensions are omitted under separate
provenance reasons. A block registration may orient its shared model with one
of the measured right-angle turns: X 90/270, Y 90/180/270, or Z 90. Other angle
triples and multi-axis compositions are rejected. Positive 90-degree turns
map `+Y` to `-Z` around X, `+X` to `+Z` around Y, and `+X` to `-Y` around Z.
The optional `rotate_top_bottom` field preserves the separately authored UV
correction for X 90 and Z 90 registrations. It is not inferred from model
orientation. The built-in full cube remains the specialized default, while
normalized cuboids are emitted into the same render-layer chunk batches.
Cube-style AO is used only when a model face requests it and spans a full
unit-cell boundary. All other model faces use conservative unoccluded vertex
values. Non-boundary model faces are not culled from cell-neighbor occupancy,
nor are faces that extend beyond a full neighbor's tangential coverage.
Model-model adjacency remains conservative, and `none` represents explicit
empty geometry. In an interactive build block resources are generated by
`scripts/rigel_assets.py`; normal tests use small synthetic definitions and do
not depend on Cosmic Reach content.

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
- Generated CR content is a configure-time input, not a runtime filesystem
  override; changes require reconfiguration and a rebuild.

---

## Related Docs

- `docs/EmbeddedAssets.md`
- `docs/AssetOwnership.md`
- `docs/CosmicReachImportParity.md`
- `docs/ShaderSystem.md`
- `docs/EntitySystem.md`
- `docs/ConfigurationSystem.md`
