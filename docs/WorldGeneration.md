# World Generation and Chunk Streaming

This document describes the current world generation pipeline and chunk
streaming system in Rigel.

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
```

Stage order is fixed in code. The `generation.stages` config map contains an
enable flag for each stage; its key order has no runtime meaning.

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
- Otherwise uses `terrain.noise` and `terrain.density_noise` fallback:
  `density = density_noise * density_strength + (height - y) * gradient_strength`.
- Fills solid blocks using `solid_block` (config).
- Fills water up to `world.sea_level` when the column biome is `sea` or `beach`
  and the block `base:water[type=source]` is registered.
- Records the highest solid block per column in `heightMap`.

### 3.5 caves

- Enabled or disabled by `generation.stages.caves`.
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

## 4. Climate, Biomes, and Density Graph

### 4.1 Climate Fields

Climate samples contain:

- `temperature`
- `humidity`
- `continentalness`

Global and local layers are combined additively.

### 4.2 Biome Blending

Biome weights are computed from the distance to each target in climate space.
`biomes.blend_power` controls falloff, and `biomes.epsilon` avoids division by
zero.

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
QueuedGen -> GenerationFailed
QueuedMesh -> MeshFailed
```

Empty chunks skip mesh generation and move directly to `ReadyMesh`.
Failed generation and mesh jobs release their queue capacity and remain in an
explicit failed state until a later streaming requeue retries them.

### 5.2 Desired Set and Distances

- The desired set is a sphere around the camera chunk with radius
  `streaming.view_distance_chunks`.
- Entries are sorted by distance, nearest first.
- Equal-distance entries have no secondary comparator. The current radius-one
  traversal order is not a scheduling guarantee: tests require exact cohort
  membership and ordering only between strictly different distances.
- Unload uses `streaming.unload_distance_chunks` for hysteresis.
- Render distance is configured separately by `render.render_distance` in world
  units and does not change the streaming desired set.

### 5.3 Background Work and Budgets

- Generation and meshing use separate pools partitioned from
  `streaming.worker_threads`. A pool with no worker executes its job inline.
- `streaming.gen_queue_limit` caps in-flight generation work, while
  `streaming.mesh_queue_limit` caps mesh work selected from the pending
  scheduler (`0` means no configured cap). Executor capacity may narrow that
  cap but the configured cap cannot expand executor capacity. With no mesh
  worker, the inline executor owns at most one completed-but-unapplied mesh
  result regardless of `streaming.apply_budget_per_frame`; this effective
  bound is observable as the `meshSubmissionLimit` streaming diagnostic.
- Generation admission has three FIFO boundaries. `m_loadGenQueue` holds
  logical load/generation scheduler visits, `m_generationCapacityWait` holds
  requests blocked by `gen_queue_limit`, and the generation `ThreadPool`
  appends submitted normal-priority jobs to its `m_jobs` deque. Camera movement
  rebuilds the first queue in new distance order, but does not reorder jobs
  already submitted to the pool. Cancellation tokens suppress departed work;
  they do not remove its physical pool entry or release its in-flight count
  before the completion drain observes the cancelled result.
  When a loader is configured, a coordinate waiting in `m_loadGenQueue` has not
  selected persisted load versus generation yet and is reported as unresolved
  rather than generation-pending.
- Eligible initial meshes and dirty remeshes share a distance-prioritized
  scheduler. Asynchronous submission is additionally capped at the actual
  mesh worker count so work that has not started remains reprioritizable.
- When both kinds are pending, mesh dispatch reserves roughly 1/4 of finite
  dispatch slots for dirty remeshes while remaining work-conserving.
- `streaming.update_budget_per_frame` limits how many queued load, generation,
  or missing-mesh requests advance during an update (`0` means unlimited). A
  desired-set rebuild itself is not spread across frames.
- `streaming.apply_budget_per_frame` limits completed generation results and
  completed mesh results separately (`0` means unlimited).
- Disk load payloads are applied via `streaming.load_apply_budget_per_frame`
  to prevent IO completions from stalling frames.
- `streaming.io_threads` controls region IO concurrency, while
  `streaming.load_worker_threads` controls chunk payload build (decode + base
  fill) concurrency.
- `streaming.load_queue_limit` caps pending disk load requests (`0` means
  unlimited).

The shipped world configuration uses `view_distance_chunks=12`,
`gen_queue_limit=128`, `update_budget_per_frame=4096`, and
`worker_threads=12`. The pool split assigns six threads to generation and six
to meshing. Generation can therefore own 128 submitted-but-undrained jobs, up
to 122 more than the generation workers can start concurrently. The radius-12
desired sphere contains 7,153 coordinates, so in a cold all-missing window up
to 7,025 additional chunks wait in the generation-capacity FIFO after the
first 128 submissions.

### 5.3.1 Cardinal-motion baseline

The constrained-worker regression
`ChunkStreamer_CardinalMotionLeavesNearGenerationBehindOlderWork` runs the
production queues with one generation worker, holds the first job, and moves
the center twice in +X and +Z. Each axial radius-two step has exactly 13 leading
and 13 trailing coordinates. The test compares those complete set differences,
validates each strict-distance cohort without relying on equal-distance order,
and then observes a newly leading chunk at squared distance 1 start after a
retained, previously submitted chunk at squared distance 2. This confirms
executor FIFO preservation across movement and rejects the hypothesis that a
desired-set rebuild reprioritizes already submitted generation.

The same evidence confirms that departure cancellation is validity-only at
the generation executor boundary: departed unstarted jobs publish cancelled
results when popped, and capacity is released when those results are drained.
It rejects cancellation of a running generation solely because its distance
priority changed; retained desired work continues. Capacity wakeup itself is
event-driven and advances one valid FIFO waiter per observed completion rather
than scanning the desired volume.

