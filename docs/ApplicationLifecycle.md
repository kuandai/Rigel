# Application Lifecycle

This document describes the `Application` lifecycle and the asynchronous
execution paths used by world generation, meshing, and disk IO.

## Index

- [Overview](#overview)
- [Phase 1: Bootstrap (Application::Application)](#phase-1-bootstrap-applicationapplication)
- [Phase 2: Runtime Loop (Application::run)](#phase-2-runtime-loop-applicationrun)
- [Phase 3: Shutdown (Application::close)](#phase-3-shutdown-applicationclose)
- [Threading Model](#threading-model)
- [Asynchronous Flows](#asynchronous-flows)
  - [A) Chunk Generation (world data)](#a-chunk-generation-world-data)
  - [B) Chunk Meshing (render data)](#b-chunk-meshing-render-data)
  - [C) Async Chunk IO (disk reads)](#c-async-chunk-io-disk-reads)
  - [D) World Save / Load](#d-world-save--load)
- [Known Caveats](#known-caveats)
- [Relevant Code](#relevant-code)

## Overview

Phases: bootstrap -> runtime loop -> shutdown. Bootstrap allocates OS/GL
resources and constructs core systems. The runtime loop processes input,
ticks simulation, streams chunks, applies async work results, and renders.
Shutdown persists world state and releases resources.

## Phase 1: Bootstrap (Application::Application)

1. Initialize GLFW and create the main window.
   - `glfwInit()` and an OpenGL 4.1 forward-compatible core context request.
   - `glfwCreateWindow()` + `glfwMakeContextCurrent()`.
   - Runtime and window ownership is recorded as each acquisition succeeds.
     An exception invokes the normal idempotent shutdown sequence before it is
     rethrown.
   - Interactive runs synchronize buffer swaps to the display. Chunk benchmark
     runs disable swap synchronization so presentation does not cap throughput.
2. Initialize GLEW and log the OpenGL version string.
3. Register window callbacks.
   - Framebuffer resize -> `glViewport`.
   - Key and mouse-button callbacks feed the application-owned `InputState`.
   - Cursor, focus, character, and scroll callbacks also feed camera or ImGui
     state as applicable.
4. Load the asset manifest and register loaders.
   - `input`, `entity_models`, `entity_anims` loaders are registered.
   - `shaders/voxel` is required. Failure to load it aborts view creation; the
     failed candidate is not published, so a later call can retry normally.
   - Voxel depth and transmission shadow shaders are optional independently.
     A missing or unloadable shader emits one warning naming its asset id and
     disables only the pass that consumes it.
   - Entity and entity-shadow shaders are optional independently. A missing or
     unloadable main shader disables entity rendering; a missing or unloadable
     shadow shader disables entity shadow casting.
   - A missing or unloadable `input/default` asset emits one warning and uses
     the built-in bindings. Bindings are published only after this fallback is
     complete.
   - ImGui initialization is optional. A false result or exception emits one
     warning naming ImGui, cleans partial UI state, and continues without UI.
5. Register persistence formats and configure persistence root.
   - Formats are registered with `WorldSet::persistenceFormats()`.
   - Root path is resolved from the world id.
   - Preferred format + provider options come from `persistence.yaml`.
6. Initialize world resources.
   - Block registry, texture atlas, and other shared resources.
   - Failed block definitions, an all-air registry, or an empty texture atlas
     abort world bootstrap before spawn discovery.
   - Successful initialization records loaded block and texture counts.
7. Load world config and create `World` + `WorldView`.
   - `WorldGenerator` is created and attached to both.
8. Load entity data from disk (chunks are lazy-loaded).
   - `loadBootstrapEntities(...)` validates and adds persisted entities without
     changing live chunks or unrelated entities.
9. Create the async chunk loader (disk IO) and wire it into `WorldView`.
   - Loader provides non-blocking requests + budgeted apply callbacks.
10. Load and apply render config, the profiling environment override, and
    stream config.
11. Snap the camera to the first air block, mark spawn discovery complete, and
    initialize `FrameRenderer`.

## Phase 2: Runtime Loop (Application::run)

Per frame:

1. Compute `deltaTime`, poll GLFW events, apply any focus-driven time reset, and
   clamp the frame time.
2. Begin the ImGui and profiler frames.
3. Record the frame-time sample and call `InputState::beginFrame()` to publish
   callback-fed key and mouse-button state and notify action listeners.
4. Apply cursor-capture actions, then update camera and interaction logic.
   - Mouse look is applied if the cursor is captured.
   - Block edit raycasts use mouse press edges; demo entity spawning uses an
     action press edge.
5. Tick entities (`World::tickEntities`).
6. Update chunk streaming (load/generation/mesh decisions).
7. Drain and apply completed generation, load, and mesh work.
8. Read the refreshed streaming lifecycle snapshot and log state transitions.
9. Submit the active world, camera, viewport, and frame time to `FrameRenderer`.
   - `FrameRenderer` handles camera matrices, TAA, world drawing, and debug
     overlays.
10. Render the ImGui profiler window, end the profiler and ImGui frames, and
    swap buffers.

The loop ends when GLFW marks the window for closure. There is no application
exit action in the current binding set.

### Streaming lifecycle

Spawn discovery completes during bootstrap, before the world becomes ready.
The first runtime streaming update begins the initial-stream phase. After each
normal update and completion drain, `ChunkStreamer` combines its generation and
mesh counts, eviction state, and the `AsyncChunkLoader` load counts supplied by
`WorldView`.

Pending counts include capacity-blocked generation and mesh requests, mesh
requests waiting for neighbor data, deferred region loads, and persistence or
eviction retries. Terminal generation, load, and mesh failures remain explicit
errors while their desired coordinates are unresolved. Version-replacement
eviction remains pending through the replacement generation result. A zero
queue count at startup or between completion stages is therefore not sufficient
for quiescence. The lifecycle reaches `quiescent` only after three consecutive
complete updates observe no pending, in-flight, or unresolved work and start no
new work. A new load, generation, mesh, or eviction request returns the
lifecycle to `streaming` immediately on the next update.

The application logs `streaming.lifecycle` records on lifecycle transitions and
when the unresolved failure signature changes. Records contain operation counts
and the generation, load, mesh, and eviction error diagnostics, so a new or
recovered failure is visible even while the lifecycle remains `streaming`.
Unchanged failures are not logged every frame. This is the developer-facing
readiness signal for starting a stationary performance measurement without a
fixed startup delay. The snapshot is assembled from active queues, unresolved
state, and counters and does not add a desired-set scan or periodic persistence
query.

## Phase 3: Shutdown (Application::close)

1. Save world to disk (synchronous) if initialized.
2. Make the application context current and shut down ImGui.
3. Disconnect streaming callbacks and stop the asynchronous chunk loader.
4. Release view GPU state, then destroy views, streaming workers, and worlds.
5. Release the shared atlas, frame renderer, and cached assets while the
   context remains current.
6. Destroy the window and terminate GLFW.

Normal process exit calls `close()` after the runtime loop. Persistence errors
leave the world resident, propagate to the process entry point, produce an
error diagnostic, and result in a failed process exit. Destructor cleanup makes
one final best-effort save attempt without throwing, including after an explicit
close error. A pending entity recovery journal is replayed before that retry can
publish another journal. Callers that omit `close()` receive the same
best-effort persistence before teardown. Failed construction skips the save and
tears down only the resources acquired so far. Repeated successful shutdown
requests are harmless.

---

## Threading Model

Main thread:
- Chunk creation/destruction.
- Applying gen/mesh results.
- Mesh store updates + rendering.
- World save/load.
- Entity updates and block edits.

Worker threads:
- World generation (`WorldGenerator::generate`).
- Mesh building (`MeshBuilder::build`).
- Region IO via `PersistenceFormat` in AsyncChunkLoader.

Synchronization:
- `detail::ConcurrentQueue` for result handoff.
- Separate generation and mesh pools partition `streaming.worker_threads`;
  asynchronous loading has region IO and payload-build pools.

---

## Asynchronous Flows

### A) Chunk Generation (world data)

**Where**: `Voxel::ChunkStreamer` (`src/voxel/ChunkStreamer.cpp`)

**How**:
- `enqueueGeneration()` schedules a job on `detail::ThreadPool`.
- Job calls `WorldGenerator::generate()` to fill a `ChunkBuffer`.
- Results are pushed into a `ConcurrentQueue<GenResult>`.
- The main thread calls `processCompletions()` each frame to apply results
  (`applyGenCompletions`).

**Queueing rules**:
- Generation-needed coordinates remain in the camera-prioritized scheduler
  until dispatch.
- `streaming.gen_queue_limit` caps selected jobs (0 = no configured cap), and
  asynchronous dispatch is additionally capped at the generation worker count.
- With no generation worker, one inline result remains submitted until the
  owner-thread completion drain observes it.
- A completion drain refills newly available generation slots before returning.
- Camera movement reprioritizes pending generation without changing retained
  submitted jobs.
- Chunks outside the desired set are cancelled (token flipped).

**Cancellation**:
- Each gen task holds a shared `atomic_bool` cancel token.
- If a chunk falls outside the desired set, pending ownership is erased and a
  submitted job's token is flipped. Submitted ownership remains accounted until
  its result is drained.
- The worker checks the token; the main thread also requires the coordinate to
  remain in `QueuedGen` before applying the result.

**Thread-safety**:
- Worker threads never mutate live `Chunk` instances.
- Only `ChunkBuffer` is produced in background threads.
- Main thread applies the buffer into `Chunk` objects.

### B) Chunk Meshing (render data)

**Where**: `Voxel::ChunkStreamer` (`src/voxel/ChunkStreamer.cpp`)

**How**:
- `enqueueMesh()` copies chunk blocks into a `MeshTask` and enqueues a worker job.
- Worker builds a `ChunkMesh` using `MeshBuilder` and a padded neighbor buffer.
- `MeshResult` is pushed into `ConcurrentQueue<MeshResult>`.
- Main thread applies mesh results in `processCompletions()`.

**Queueing rules**:
- Initial meshes and dirty remeshes remain in one priority scheduler until
  dispatch.
- `streaming.mesh_queue_limit` caps selected mesh jobs (0 = no configured
  cap). Executor capacity can narrow that configured cap: asynchronous
  submission is capped at the mesh worker count, while an executor with no
  mesh worker owns at most one completed-but-unapplied inline result regardless
  of `streaming.apply_budget_per_frame`. The queue cap cannot expand that
  physical slot. The effective bound is reported by the `meshSubmissionLimit`
  streaming diagnostic.
- A portion of finite dispatch capacity is reserved for dirty remeshes when
  both request kinds are pending.
- At most one mesh job is in flight for a chunk. Additional invalidations are
  coalesced and cause a replacement build after the current result returns.

**Neighbor gating**:
- Meshing waits for each cardinal neighbor that is also in the desired set.
  Neighbors outside the desired set are sampled as air.

**Thread-safety**:
- Worker threads operate on copied block data only.
- GPU updates / mesh store mutations happen on the main thread.

**Result validity**:
- Installation requires the active request id, a live `QueuedMesh` state, the
  same chunk instance id, and the same mesh revision captured by the task.
- Invalid results are counted as stale and current dirty state is rescheduled.

### C) Async Chunk IO (disk reads)

**Where**: `AsyncChunkLoader` (`src/persistence/AsyncChunkLoader.cpp`)

**How**:
- `WorldView` calls the loader's request callback when a chunk is needed.
- The loader maps chunk -> region key and schedules region IO on the IO pool.
- Region results are cached (LRU) and used to schedule per-chunk payload builds.
- Payload builds run on a worker pool. They decode stored spans and, when the
  selected format enables `fillMissingChunkSpans`, generate base fill first.
- `ChunkStreamer::processCompletions()` drains payloads on the main thread via
  `ChunkLoadDrainCallback`, honoring `streaming.load_apply_budget_per_frame`.
- Failed region reads are retried from their completion events. Exhausted reads
  complete with a failure outcome and do not fall through to generation.

**Merge behavior**:
- When spans exist, `mergeChunkSpans()` overlays disk data into a chunk.
- For partial spans, Memory enables generator base fill before overlays; CR and
  the default capability leave uncovered voxels as air.
- Persist/dirty flags are cleared after disk data is applied.

**Prefetch**:
- Neighboring regions are queued around the requested region.

**Pending gating**:
- If a chunk has a pending disk request, the streamer skips world-gen until the
  request completes or is canceled.

### D) World Save / Entity Bootstrap

**Where**: `Persistence::saveWorldToDisk`, `loadBootstrapEntities`
(`src/persistence/WorldPersistence.cpp`)

**Behavior**:
- Synchronous on the main thread.
- Uses format containers + region layout to save chunk spans.
- Bootstrap loads entities only when supported by the format; chunks are
  loaded through `AsyncChunkLoader`.
- All persisted entity records and live-ID collisions are validated before any
  entity is spawned.
- Dirty chunk tracking controls what is written on save.

## Known Caveats

- `Voxel::Chunk` and `BlockRegistry` are not thread-safe; treat them as
  main-thread-only objects.
- Region IO is async, but application of spans is always main-threaded.
- Worker counts are controlled by `StreamingConfig` (`worker_threads` is split
  between generation and meshing; `io_threads` and `load_worker_threads`
  control asynchronous loading).

---

## Related Docs

- `docs/InputSystem.md`
- `docs/WorldGeneration.md`
- `docs/RenderingPipeline.md`
- `docs/DebugTooling.md`
- `docs/EntitySystem.md`
- `docs/PersistenceAPI.md`
- `docs/ConfigurationSystem.md`

---

## Relevant Code

- `src/core/Application.cpp`
- `src/render/FrameRenderer.cpp`
- `src/voxel/WorldView.cpp`
- `src/voxel/ChunkStreamer.cpp`
- `src/persistence/WorldPersistence.cpp`
