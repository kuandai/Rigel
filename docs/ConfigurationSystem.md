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

Three config types are supported today:

- `WorldGenConfig` (world generation + streaming)
- `WorldRenderConfig` (render pipeline settings)
- `PersistenceConfig` (save/load format and provider options)

The loader is implemented in `Voxel::ConfigProvider` and uses YAML input
(rapidyaml parser). Unknown keys are ignored; only known fields are applied.

---

## Config Sources and Precedence

Each config type has a fixed source order defined in
`src/voxel/WorldConfigBootstrap.cpp`. The general rule is:

1) Embedded defaults (from assets).
2) Project-level overrides under `config/`.
3) Project root overrides (for quick testing).
4) Per-world overrides under `config/worlds/<worldId>/`.

### World Generation

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

## Config Provider and Sources

`ConfigProvider` aggregates a list of `IConfigSource` instances:

- `EmbeddedConfigSource` reads an embedded raw asset (e.g. `raw/world_config`).
- `FileConfigSource` reads a file from disk.

Each source provides:

- `load()` -> full YAML file content.
- `loadPath(path)` -> overlay resolution (optional).

Overlay resolution has two behaviors:

- For embedded sources, `assets/` is stripped and the file is fetched from the
  embedded `ResourceRegistry` (e.g. `assets/config/worldgen_overlays/no_carvers.yaml`).
- For file sources, relative paths are resolved against the source file's
  directory (e.g. `config/world_generation.yaml` can reference
  `worldgen_overlays/no_carvers.yaml`).

---

## World Generation Config

`WorldGenConfig` is loaded by applying all sources in order, then processing
any overlays listed in the final config state.

Defaults below reflect the code defaults from `WorldGenConfig`. The embedded
config (`assets/config/world_generation.yaml`) overrides many of these values.

### Quick Reference (World Generation)

| Key | Type | Default | Notes |
| --- | --- | --- | --- |
| `seed` | int | `1337` | Global world seed. |
| `solid_block` | string | `base:debug` | Block ID used for solid fill. |
| `surface_block` | string | `base:debug` | Block ID used for surface fill. |
| `world.min_y` | int | `-64` | Minimum world Y coordinate. |
| `world.max_y` | int | `320` | Maximum world Y coordinate. |
| `world.sea_level` | int | `0` | Sea level for water placement. |
| `world.lava_level` | int | `-32` | Lava level (not used by generator). |
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
| `climate.elevation_lapse` | float | `0.0` | Unused by generator. |
| `climate.global.*` | object | - | Global climate noise (see below). |
| `climate.local.*` | object | - | Local climate noise (see below). |
| `biomes.blend_power` | float | `2.0` | Biome blend power. |
| `biomes.epsilon` | float | `0.0001` | Blend epsilon. |
| `biomes.coast_band.*` | object | - | Optional coast override. |
| `biomes.entries[]` | list | - | Biome definitions. |
| `density_graph.outputs` | map | - | Output name -> node id. |
| `density_graph.nodes[]` | list | - | Density node graph. |
| `caves.enabled` | bool | `true` | Enables cave carving. |
| `caves.density_output` | string | `cave_density` | Density output name. |
| `caves.threshold` | float | `0.5` | Density threshold. |
| `caves.sample_step` | int | `4` | Step size for carving. |
| `structures.features[]` | list | - | Simple feature definitions. |
| `streaming.view_distance_chunks` | int | `6` | Render/stream radius in chunks. |
| `streaming.unload_distance_chunks` | int | `8` | Unload radius in chunks. |
| `streaming.gen_queue_limit` | int | `0` | Generation queue cap (0 = unlimited). |
| `streaming.mesh_queue_limit` | int | `0` | Mesh queue cap (0 = unlimited). |
| `streaming.apply_budget_per_frame` | int | `0` | Apply budget (0 = unlimited). |
| `streaming.worker_threads` | int | `2` | Gen/mesh thread count. |
| `streaming.max_resident_chunks` | int | `0` | Cache cap (0 = unlimited). |
| `generation.pipeline[]` | list | - | Stage enable list. |
| `flags` | map | - | Boolean flags for overlays. |
| `overlays[]` | list | - | Overlay definitions. |