### 5.4 Meshing Constraints

- Meshes wait for all cardinal neighbors that fall inside the desired set.
  Missing neighbors outside that set are sampled as air.
- Mesh work uses padded block data to sample neighbors and AO.
- A chunk has at most one mesh build in flight. Repeated invalidations are
  coalesced while it runs.
- Mesh installation validates the request id, streamer state, chunk instance,
  and captured mesh revision. Invalid results are rejected and current dirty
  state is queued for another build.

### 5.5 Cancellation and Eviction

- Generation tasks carry cancellation tokens; leaving the desired set cancels
  work before it is applied.
- `streaming.max_resident_chunks` enables an LRU eviction pass (via `ChunkCache`).
  Desired chunks are protected, so the cache can remain above this limit when
  the desired set alone exceeds it.
- Chunks outside `unload_distance_chunks` are unloaded after modified data is
  persisted. Failed persistence defers removal to a bounded retry update and
  remains explicit pending lifecycle work between attempts.
- If a loaded chunk’s `worldGenVersion` does not match the generator, modified
  data is persisted through the same removal gate before the chunk is discarded
  and regenerated. A deferred replacement remains unresolved until replacement
  generation completes or the coordinate leaves the desired set.

---

## 6. Persistence Integration

`ChunkStreamer` supports disk-backed loads via callbacks:

- `ChunkLoadCallback` enqueues a non-blocking request for disk data.
- `ChunkPendingCallback` reports whether a load is already in flight.
- `ChunkLoadDrainCallback` reports loaded, missing, or failed requests within a
  per-frame budget.

The default loader (`AsyncChunkLoader`) uses the persistence service to fetch
region data asynchronously, builds chunk payloads off-thread, then applies
payloads on the main thread with a budget. Partial Memory-format spans use
generator base fill before stored overlays because Memory enables
`fillMissingChunkSpans`; CR and the default capability leave uncovered voxels
as air.

Region reads enter a loader-owned priority scheduler. Direct chunk demand and
promoted same-region prefetch are selected before unstarted speculative reads.
Submission is bounded by the physical IO worker count and, when nonzero, the
configured in-flight cap. With no IO worker, reads execute inline and at most
one completed result remains dispatched-undrained. The pool's pending lane
supports promotion and removal before worker start, so priority survives the
final dispatch boundary; running reads are not cancelled. The speculative
scheduler is bounded by the configured region cap. When that cap is disabled,
at most 64 speculative owners are retained in the loader-owned queue.
Dispatched-undrained work, including an unstarted pool-pending submission, is
bounded separately by the IO executor capacity. Direct demand can displace an
unstarted speculative read at capacity without changing region coalescing.
After a successful pre-start pool yield or demand-cancellation demotion, the
same region owner returns to `SchedulerPending` before it becomes visible in a
loader queue. Resubmission then advances it through `PoolQueued`,
`WorkerRunning`, and `ResultPublished` again.

`AsyncChunkLoader::metrics()` exposes bounded lifetime aggregates rather than
per-region history. Direct and speculative admissions retain their immutable
origin through promotion. Logical admissions, retry admissions, pool
submissions and resubmissions, successful pre-start pool yields, terminal pool
cancellations, pool and inline starts, published and drained results, missing
probes, admission-to-worker-start time, and worker-execution time are accounted
separately. At quiescence, logical admissions equal published results plus
logical pre-start cancellations; pool submissions equal pool worker starts plus
successful pool yields and terminal pool cancellations. Separate counters
record same-region demand promotion, the first direct use of a prefetched cache
entry, and speculative cache eviction before demand.

The four current-owner gauges report demand-owned or speculative-owned work as
loader-queued or dispatched-undrained. Dispatched-undrained includes pool and
inline submissions through completed results that have not yet been drained.
Pool-pending speculative and yield candidate-visit counters expose the physical
speculative displacement bound independently of logical region owners.

A four-run synthetic later-demand contention fixture used the headless Memory
format with 2 IO threads, a 16-region cap, radius-1 prefetch, 12 speculative
candidates, and a 16-region drain budget. Two initial workers were
capacity-gated until a later direct request had entered the scheduler.
Fresh-empty stored no regions; sparse-persisted stored only the initial and
later direct regions; dense-persisted stored one full solid chunk in every
radius-1 neighbor region plus both direct regions. The reported duration is the
later direct admission-to-worker-start time, which isolates region scheduler
wait from region execution:

| Fixture | No prefetch | FIFO prefetch | Prioritized prefetch |
| --- | ---: | ---: | ---: |
| Fresh-empty | 0.011-0.048 ms | 0.128-0.159 ms | 0.049-0.097 ms |
| Sparse-persisted | 0.008-0.011 ms | 0.237-0.277 ms | 0.052-0.106 ms |
| Dense-persisted | 0.011-0.063 ms | 100.9-112.4 ms | 17.0-22.9 ms |

The dense synthetic fixture established material later-demand contention in
FIFO dispatch. Priority dispatch reduced that scheduler wait by 77-85%, which
is the measured reason for direct-first scheduling, while still allowing a
running speculative read to finish. These ranges are not camera-near timing or
shipped-backend evidence; the camera-near conclusion is deferred to the
representative interactive benchmark.

If no stored data is found, generation proceeds normally. Region read failures
are retried from completion events and never imply that stored data is absent.
An irrecoverable persisted-data failure remains a load error for the desired
coordinate; it does not fall through to generation.

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

---

## Related Docs

- `docs/VoxelEngine.md`
- `docs/PersistenceAPI.md`
- `docs/ConfigurationSystem.md`
