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
`models/blocks/*.yaml` resources before `blocks/*.yaml` resources.

## Normalized block models

Normalized block models are Rigel assets, not a runtime representation of the
Cosmic Reach model language. The importer resolves compatible source parent
graphs, inherited geometry, texture aliases, block-state generators, and
inflation before writing them. Runtime code only parses the resulting
`models/blocks/*.yaml` and `blocks/*.yaml` resources; it never reads the source
JAR or CR JSON.

Importer-generated models live under the ignored `.rigel/assets/` root and are
replaced only through the synchronized asset workflow; they are not hand-edited
or tracked. Rigel-authored content may use the same normalized format under the
tracked `assets/` root.

The model document has exactly three root fields:

```yaml
id: base:block_model/example_post
texture_slots: [post, cap]
cuboids:
  - bounds: [0.375, 0.0, 0.375, 0.625, 1.125, 0.625]
    faces:
      pos_x:
        texture: post
        uv: [0.25, 0.0, 0.75, 1.0]
        rotation: 90
        shading: pos_x
        ambient_occlusion: false
        cull: false
      pos_y:
        texture: cap
```

- `id` is the reusable model identity. A block registration refers to it from
  its `model` field and binds every declared `texture_slots` entry to a block
  texture path. Texture-only source children therefore share geometry while
  retaining block-local texture bindings.
- `cuboids` is a non-empty list of axis-aligned bounds in block-cell units,
  ordered as minimum X/Y/Z then maximum X/Y/Z. Imported inflation is already
  folded into these values. Bounds are finite but deliberately are not clamped
  to `[0, 1]`, so inflated and piston geometry may cross cell boundaries.
- `faces` contains only the cardinal faces that exist. Omitting a face omits
  its quad. A face selects a declared texture slot and may carry a normalized
  `[u0, v0, u1, v1]` rectangle, a 0/90/180/270-degree UV rotation, a separate
  shading direction, and authored AO and neighbor-culling flags. UV endpoints
  may be reversed, so cropping, mirroring, and quarter turns survive import.
  Rotation follows the normalized face-local convention; the mesher maps its
  face-specific corner order through Rigel's vertically flipped texture atlas.
  Defaults are the full UV rectangle, no rotation, geometric face shading,
  conservative AO, and no neighbor culling.

A block registration may orient shared geometry only with the right-angle
states measured in the source: X 90/270, Y 90/180/270, or Z 90, plus identity.
Other angle triples and all multi-axis compositions are rejected. Positive
90-degree turns map `+Y` to `-Z` around X, `+X` to `+Z` around Y, and `+X` to
`-Y` around Z. The optional `rotate_top_bottom` field preserves a separately
authored UV correction for X 90 and Z 90 registrations; it is not inferred
from orientation.

### Normalized block collision shapes

Physical block geometry is authored on the block registration, independently
from its visual model. The required `collision` field accepts the compact
scalars `none` and `full`, or one or more inline axis-aligned boxes:

```yaml
id: base:example_post
model: base:block_model/example_post
collision:
  boxes:
    - [0.375, 0.0, 0.375, 0.625, 1.0, 0.625]
    - [-0.25, 0.875, 0.25, 1.25, 1.0, 0.75]
textures:
  post: textures/blocks/example_post.png
  cap: textures/blocks/example_cap.png
```

Box coordinates are ordered as minimum X/Y/Z then maximum X/Y/Z. Every box
must have finite coordinates, positive volume, and be unique within the shape.
Coordinates may extend by at most 0.25 cell beyond a block boundary, giving an
inclusive supported range of `[-0.25, 1.25]`; values outside that range are
rejected rather than clamped. The collision mapping has no fields other than
`boxes`, and the list must contain between one and 16 boxes. The same maximum
is enforced by normalized-tree generation and validation, strict runtime asset
loading, and direct `BlockCollisionShape` construction.

An explicit collision may carry top-level `collision_provenance` metadata.
Ordinary authored assets omit the field and are recorded as `authored` at
runtime. Normalized imports use `exact` or `conservative_fallback`; the metadata
does not alter the physical shape.

Every normalized block must declare `collision`; omission is rejected. The
normalized format has no separate solidity flag. `BlockType` stores collision
as an immutable `BlockCollisionShape`; empty and full shapes carry no allocated
box storage, while inline boxes are shared through read-only storage and
exposed as a non-owning read-only span. Collision is never derived at runtime
from `BlockModel` cuboids.

The Cosmic Reach importer always writes an explicit collision snapshot for
each published registration and marks its per-registration provenance `exact`.
A source `walkThrough` state becomes `none`, even when it has visible geometry.
Other states use the positive-volume cuboids from the fully resolved source
model. Imported inflation is already folded into the bounds, zero-volume render
helpers are discarded, and the registration's orthogonal state orientation is
applied before the boxes are written. A single unit-cell box is canonicalized
to `full`; all other boxes remain inline on the registration. This use of
resolved visual cuboids is confined to import time. The normalized collision
asset and runtime type remain independent from the normalized visual model.

