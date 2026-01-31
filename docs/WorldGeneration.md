# World Generation and Chunk Streaming

This document describes the current world generation pipeline and chunk streaming
system in Rigel. Planned items are called out explicitly.

---

## 1. Overview

- Deterministic, seed-based generation per chunk.
- Fixed stage order; config can enable or disable stages.
- Generation and meshing run on background threads.
- Streaming is driven by `WorldView` and `ChunkStreamer`.
- Persistence integrates via loader callbacks and chunk dirty flags.

---

## 2. Generation Pipeline (Fixed Order)

Stages execute in the order below (see `kWorldGenPipelineStages`):

```
climate_global
climate_local
biome_resolve
terrain_density
caves
surface_rules
structures
post_process
```

Stage order is fixed in code. The config `generation.pipeline` section is used
to enable or disable stages, not reorder them.

---

## 3. Stage Details

### 3.1 climate_global

- Samples 2D fBm noise for temperature, humidity, and continentalness.
- Optional latitude bias via `climate.latitude_scale` and `climate.latitude_strength`.
- Populates `WorldGenContext.climate` for each XZ column.

### 3.2 climate_local

- Samples higher-frequency 2D noise and blends into the global climate.
- Blend factor is `climate.local_blend` (0 disables this stage).

### 3.3 biome_resolve

- Computes weights from distance to each biome target.
- Picks primary and secondary biomes plus blend factor.
- Optional coast band override: `biomes.coast_band` forces a biome within a
  continentalness range.

### 3.4 terrain_density

- Clears the chunk to air.
- If a density graph is present, evaluates output `base_density` per voxel.
- Otherwise uses `terrain.height_noise` and `terrain.density_noise` fallback:
  `density = density_noise * density_strength + (height - y) * gradient_strength`.
- Fills solid blocks using `solid_block` (config).
- Fills water up to `world.sea_level` when the column biome is `sea` or `beach`
  and the block `base:water[type=source]` is registered.
- Records the highest solid block per column in `heightMap`.

### 3.5 caves

- Requires a density graph.
- Evaluates the `caves.density_output` output (default `cave_density`).
- Carves to air when `density > caves.threshold`.

### 3.6 surface_rules

- Uses `heightMap` to apply surface materials.
- If the biome defines `surface` layers, those are applied in order.
- Otherwise uses `surface_block` with `terrain.surface_depth`.
- Uses sand when `height <= sea_level + 4` and `base:sand` is registered.

### 3.7 structures

- Places simple vertical pillar features from `structures.features`.
- Chance is driven by 2D noise per column.
- Optional biome filters limit placement to specific biomes.

### 3.8 post_process

- No-op stage reserved for future post-processing (lighting, ores, etc).

---

## 4. Climate, Biomes, and Density Graph

### 4.1 Climate Fields

Climate samples contain:

- `temperature`
- `humidity`
- `continentalness`

Global and local layers are combined additively. `climate.elevation_lapse` is
defined in config but not used by the current generator.

### 4.2 Biome Blending

Biome weights are computed from the distance to each target in climate space.
`biomes.blend.blend_power` controls falloff, and `biomes.blend.epsilon` avoids
division by zero.

### 4.3 Density Graph

`density_graph` defines a directed graph of nodes and named outputs:

- Node types include noise2D, noise3D, add/mul, clamp, spline, climate lookups,
  and `y` (vertical coordinate).
- `density_graph.outputs.base_density` drives the base terrain density.
- `caves.density_output` selects the output used for cave carving.

When a graph is present, 3D noise nodes are sampled using a per-chunk grid
and trilinear interpolation (fixed step of 4) to reduce cost.

---

## 5. Chunk Streaming and Tasking

### 5.1 State Machine

Chunks transition through the following states:

```
Missing -> QueuedGen -> ReadyData -> QueuedMesh -> ReadyMesh
```

Empty chunks skip mesh generation and move directly to `ReadyMesh`.

### 5.2 Desired Set and Distances

- The desired set is a sphere around the camera chunk with radius
  `streaming.view_distance_chunks`.
- Entries are sorted by distance, nearest first.
- Unload uses `streaming.unload_distance_chunks` for hysteresis.

### 5.3 Background Work and Budgets

- Generation and meshing run on a thread pool sized by `streaming.worker_threads`.
- `streaming.gen_queue_limit` and `streaming.mesh_queue_limit` cap in-flight work
  (`0` means unlimited).
- Mesh queue capacity reserves roughly 1/4 of the slots for dirty remeshes to
  keep player edits responsive.
- `streaming.update_budget_per_frame` limits how many desired chunks are scanned
  during the streaming update step (`0` means unlimited).
- `streaming.apply_budget_per_frame` limits how many completed results are
  applied per frame (`0` means unlimited).
- Disk load payloads are applied via `streaming.load_apply_budget_per_frame`
  to prevent IO completions from stalling frames.
- `streaming.io_threads` controls region IO concurrency, while
  `streaming.load_worker_threads` controls chunk payload build (decode + base
  fill) concurrency.
- `streaming.load_queue_limit` caps pending disk load requests (`0` means
  unlimited).

### 5.4 Meshing Constraints

- Meshes are only built when all 6 neighboring chunks are loaded to avoid
  culling seams at chunk borders.
- Mesh work uses padded block data to sample neighbors and AO.
- Each chunk has a `meshRevision` so stale results are discarded.

### 5.5 Cancellation and Eviction

- Generation tasks carry cancellation tokens; leaving the desired set cancels
  work before it is applied.
- `streaming.max_resident_chunks` enables an LRU eviction pass (via `ChunkCache`).
- Chunks outside `unload_distance_chunks` are unloaded immediately.
- If a loaded chunk’s `worldGenVersion` does not match the generator, the chunk
  is discarded and regenerated.

---

## 6. Persistence Integration

`ChunkStreamer` supports disk-backed loads via callbacks:

- `ChunkLoadCallback` enqueues a non-blocking request for disk data.
- `ChunkPendingCallback` reports whether a load is already in flight.
- `ChunkLoadDrainCallback` applies completed payloads within a per-frame budget.

The default loader (`AsyncChunkLoader`) uses the persistence service to fetch
region data asynchronously, builds chunk payloads off-thread (including base
fill for partial spans), then applies payloads on the main thread with a budget.
If no stored data is found, generation proceeds normally.

Chunk modifications mark `persistDirty`, which allows save logic to skip
unchanged chunks when persisting data.

---

## 7. Configuration and Overrides

World generation config is loaded from:

- `assets/config/world_generation.yaml` (embedded as `raw/world_config`)
- `config/world_generation.yaml`
- `world_generation.yaml`
- `config/worlds/<worldId>/world_generation.yaml`

Overlays:

- `overlays` is a list of `{ path, when }`.
- `when` references a boolean flag from `flags`.
- Overlays are resolved using the same config sources.

Reserved or not-yet-used fields:

- `world.lava_level` (unused in generator)
- `climate.elevation_lapse` (unused in generator)

---

## Related Docs

- `docs/VoxelEngine.md`
- `docs/PersistenceAPI.md`
- `docs/ConfigurationSystem.md`
