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

Block definitions additionally flow through a canonical intermediate
representation (IR) before runtime registration:
- Source-specific compilers (`Rigel` embedded YAML, `CR` filesystem JSON)
  emit `AssetGraphIR`.
- Validation runs on IR and reports source path + identifier + field details.
- Runtime block registration consumes IR state entries, preserving deterministic
  ordering and centralized validation behavior.
- Block-state model references are normalized before registration:
  - built-ins (`cube`, `cross`, `slab`) are lowercased
  - namespace prefixes are stripped from asset paths
  - `./` and leading `/` path prefixes are removed
- Block-state registration is sorted by (`rootIdentifier`, `state.identifier`,
  `sourcePath`) before assigning runtime IDs.
- Render-layer values are normalized to lowercase and default by opacity:
  - `opaque` when `isOpaque == true`
  - `transparent` when `isOpaque == false`

### CR Block IR Compiler

The CR compiler path (`compileCRFilesystem`) now performs real block-state
expansion instead of only scanning `stringId` values:
- Parses CR block JSON from `base/blocks/**.json` with lenient handling
  (comments + trailing commas tolerated).
- Applies `defaultParams` and `blockStates` keys to produce expanded state IDs.
- Reads generator definitions from `base/block_state_generators/**.json` and
  expands supported `stateGenerators` include chains.
- Applies per-state and generator overrides for fields used by Rigel runtime
  (`modelName`, `isOpaque`, `isSolid`/`walkThrough`, `lightAttenuation`,
  `renderLayer`).
- Emits deterministic canonical state identifiers (sorted param keys) and
  preserves CR-facing external ordering through reversible alias entries.
- Emits compile diagnostics for unsupported generators, generator include
  cycles, conflicting canonical collisions, and malformed files.
- Inventories `models`, `materials`, and `textures` with deterministic sort
  order to support stable validation and audits.

### CR Entity + Item IR Compiler

`compileCRFilesystem` also ingests entity and item definitions into IR:

- Entity sources:
  - `base/entities/**`
  - `base/mobs/**`
  - `base/models/entities/**` (fallback entity IDs)
- Item sources:
  - `base/items/**`
- Both JSON and YAML are accepted with the same lenient parser path used by
  CR block ingestion.

Entity fields currently mapped into IR:

- identifier (`id`, `identifier`, `stringId`, `entityTypeId`, or path fallback)
- model reference (`modelRef`, `modelId`, `modelName`, `model`, render map fallbacks)
- animation reference (`animationRef`, `animationSet`, `animation_set`,
  `animationId`, `animation`)
- render mode (`renderMode` / `lighting`)
- optional `renderOffset`
- optional hitbox (`min`/`max`)

Item fields currently mapped into IR:

- identifier (`id`, `identifier`, `stringId`, or path fallback)
- model reference (`modelRef`, `modelId`, `modelName`, `model`)
- texture reference (`textureRef`, `texture`, `icon`)
- render mode (`renderMode`, `modelType`)
- `itemProperties` fallback extraction for model/texture/render keys

Unknown/source-specific fields are preserved in IR extension metadata instead
of being discarded.

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

IR validation diagnostics are summarized at startup with:
- total error/warning counts
- top-N sampled issues (with source path, identifier, and field)
- omitted diagnostic count when the full set is larger than the sample limit

## Block Texture Channel Fallbacks

When registering block states from IR, texture assignment follows this order:

- Uniform texture:
  - `all`
  - else `default`
  - else `albedo`
  - else `diffuse`
- Top/bottom/sides triplet:
  - `top`, `bottom`, and (`sides` or `side`)
- Per-face override keys:
  - `north`, `south`, `east`, `west`, `up`, `down`

Missing texture files are logged as warnings and do not abort registration of
other block states.

## IR Validation Severity Policy

- Unresolved model references are validation `Error`s.
- Unresolved texture references are validation `Warning`s.
- Render-layer and opacity mismatches are validation `Warning`s because they
  are legal but often indicate sorting/culling mistakes in authored content.
- Alias mapping collisions are validation `Error`s:
  - one external ID mapping to multiple canonical IDs
  - one canonical ID mapping to multiple external IDs

## Registry Determinism Contract

Runtime `BlockRegistry` identity is deterministic:

- Runtime ID assignment follows sorted block-state registration order, not
  filesystem traversal order.
- `BlockRegistry::snapshotHash()` computes a deterministic hash over
  schema-relevant fields in runtime ID order.
- Snapshot hash excludes transient runtime payloads (for example `customData`)
  so it remains stable for equivalent content.

Entity and item runtime registries follow the same contract:

- `EntityTypeRegistry` and `ItemDefinitionRegistry` are built from IR in sorted
  identifier order.
- Duplicate identifiers are diagnosed and skipped deterministically.
- Snapshot hashes are emitted for deterministic parity checks.

## CR Field Support Matrix

Current support is intentionally scoped to fields required for runtime
registration, rendering selection, and persistence identity continuity.

| Category | Supported fields (current) | Behavior |
|----------|-----------------------------|----------|
| Blocks | `stringId`, `defaultParams`, `defaultProperties`, `blockStates`, `stateGenerators` (subset), `modelName`, `renderLayer`, `isOpaque`, `isSolid`, `walkThrough`, `lightAttenuation` | Expanded deterministically into canonical state IDs + alias map. |
| Entities | `id`/`identifier`/`stringId`/`entityTypeId`, model refs, animation refs, `renderMode`/`lighting`, `renderOffset`, `hitbox` | Parsed to IR entity definitions; unknown fields preserved in extensions. |
| Items | `id`/`identifier`/`stringId`, model refs, texture refs, `renderMode`/`modelType`, `itemProperties` model/texture/render fields | Parsed to IR item definitions; unknown fields preserved in extensions. |

## Unsupported/Partial CR Behavior

- Block generators are partial: unsupported generators emit diagnostics and are
  not expanded.
- Unknown/extra CR fields are preserved in IR extensions but not interpreted by
  runtime registration unless explicitly mapped.
- Entity/item runtime behavior still depends on existing Rigel runtime systems
  (factory/render/persistence); IR ingestion alone does not add new gameplay
  semantics.

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
- CR parser coverage is schema-subset based, not full spec-complete.
- Unsupported CR generators/properties are surfaced via diagnostics, not silent
  behavior changes.

## Troubleshooting Parity Failures

- Symptom: blocks/entities/items silently missing after compile.
  - Check validation output for duplicate identifiers and unresolved refs.
  - Run `--asset-audit` to compare inventories against CR dump source.
- Symptom: CR states differ run-to-run.
  - Verify deterministic expansion tests and snapshot hash tests are passing.
  - Ensure source file traversal order is sorted in compiler paths.
- Symptom: persistence round-trip drops unknown IDs.
  - Verify `UnknownIdPolicy` settings and associated policy tests.
  - Confirm identity provider wiring is present in persistence context.

---

## Related Docs

- `docs/EmbeddedAssets.md`
- `docs/ShaderSystem.md`
- `docs/EntitySystem.md`
- `docs/ConfigurationSystem.md`
