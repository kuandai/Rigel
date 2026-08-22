# Configuration System

This document describes how Rigel loads and merges runtime configuration
(world generation, rendering, and persistence). It reflects the current
implementation and the on-disk configuration files shipped with the project.

---

## Index

- [Overview](#overview)
- [Config Sources and Precedence](#config-sources-and-precedence)
  - [World Generation](#world-generation)
  - [Rendering](#rendering)
  - [Persistence](#persistence)
- [Config Provider and Sources](#config-provider-and-sources)
- [World Generation Config](#world-generation-config)
- [Render Config](#render-config)
- [Persistence Config](#persistence-config)
- [Per-World Overrides](#per-world-overrides)
- [Limitations](#limitations)
- [Related Docs](#related-docs)

---

## Overview

Rigel uses a layered configuration system. Each config type is loaded from a
stack of sources, and later sources override earlier values. Configs are read
once during application bootstrap.

Fields merge according to their YAML shape:

- Scalars replace the earlier value.
- Objects and maps merge by key, so omitted keys retain their earlier values.
- Sequences replace the earlier sequence, including when the later sequence is
  empty.

World generation has one keyed sequence exception: `density_graph.nodes`
merges entries by `id`, replacing a matching node as a whole. Persistence
`providers` is a map keyed by provider ID, and each provider's options merge by
option name.

Four config types are supported today:

- `WorldGenConfig` (world generation)
- `StreamingConfig` (runtime chunk loading, generation, and meshing schedules)
- `WorldRenderConfig` (render pipeline settings)
- `PersistenceConfig` (save/load format and provider options)

Typed providers load each subsystem's settings from YAML input using rapidyaml.
`Voxel::WorldConfigProvider` loads generation and streaming settings together
so their shared overlays have one deterministic order. Rendering is loaded by
`Render::RenderConfigProvider`, and persistence by
`Persistence::PersistenceConfigProvider`. Each subsystem's bootstrap function
uses the shared standard-source builder, but the typed provider remains the
semantic owner of parsing and merging its settings. Unknown fixed keys produce
a warning and are not applied.

---

## Config Sources and Precedence

Each config type has a fixed source order defined by its subsystem bootstrap.
The general rule is:

1) Embedded defaults (from assets).
2) Project-level overrides under `config/`.
3) Project root overrides (for quick testing).
4) Per-world overrides under `config/worlds/<worldId>/`.

### World Generation and Streaming

Sources (in order):

1. `assets/config/world_generation.yaml` (embedded as `raw/world_config`)
2. `config/world_generation.yaml`
3. `world_generation.yaml`
4. `config/worlds/<worldId>/world_generation.yaml`

### Rendering

Sources (in order):

1. `assets/config/render.yaml` (embedded as `raw/render_config`)
2. `config/render.yaml`
3. `render.yaml`
4. `config/worlds/<worldId>/render.yaml`

### Persistence

Sources (in order):

1. `assets/config/persistence.yaml` (embedded as `raw/persistence_config`)
2. `config/persistence.yaml`
3. `persistence.yaml`
4. `config/worlds/<worldId>/persistence.yaml`

---

## Config Providers and Sources

Each typed provider aggregates neutral `Config::IConfigSource` instances:

- `EmbeddedConfigSource` reads an embedded raw asset (e.g. `raw/world_config`).
- `FileConfigSource` reads a file from disk.

Each source provides:

- `load()` -> full YAML file content.
- `loadPath(path)` -> resolution of overlays declared by that source (optional).

Overlay resolution has two behaviors:

- For embedded sources, `assets/` is stripped and the file is fetched from the
  embedded `ResourceRegistry` (e.g. `assets/config/worldgen_overlays/no_carvers.yaml`).
- For file sources, relative paths are resolved against the source file's
  directory (e.g. `config/world_generation.yaml` can reference
  `worldgen_overlays/no_carvers.yaml`).

Each source and its overlays form one precedence layer. A source's base YAML is
applied first, followed by its overlays in declaration order. Loading then
continues with the next source, so project-root and per-world values cannot be
overridden by an overlay from a lower-precedence source. Overlay paths are
resolved only by the source that declared them.

---

## World Generation Config

`WorldConfiguration` holds separate `WorldGenConfig` and `StreamingConfig`
values. Each source and its overlays are applied to both typed configurations
before loading moves to the next source.

Defaults below reflect the code defaults from `WorldGenConfig`. The embedded
config (`assets/config/world_generation.yaml`) overrides many of these values.

### Quick Reference (World Generation)

| Key | Type | Default | Notes |
| --- | --- | --- | --- |
| `seed` | int | `1337` | Global world seed. |
| `solid_block` | string | `base:debug` | Block ID used for solid fill. |
| `surface_block` | string | `base:debug` | Block ID used for surface fill. |
| `world.min_y` | int | `-64` | Minimum world Y coordinate; supported range is `[-4096,4096]`. |
| `world.max_y` | int | `320` | Maximum world Y coordinate; supported range is `[-4096,4096]`. |
| `world.sea_level` | int | `0` | Sea level for water placement; must be inside the world bounds. |
| `world.version` | int | `1` | World generation version. |
| `terrain.base_height` | float | `16.0` | Base terrain height. |
| `terrain.height_variation` | float | `16.0` | Terrain height variation. |
| `terrain.surface_depth` | int | `3` | Surface layer depth. |
| `terrain.density_strength` | float | `0.0` | Adds density noise influence. |
| `terrain.gradient_strength` | float | `1.0` | Vertical density gradient. |
| `terrain.noise.*` | object | - | Base height noise (see below). |
| `terrain.density_noise.*` | object | - | Density noise (see below). |
| `climate.local_blend` | float | `1.0` | Blend factor for local climate. |
| `climate.latitude_scale` | float | `0.0` | Latitude noise scale. |
| `climate.latitude_strength` | float | `0.0` | Latitude influence. |
| `climate.global.*` | object | - | Global climate noise (see below). |
| `climate.local.*` | object | - | Local climate noise (see below). |
| `biomes.blend_power` | float | `2.0` | Biome blend power. |
| `biomes.epsilon` | float | `0.0001` | Blend epsilon. |
| `biomes.coast_band.*` | object | - | Optional coast override. |
| `biomes.entries[]` | list | - | Biome definitions. |
| `density_graph.outputs` | map | - | Output name -> node id. |
| `density_graph.nodes[]` | list | - | Density node graph. |
| `caves.density_output` | string | `cave_density` | Density output name. |
| `caves.threshold` | float | `0.5` | Density threshold. |
| `structures.features[]` | list | - | Simple feature definitions. |
| `generation.stages` | map | all enabled | Boolean stage enable flags. |
| `flags` | map | - | Boolean flags for overlays. |
| `overlays[]` | list | - | Overlay definitions. |

Noise objects (`terrain.noise`, `terrain.density_noise`, `climate.*.*`) use:

| Key | Type | Default |
| --- | --- | --- |
| `octaves` | int | `5` (maximum `16`) |
| `frequency` | float | `0.005` |
| `lacunarity` | float | `2.0` |
| `persistence` | float | `0.5` |
| `scale` | float | `1.0` |
| `offset` | float | `0.0` |

Key top-level fields (see `assets/config/world_generation.yaml` for examples):

- `seed`, `solid_block`, `surface_block`
- `world`: `min_y`, `max_y`, `sea_level`, `version`
- `flags`: boolean map used by overlays
- `terrain`: base heights and noise controls
- `climate`: global/local temperature + humidity + continentalness noise
- `biomes`: biome targets, weighting, and surface layers
- `density_graph`: node graph for terrain density
- `caves`: carver settings
- `structures`: simple feature generation
- `generation.stages`: stage enable map
- `overlays`: conditional config overlays

World bounds are inclusive, ordered, and limited to 1,024 blocks of vertical
span. The coordinate limit allows worlds to be placed within 128 vertical
chunks on either side of the origin, while the span limit bounds the global
surface search to 1,024 density samples per column. Noise declarations are
limited to 16 octaves so every fBm sample has a fixed product-level work bound.
These values are validated before a `WorldGenerator` is constructed.

### Pipeline Stages

`generation.stages` maps fixed stage names to boolean enable flags. Map key
order has no runtime meaning. Unknown stage names are reported and ignored.

Current stage names:

- `climate_global`
- `climate_local`
- `biome_resolve`
- `terrain_density`
- `caves`
- `surface_rules`
- `structures`

Stages default to enabled unless explicitly disabled.
The `generation.stages.caves` flag is the only control that enables or disables
cave carving; the `caves` object contains only cave-stage parameters.

### Flags and Overlays

Overlays are applied after the declaring source's base YAML, in the order they
appear in the `overlays` list. Each overlay is a `{ path, when }` pair:

- `path`: YAML file to load.
- `when`: name of a boolean in `flags` (optional).

If `when` is omitted, the overlay is unconditional. If it names a false or
missing flag, the overlay is skipped. The condition uses the configuration
state at that source layer. Overlays may declare more overlays. Nested
declarations are appended after any sibling overlays already pending for the
source layer, and a path is applied at most once within that layer.

The shipped overlay:

- `assets/config/worldgen_overlays/no_carvers.yaml` disables caves.

---

## Streaming Config

`StreamingConfig` owns runtime chunk loading, generation, and meshing schedule
settings under the `streaming` key. It uses the world generation and streaming
source order above so existing project and per-world overrides retain their
precedence.

| Key | Type | Code fallback | Notes |
| --- | --- | --- | --- |
| `streaming.view_distance_chunks` | int | `6` | Desired chunk radius around the camera (maximum `16`). |
| `streaming.unload_distance_chunks` | int | `8` | Unload radius (maximum `24`); must be at least the view radius. |
| `streaming.gen_queue_limit` | int | `0` | In-flight generation cap (0 = unlimited; maximum explicit cap `32768`). |
| `streaming.mesh_queue_limit` | int | `0` | In-flight mesh cap (0 = unlimited; maximum explicit cap `32768`). |
| `streaming.update_budget_per_frame` | int | `0` | Load/generation/missing-mesh requests advanced per update (0 = unlimited). |
| `streaming.apply_budget_per_frame` | int | `0` | Generation and mesh results applied per category (0 = unlimited). |
| `streaming.worker_threads` | int | `2` | Total worker count partitioned between generation and meshing. |
| `streaming.io_threads` | int | `1` | Region IO thread count. |
| `streaming.load_worker_threads` | int | `2` | Chunk payload build thread count; all three worker settings total at most `64`. |
| `streaming.load_apply_budget_per_frame` | int | `8` | Disk payload apply budget (0 = unlimited). |
| `streaming.load_region_drain_budget` | int | `32` | Region completion drain budget. |
| `streaming.load_queue_limit` | int | `0` | Pending disk load cap (0 = unlimited; maximum explicit cap `32768`). |
| `streaming.load_max_cached_regions` | int | `8` | Cached region cap (0 = disabled, maximum `256`). |
| `streaming.load_max_inflight_regions` | int | `8` | Concurrent region read cap (0 = disabled, maximum `64`). |
| `streaming.load_prefetch_radius` | int | `1` | Region prefetch radius (maximum `4`). |
| `streaming.load_prefetch_per_request` | int | `12` | Prefetch request cap per chunk request (maximum `512` and no more than the selected neighbor cube). |
| `streaming.max_resident_chunks` | int | `0` | Resident chunk cache cap (0 = unlimited, maximum explicit cap `65536`). |

The embedded configuration shipped with Rigel sets the view distance to 12
chunks and the unload distance to 13 chunks. Configurations with an unload
distance below the view distance are rejected. Hysteresis exists only when
`unload_distance_chunks` is greater than `view_distance_chunks`; equal values
evict residents as soon as they leave the desired sphere.

The one-chunk hysteresis was selected with a deterministic lifecycle regression.
It preloads a sparse radius-12 sphere with one solid boundary probe, settles to
quiescence, moves one chunk on the X axis, settles again, reverses to the origin,
and settles a third time. The two runs differ only in unload distance, and no
terrain generation is involved. With unload distance 12, the move entered 441
chunks, evicted 441, and left 7,153 resident. Reversal then reloaded 441,
generated none, evicted 441 from the opposite side, started no remesh jobs, and
returned to 7,153 residents. With unload distance 13, the move entered the same
441 chunks, evicted none, and left 7,594 resident; reversal caused no loads,
generation, eviction, or remeshing and retained 7,594 residents. This evidence
supports radius 13 for the tested one-chunk reversal without implying general
CPU or byte savings.

The immediate retention cost in that regression is 441 chunks, or 6.2% over
the 7,153 radius-12 residents. This is not the full distance-bound increase:
inclusive integer spheres contain 7,153 coordinates at radius 12 and 9,171 at
radius 13, a worst increase of 2,018 coordinates, or 28.2%. A fully allocated
chunk has 128 KiB of block arrays, so the dense block-array bounds for those
deltas are 55.125 MiB and 252.250 MiB, respectively. Chunks allocate block
subchunks sparsely, and these figures exclude chunk and allocator overhead,
asynchronous copies, and currently unmetered CPU and GPU mesh memory. They are
block-storage bounds, not measurements of total resident memory.

The view limit bounds a synchronous desired-set rebuild to 35,937 cube
candidates and 17,077 selected sphere coordinates. The unload limit bounds its
distance-retention sphere to 57,777 coordinates. Prefetch scans at most 728
neighbors, and the per-request cap cannot exceed the neighbor count selected
by its radius. Per-frame budgets are limited to 32,768. These are operational
ceilings for Rigel's fixed 32-cubed chunks rather than integer or address-space
maxima.

Negative queue, budget, thread, cache, and prefetch values are clamped to zero.
The desired set is rebuilt only when the camera enters a different chunk or a
distance changes; `update_budget_per_frame` does not turn that rebuild into a
partial desired-set scan.

---

## Render Config

`WorldRenderConfig` is loaded from YAML under the optional `render` root node.
If `render` is absent, the root is used directly.

Defaults below reflect the code defaults from `WorldRenderConfig`. The embedded
config (`assets/config/render.yaml`) may override them.

### Quick Reference (Rendering)

| Key | Type | Default | Notes |
| --- | --- | --- | --- |
| `render.render_distance` | float | `256.0` | Distance culling for chunks. |
| `render.sun_direction` | vec3 | `[0.5, 1.0, 0.3]` | Directional light vector. |
| `render.transparent_alpha` | float | `0.5` | Alpha for transparent pass. |
| `render.shadow.enabled` | bool | `false` | Toggle cascaded shadows. |
| `render.shadow.cascades` | int | `3` | Cascade count (maximum `4`). |
| `render.shadow.map_size` | int | `1024` | Shadow map resolution (maximum `8192`). |
| `render.shadow.max_distance` | float | `200.0` | Shadow max distance. |
| `render.shadow.split_lambda` | float | `0.5` | Log/linear split blend. |
| `render.shadow.bias` | float | `0.0005` | Depth bias. |
| `render.shadow.normal_bias` | float | `0.005` | Normal-based bias. |
| `render.shadow.pcf_radius` | int | `1` | Fallback for near and far PCF radii (maximum `4`). |
| `render.shadow.pcf_radius_near` | int | `1` | Near PCF radius override (maximum `4`). |
| `render.shadow.pcf_radius_far` | int | `1` | Far PCF radius override (maximum `4`). |
| `render.shadow.transparent_scale` | float | `1.0` | Transparent attenuation. |
| `render.shadow.strength` | float | `1.0` | Shadow strength multiplier. |
| `render.shadow.fade_power` | float | `1.0` | Shadow fade exponent. |
| `render.taa.enabled` | bool | `false` | Toggle TAA. |
| `render.taa.blend` | float | `0.9` | History blend factor. |
| `render.taa.jitter_scale` | float | `1.0` | Subpixel jitter scale. |
| `render.profiling.enabled` | bool | `false` | Enable the per-frame profiler. |

`streaming.view_distance_chunks` controls which chunks are loaded and meshed.
`render.render_distance` independently controls distance culling of available
chunk meshes in world units.

Key fields:

- `render_distance` (float)
- `sun_direction` (vec3)
- `transparent_alpha` (float)
- `shadow`:
  - `enabled`, `cascades`, `map_size`, `max_distance`
  - `split_lambda`, `bias`, `normal_bias`
  - `pcf_radius`, `pcf_radius_near`, `pcf_radius_far`
  - `transparent_scale`, `strength`, `fade_power`
- `taa`:
  - `enabled`, `blend`, `jitter_scale`

Values are clamped during load:

- `shadow.cascades` and `shadow.map_size` are clamped to at least `1`.
- `pcf_radius` and related values are clamped to at least zero.
- `taa.blend` is clamped to `[0, 1]`.

Shadow cascades, map dimensions, and PCF radii above their documented maxima
are rejected before renderer or streaming workers are created. The 8,192 map
limit leaves headroom above the shipped 6,144 maps while placing a fixed bound
on both texture-array allocations. The PCF limit matches the implemented
four-texel shader kernel instead of accepting values that would be silently
reduced during rendering.

When a near or far PCF radius is not configured, it follows `pcf_radius`.
Specific near and far values take precedence across configuration layers, so a
later generic radius only updates sides that have no specific value.

`RIGEL_PROFILE=1` forces profiling on at runtime, regardless of config. Setting
`RIGEL_PROFILE=0` forces profiling off.

---

## Persistence Config

`PersistenceConfig` is loaded from YAML under the optional `persistence` root
node. If `persistence` is absent, the root is used directly.

Defaults below reflect the code defaults from `PersistenceConfig`. The embedded
config (`assets/config/persistence.yaml`) may override them.

### Quick Reference (Persistence)

| Key | Type | Default | Notes |
| --- | --- | --- | --- |
| `persistence.format` | string | `cr` | Preferred format ID. |
| `persistence.providers` | map | - | Provider options by ID. |
| `persistence.providers.rigel:persistence.cr.lz4` | bool | `false` | CR backend compression. |

Key fields:

- `format`: preferred format ID (default `cr`).
- `providers`: map of provider ID -> options.

Provider options are stored as strings. Consumers interpret them as needed
(e.g. `getBool`, `getString`). Example from the shipped config:

```yaml
persistence:
  format: cr
  providers:
    rigel:persistence.cr:
      lz4: false
```

Provider IDs used by the persistence system:

- `rigel:persistence.cr` (CR backend settings)
- `rigel:persistence.block_registry` (block registry provider)

---

## Per-World Overrides

Per-world overrides are resolved by world ID. The default world ID is numeric
and used directly in the override paths:

- `config/worlds/<worldId>/world_generation.yaml`
- `config/worlds/<worldId>/render.yaml`
- `config/worlds/<worldId>/persistence.yaml`

These files are optional and only override fields they define. As the last
source layer, a per-world file and any overlays it declares have the highest
precedence.

---

## Limitations

- Configs are loaded once at startup; there is no hot reload.
- There is no schema validation beyond basic clamping.
- World generation overlays are the only supported overlay mechanism.
- Input bindings are configured through the asset manifest, not this system.

---

## Related Docs

- `docs/WorldGeneration.md`
- `docs/RenderingPipeline.md`
- `docs/PersistenceAPI.md`
- `docs/MultiWorld.md`
- `docs/AssetSystem.md`
- `docs/EmbeddedAssets.md`
- `docs/InputSystem.md`
