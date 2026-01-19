# Application Lifecycle

This document describes the `Application` lifecycle and the asynchronous
execution paths used by world generation, meshing, and disk IO.

## Index

- [Overview](#overview)
- [Phase 1: Bootstrap (Application::Application)](#phase-1-bootstrap-applicationapplication)
- [Phase 2: Runtime Loop (Application::run)](#phase-2-runtime-loop-applicationrun)
- [Phase 3: Shutdown (Application::~Application)](#phase-3-shutdown-applicationapplication)
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
   - `glfwInit()` and OpenGL version hints.
   - `glfwCreateWindow()` + `glfwMakeContextCurrent()`.
2. Initialize GLEW and log the OpenGL version string.
3. Register window callbacks.
   - Framebuffer resize -> `glViewport`.
   - Key + mouse callbacks wired through `Input` helpers.
4. Load the asset manifest and register loaders.
   - `input`, `entity_models`, `entity_anims` loaders are registered.
5. Register persistence formats and configure persistence root.
   - Formats are registered with `WorldSet::persistenceFormats()`.
   - Root path is resolved from the world id.
   - Preferred format + provider options come from `persistence.yaml`.
6. Initialize world resources.
   - Block registry, texture atlas, and other shared resources.
7. Load world config and create `World` + `WorldView`.
   - `WorldGenerator` is created and attached to both.
8. Load entity data from disk (chunks are lazy-loaded).
   - `loadWorldFromDisk(..., SaveScope::EntitiesOnly)` loads only entities.
9. Create the async chunk loader (disk IO) and wire it into `WorldView`.
   - Loader provides chunk data on demand via callbacks.
10. Apply render config + stream config.
11. Snap camera to the first air block, initialize debug overlays, init TAA.

## Phase 2: Runtime Loop (Application::run)

Per frame:
1. Poll events and compute `deltaTime` (clamped to max).
2. Update key state + dispatch action events (`InputDispatcher`).
3. Update camera and interaction logic.
   - Mouse look is applied if the cursor is captured.
   - Block edit raycasts and demo entity spawn occur here.
4. Tick entities (`World::tickEntities`).
5. Update chunk streaming (load/gen/mesh decisions).
6. Apply completed generation + mesh tasks.
7. Render scene (voxel + entities + debug overlays).
   - Shadow maps, TAA, and debug overlays are all handled here.
8. Swap buffers and check for exit (`exit` action).

## Phase 3: Shutdown (Application::~Application)

1. Save world to disk (synchronous) if initialized.
2. Release debug GPU resources.
3. Destroy window and terminate GLFW.

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
- `detail::ThreadPool` per subsystem (generation/meshing, IO).

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
- Queue size is capped by `stream.gen_queue_limit` (0 = unlimited).
- Chunks outside the desired set are cancelled (token flipped).

**Cancellation**:
- Each gen task holds a shared `atomic_bool` cancel token.
- If a chunk falls outside the desired set, the token is flipped.
- The worker checks the token; cancelled results are ignored.

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
- Queue size is capped by `stream.mesh_queue_limit` (0 = unlimited).
- A portion of the queue is reserved for dirty remeshes.

**Neighbor gating**:
- Meshing is skipped until all 6 neighbors are loaded (`hasAllNeighborsLoaded`).

**Thread-safety**:
- Worker threads operate on copied block data only.
- GPU updates / mesh store mutations happen on the main thread.

### C) Async Chunk IO (disk reads)

**Where**: `AsyncChunkLoader` in `Application` (`src/core/Application.cpp`)

**How**:
- `WorldView` calls the loader when a chunk is needed.
- Loader maps chunk -> region key and schedules region load jobs on a
  dedicated `detail::ThreadPool`.
- Each job opens a format instance and reads region data from storage.
- `LoadResult` is pushed into a `ConcurrentQueue`.
- `load()` drains completions on the main thread and updates:
  - Region cache + LRU
  - Present-chunk set per region
  - Loaded chunks (if already in memory)

**Merge behavior**:
- When spans exist, `mergeChunkSpans()` overlays disk data into a chunk.
- For partial spans, a base fill can be generated before overlays.
- Persist/dirty flags are cleared after disk data is applied.

**Prefetch**:
- Neighboring regions are queued around the requested region.

**Pending gating**:
- If a region is not yet cached, the streamer treats its chunks as pending and
  avoids generating them.

### D) World Save / Load

**Where**: `Persistence::saveWorldToDisk`, `loadWorldFromDisk`
(`src/persistence/WorldPersistence.cpp`)

**Behavior**:
- Synchronous on the main thread.
- Uses format containers + region layout to load/save spans.
- Entities are loaded/saved only if supported by the format.
 - Dirty chunk tracking controls what is written on save.

## Known Caveats

- `Voxel::Chunk` and `BlockRegistry` are not thread-safe; treat them as
  main-thread-only objects.
- Region IO is async, but application of spans is always main-threaded.
- Thread pool sizes are controlled by `WorldGenConfig::StreamConfig` and
  the async loader's `ioThreads` setting.

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
- `src/voxel/WorldView.cpp`
- `src/voxel/ChunkStreamer.cpp`
- `src/persistence/WorldPersistence.cpp`