Collision import provenance records a support schema and disjoint shape counts
for empty, full, single-partial, and multi-box registrations. It also reports
exact derivations, conservative full-cube fallbacks, and ambiguous source
constructs. Published shapes are currently required to be exact derivations,
so the fallback and ambiguity counts must both be zero. Validation reconstructs
all seven counts from that publication policy and the published tree, rejecting
plausible-looking derivation-count changes as well as stale shape counts. Source
geometry outside the supported overhang range fails instead of being clamped.

For the validated Cosmic Reach 0.6.1 input, the 2,021 published registrations
resolve to 67 empty shapes, 315 full cubes, 956 single partial boxes, and 683
multi-box shapes. All are exact derivations; fallback and ambiguity counts are
zero. The empty total includes 66 walk-through registrations with visual
geometry. Twenty-four piston-head registrations retain collision boxes
that reach exactly 0.25 cell beyond the owning cell. Stair collision excludes
the source models' zero-volume render helpers.

The format supports the currently measured single- and multiple-cuboid models.
It is not a scene graph or a general transform/model format. Plane primitives,
animated block textures, and non-16-by-16 block textures are not supported.
The importer omits those cases with disjoint provenance reasons rather than
publishing an approximation or claiming arbitrary CR-model compatibility.

### Opacity and texture alpha

The importer makes three separate decisions rather than treating source
transparency as one material flag:

1. The registration's `opaque` value comes from an explicit source `isOpaque`
   property when present, otherwise from the inverse of the resolved model's
   `isTransparent`. This value controls whole-block occlusion behavior; it does
   not select alpha blending.
2. Every referenced PNG is decoded and classified as fully opaque, binary
   alpha (only zero and fully opaque texels), or fractional alpha. The
   canonical render layers are respectively `opaque`, `cutout`, and
   `transparent`.
3. A source `refractiveIndex` can promote a binary-alpha texture slot from
   `cutout` to `transparent`. It never promotes a fully opaque slot. Source
   fluid and model-transparency flags do not otherwise override the PNG class.

Layer selection remains per texture slot for normalized models. The generated
block document stores the most common effective slot layer as `layer`, using
the stable `opaque`, `cutout`, `transparent` order to break a tie, and records
only differing slots in `texture_render_layers`. This is a compact encoding;
the runtime resolves every face through its named slot and batches it directly
into the corresponding existing chunk layer range. A mixed wood/glass model
therefore keeps its wood opaque while its glass blends. A built-in full cube
cannot express per-slot overrides, so an import whose cube faces require
conflicting layers fails instead of collapsing them to one material.

The foreground shader preserves the sampled alpha for the `transparent`
range. `cutout` performs the existing 0.5 alpha test and writes depth, while
`opaque` does not blend. The renderer does not apply a generic transparent
alpha multiplier and this contract does not introduce a general material
system.

### Publication and runtime semantics

Before publication, generated-tree validation parses every normalized model
and rejects duplicate resource/model identities, malformed cuboids, unresolved
models or texture slots, invalid orientations, block/model identifier
collisions, missing texture resources, malformed or unsupported referenced PNG
data, and render layers that contradict the decoded PNG alpha class.
`BlockLoader` then validates the complete reusable-model, block-definition, and
texture set as one candidate. An aggregate failure publishes none of it and
rolls back atlas entries added by that attempt.

Published block registrations retain their model through
`shared_ptr<const BlockModel>`. World initialization freezes the block registry
before mesh workers can read it, so registrations and their transitively held
models remain immutable for the lifetime of asynchronous meshing.

The built-in full cube remains the specialized default and `none` represents
explicit empty geometry. Cuboid faces join the same per-render-layer chunk
batches as cubes; they are not standalone render objects. Face culling requires
actual opposite-boundary coverage, so an opaque partial model does not hide a
whole neighboring cell face. Cube-style AO is used on a normalized face only
when that face requests it and spans an entire unit-cell boundary. Other model
faces start with conservative unoccluded AO.

Visual geometry does not define collision or lighting semantics. `opaque`,
`collision`, `emits_light`, and `light_attenuation` remain properties of the
block registration emitted from source fields. Entity movement sweeps against
the physical collision boxes. Edit targeting is deliberately different: it
intersects the immutable visual model's declared oriented cuboid faces and
never consults `BlockCollisionShape`. The exact contract is documented in
`docs/BlockTargeting.md`.

Interactive block/model resources are generated by `scripts/rigel_assets.py`.
Normal tests use small invented definitions and do not depend on Cosmic Reach
content. World-query and entity-response semantics are documented in
`docs/BlockCollision.md`.

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