Noise objects (`terrain.noise`, `terrain.density_noise`, `climate.*.*`) use:

| Key | Type | Default |
| --- | --- | --- |
| `octaves` | int | `5` |
| `frequency` | float | `0.005` |
| `lacunarity` | float | `2.0` |
| `persistence` | float | `0.5` |
| `scale` | float | `1.0` |
| `offset` | float | `0.0` |

Key top-level fields (see `assets/config/world_generation.yaml` for examples):

- `seed`, `solid_block`, `surface_block`
- `world`: `min_y`, `max_y`, `sea_level`, `lava_level`, `version`
- `flags`: boolean map used by overlays
- `terrain`: base heights and noise controls
- `climate`: global/local temperature + humidity + continentalness noise
- `biomes`: biome targets, weighting, and surface layers
- `density_graph`: node graph for terrain density
- `caves`: carver settings
- `structures`: simple feature generation
- `streaming`: chunk streamer and thread pool settings
- `generation.pipeline`: stage enable list
- `overlays`: conditional config overlays

### Pipeline Stages

`generation.pipeline` does **not** reorder stages; it only enables or disables
fixed stages. If the list order does not match the fixed pipeline order, a
warning is logged and the order is ignored.

Current stage names:

- `climate_global`
- `climate_local`
- `biome_resolve`
- `terrain_density`
- `caves`
- `surface_rules`
- `structures`
- `post_process`

Stages default to enabled unless explicitly disabled.

### Flags and Overlays

Overlays are applied after base config load, in the order they appear in the
`overlays` list. Each overlay is a `{ path, when }` pair:

- `path`: YAML file to load.
- `when`: name of a boolean in `flags` (optional).

If `when` is missing or the flag is false, the overlay is skipped.
Overlays are deduplicated by path; each path is applied at most once.

The shipped overlay:

- `assets/config/worldgen_overlays/no_carvers.yaml` disables caves.

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
| `render.shadow.cascades` | int | `3` | Clamped to `[1,4]`. |
| `render.shadow.map_size` | int | `1024` | Shadow map resolution. |
| `render.shadow.max_distance` | float | `200.0` | Shadow max distance. |
| `render.shadow.split_lambda` | float | `0.5` | Log/linear split blend. |
| `render.shadow.bias` | float | `0.0005` | Depth bias. |
| `render.shadow.normal_bias` | float | `0.005` | Normal-based bias. |
| `render.shadow.pcf_radius` | int | `1` | PCF radius (fallback). |
| `render.shadow.pcf_radius_near` | int | `1` | Near PCF radius. |
| `render.shadow.pcf_radius_far` | int | `1` | Far PCF radius. |
| `render.shadow.transparent_scale` | float | `1.0` | Transparent attenuation. |
| `render.shadow.strength` | float | `1.0` | Shadow strength multiplier. |
| `render.shadow.fade_power` | float | `1.0` | Shadow fade exponent. |
| `render.taa.enabled` | bool | `false` | Toggle TAA. |
| `render.taa.blend` | float | `0.9` | History blend factor. |
| `render.taa.jitter_scale` | float | `1.0` | Subpixel jitter scale. |
| `render.profiling.enabled` | bool | `false` | Enable the per-frame profiler. |
| `render.profiling.overlay_enabled` | bool | `false` | Show profiler overlay (requires debug overlay). |

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

- `shadow.cascades` is clamped to `[1, ShadowConfig::MaxCascades]`.
- `pcf_radius` and related values are clamped to non-negative.
- `taa.blend` is clamped to `[0, 1]`.

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

These files are optional and only override fields they define.

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
