# Debug Tooling

This document describes the runtime debug overlays and diagnostic hooks in
Rigel. These tools are intended for development builds and are not configurable
through gameplay UI.

---

## 1. Overview

`FrameRenderer` owns the debug overlay state and GPU resources. It draws the GL
overlays after the main scene; Application renders the ImGui profiler
window from the same state. When enabled the tooling draws:

- Chunk streaming field (colored cubes for pipeline state).
- Frame time graph (ms per frame).
- ImGui profiler window (flame graph of per-frame scopes).
- Entity bounds wireframes.

The GL overlays are toggled by the `debug_overlay` action (F1 by default). The
ImGui profiler window is toggled independently by the `imgui_overlay` action
(F3 by default).

---

## 2. Toggle and Lifetime

- `FrameRenderer` owns the `Render::DebugState` containing `DebugField`,
  `FrameTimeGraph`, and `EntityDebug`.
- The application-owned `InputState` notifies `Input::DebugOverlayListener` and
  `Input::ImGuiOverlayListener` on action releases. The listeners toggle
  `DebugState::overlayEnabled` and `DebugState::imguiEnabled`, respectively.
- `FrameRenderer::initialize` calls:
  - `Render::initDebugField`
  - `Render::initFrameGraph`
  - `Render::initEntityDebug`
- `FrameRenderer::release` calls `Render::releaseDebugResources` on shutdown.

If a shader asset is missing, the corresponding overlay logs a warning and is
skipped.

---

## 3. Chunk Streaming Field

### 3.1 Data Source

- `WorldView::getChunkDebugStates` exposes `ChunkStreamer` state.
- Only tracked chunks appear (queued, ready, or failed states).
- The field is centered on the camera chunk and clipped to the current
  `viewDistanceChunks` radius.

### 3.2 Layout and Scale

- The field is rendered in a fixed-size viewport in the top-left corner.
- Constants live in `src/render/DebugOverlay.cpp`:
  - `kDebugViewportSize = 130`
  - `kDebugViewportMargin = 12`
  - `kDebugTargetSpan = 6.0f`
- Cell size is `kDebugTargetSpan / diameter`, so the overall field size stays
  constant as view distance changes.
- The field is positioned `debugDistance` units in front of the camera
  (`Render::DebugState::debugDistance`, default 8.0).

### 3.3 Colors and Meanings

State mapping (from `ChunkStreamer::DebugState`):

- `QueuedGen` (red): waiting for world generation.
- `LoadedFromDisk` (gray): persisted chunk data is ready but not yet meshed.
- `ReadyData` (yellow): chunk data loaded/generated, mesh not queued.
- `QueuedMesh` (blue): waiting for mesh build.
- `ReadyMesh` (green): mesh available.
- `GenerationFailed` (magenta): generation terminated with an error.
- `MeshFailed` (orange): mesh construction terminated with an error.

### 3.4 Rendering Rules

- Shader: `shaders/chunk_debug`.
- Per-state meshes are built so faces between same-state neighbors are culled.
- Faces between different states are not culled (state boundaries remain
  visible).
- Backface culling is disabled; depth testing is off; alpha blending is on.

---

## 4. Frame Time Graph

- Shader: `shaders/frame_graph`.
- `FrameRenderer::recordFrameTime` appends delta time in milliseconds.
- Ring buffer size is 180 samples; newest samples render on the right.
- Values are clamped to 50 ms and drawn as vertical bars at the bottom of the
  screen.
- Graph rendering disables depth test and uses alpha blending.

---

## 5. Profiler Window (ImGui)

- The ImGui profiler window displays a flame graph for the last frame.
- ImGui is a required build dependency.
- The window is toggled by `imgui_overlay` (F3 by default), independently of the
  GL overlays.

---

## 6. Entity Bounds Overlay

- Shader: `shaders/entity_debug`.
- `renderEntityDebugBoxes` draws a wireframe AABB for every entity.
- Bounds use `Entity::worldBounds`, not model geometry.
- Depth testing is enabled; depth writes are disabled.
- Wireframes render as `GL_LINE` polygons.

If TAA is enabled, entity debug boxes are drawn before the TAA resolve and are
subject to the jitter/resolve pass. The chunk field and frame graph render after
TAA, so they are stable.

---

## 7. Benchmark Logging

- `RIGEL_CHUNK_BENCH=1` enables chunk benchmark statistics and disables swap
  synchronization so rendering does not cap measured throughput. Normal runs
  synchronize buffer swaps to the display.
- When enabled, `Application` prints a summary on exit:
  - Generated, processed, meshed counts and rates.
  - Timing breakdown for generation and meshing.

---

## 8. Streaming Lifecycle Signal

`WorldView::streamingDiagnostics()` exposes the current streaming lifecycle and
generation, chunk-load, mesh, and eviction work counts. Each scheduled category
reports pending requests, in-flight work, and a cumulative started count.
Pending load counts include deferred region requests. Pending generation and
mesh counts include scheduler and capacity wait queues; mesh requests waiting
for neighbor data are also pending. Eviction pending counts cover deferred
persistence and generation-version replacement until it completes or is
canceled. Terminal generation, load, and mesh errors retain an operation and
coordinate diagnostic while they leave a desired coordinate unresolved.

The lifecycle states are:

- `discovering_spawn`: initial camera placement is not complete.
- `awaiting_initial_stream`: spawn discovery completed, but no streaming update
  has run.
- `streaming`: work was pending, in flight, unresolved, or started during the
  current update.
- `stabilizing`: an entire update completed without streaming work.
- `quiescent`: three consecutive full updates completed without pending,
  in-flight, or newly started work.

The application writes a `streaming.lifecycle` log record whenever the lifecycle
state or unresolved failure signature changes. The record contains the state,
all work counts, operation errors, and the stable update count. Repeated updates
do not log an unchanged failure. In particular, this record is the readiness
signal for automated performance capture:

```text
streaming.lifecycle state=quiescent generation.pending=0 generation.in_flight=0 ...
```

The started counts remain cumulative, so tests can take a snapshot at
quiescence and verify that stationary updates do not start additional work.
Quiescence bookkeeping examines active requests and explicitly retained
unresolved state; it does not rescan the desired chunk set or poll persistence
for discovery.

### 8.1 Chunk visibility latency trace

`WorldView::setVisibilityTracer` installs an opt-in trace for one identified
chunk coordinate. Construct `ChunkVisibilityTracer` with the coordinate and a
nonzero record capacity, then retain the shared tracer and call `measurement()`
to atomically copy the records and accounting. Each measurement has a sequence
identifying the captured tracer state. A capacity of zero disables the trace
without reading its clock or adding streamer inspection work.

Each record has an immutable coordinate/lifecycle ID key and a lifecycle kind:
`camera_demand` or `remesh`. Tracers allocate lifecycle IDs from one process-wide
sequence, so keys do not collide across tracer or streamer replacement. A
record receives an optional MeshTask identity only when the streamer physically
dispatches that task. The task identity is the actual mesh request ID, work
epoch, chunk instance ID, and mesh revision. Voxel-empty and cached lifecycles
therefore have no MeshTask identity, while a stale completion remains correlated
with its own dispatched input rather than a replacement lifecycle.

The record origin classifies observed persisted loads, observed generation,
remeshes, and already-resident data whose earlier history is left-censored.
`unresolved` means the data source had not reached an observed terminal choice
when the measurement was taken. A trace installed after eligibility can begin
at physical mesh dispatch; its resident-left-censored origin and absent earlier
stages make that late start explicit.

Retention is FIFO and includes pending records in the configured capacity.
The accounting returned with `measurement()` reports retained, dropped,
dropped-unfinished, unmatched-event, and clock-failure counts, so capacity
eviction, late events for evicted records, and unusable timestamps are visible
rather than silent.

The trace timestamps these stages:

- `desired`, `data_request`, and `data_ready` cover actual camera-demand and
  chunk-data transitions.
- `neighbor_ready` records the event that supplies the final required neighbor;
  `mesh_eligible` records the transition to dispatchable mesh work.
- `scheduler_wait`, `pool_submit`, and `worker_start` isolate scheduler and pool
  delay from worker execution.
- `worker_finish` and `result_accepted` cover build completion and main-thread
  validation.
- `first_draw` is recorded only after the renderer issues a nonempty main-pass
  draw. Mesh-store insertion and the streamer's `ReadyMesh` state do not set it.

The final-neighbor event can precede the next scheduler visit. In that case
dependency wait ends at the neighbor event and the intervening backlog is part
of `scheduler_wait`, not dependency wait. `ChunkVisibilityTraceRecord::durations()`
returns an interval only when both of its endpoint stages exist.

Stage absence is intentional and follows the lifecycle:

| Lifecycle or outcome | Legitimately absent stages |
| --- | --- |
| Camera demand for already-resident data | `data_request` and `data_ready`; `neighbor_ready` is also absent when required neighbors were already resident. |
| Remesh, including retained fringe work | `desired`, `data_request`, and `data_ready`; `neighbor_ready` is absent unless the remesh actually waits for a missing required neighbor. |
| Cached mesh | No MeshTask identity and no data, dependency, scheduler, pool, worker, or `result_accepted` stages. Cached empty geometry also has no `first_draw`. |
| Voxel-empty chunk | No MeshTask identity, pool/worker, `result_accepted`, or `first_draw` stages. Earlier demand or data stages remain only if those transitions occurred. |
| Dispatched stale or failed task | `result_accepted` and `first_draw`; pre-dispatch stages remain absent when tracing began after those transitions. |
| Accepted empty geometry | `first_draw`; resident or remesh rules still determine which earlier stages are absent. |
| Accepted nonempty geometry | `first_draw` remains absent until an actual main-pass draw submission. |
| Camera leave, tracer replacement, reset, generator replacement, or streamer destruction before dispatch | MeshTask identity and all task stages; any earlier stages remain recorded. |
| Clock callback failure | The timestamp for that event; lifecycle outcomes and scheduler work still advance normally. |

Camera departure after physical mesh dispatch preserves the original record's
MeshTask identity. Its late successful or failed result is rejected as stale
without publishing a mesh or terminal error. Re-entry starts a new camera
lifecycle; after the stale result is drained, the replacement dispatch receives
a distinct MeshTask identity.

Build outcomes distinguish cached empty/nonempty geometry, voxel-empty chunks,
accepted empty/nonempty geometry, stale results, failures, and each lifecycle
exit listed above. A nonempty cached or accepted record separately ends its draw
lifecycle as `drawn`, `camera_left_before_draw`, `mesh_removed_before_draw`,
`mesh_replaced_before_draw`, or `trace_replaced_before_draw`. Mesh entries retain
strong tracer ownership until one of those transitions, and `first_draw` is set
only by the renderer after a real nonempty main-pass draw call.

Clock callbacks are serialized for worker safety, are invoked without the
record mutex held, and cannot propagate exceptions into streaming or rendering
work. A callback failure leaves that event timestamp absent while terminal and
draw outcomes still close normally. `observed(stage)` remains true for such a
transition, and a later idempotent notification does not retimestamp it.
Installing or disabling a tracer does not
synthesize stages, requeue scheduler work, or add stationary desired-set scans;
only subsequent production lifecycle events create or advance records.

Do not treat an absent timestamp or duration as zero. Reject a measurement
window used for latency claims when it contains pending build outcomes,
nonempty outcomes still awaiting a draw transition, retention drops, unmatched
events, or clock failures. Also reject `unresolved` and
resident-left-censored records from end-to-end demand latency aggregates;
classify them separately if resident reuse is the metric being studied. Use
persisted, generated, camera-demand, and remesh populations separately, and
include a duration only when both endpoint stages are present. These rules also
apply when a measurement sequence is internally consistent: atomic capture
prevents mixed accounting, but it cannot make an incomplete or censored sample
complete.

---

## 9. Reproducible Streaming Validation

Use the same world and streaming configuration for comparisons. Every
interactive result must include the successful resource record emitted before
spawn discovery, for example:

```text
world.resources blocks.loaded=248 blocks.failed=0 blocks.skipped=1 blocks.discovered=249 textures.loaded=134
```

Require `blocks.loaded` and `textures.loaded` to be greater than zero and
`blocks.failed` to be zero. If this record is absent, or resource initialization
reports a failure, do not use a later lifecycle line as a measurement boundary;
an all-air run is not representative streaming or remeshing evidence.

Record the resource line and the full
`streaming.lifecycle state=quiescent` line with every accepted result. Configure
an out-of-tree build as described in the project README, then set its location:

```bash
rigel_build_dir=/absolute/path/to/rigel-build
cmake --build "$rigel_build_dir" --parallel --target Rigel Rigel_tests
```

### 9.1 Renderer-independent behavior

Run the lifecycle and steady-state scheduler regressions directly. These tests
do not create a window or execute renderer code:

```bash
"$rigel_build_dir/Rigel_tests" \
  --filter ChunkStreamer_QuiescenceRequiresStableIdleUpdates --verbose
"$rigel_build_dir/Rigel_tests" \
  --filter ChunkStreamer_SteadyStateSchedulerWorkDoesNotScaleWithViewVolume --verbose
```

The first test covers startup gating, pending and in-flight load states, the
stable update window, stationary work counts, and leaving quiescence after an
edit. The second verifies that a settled update performs no scheduler or desired
set inspection even when the configured view volume is large. Use these results
for claims about streaming behavior independent of rendering.

### 9.2 Interactive hardware-GPU run

On a machine with a hardware OpenGL driver, launch Rigel with profiling enabled
and capture its log:

```bash
RIGEL_PROFILE=1 RIGEL_CHUNK_BENCH=1 \
  "$rigel_build_dir/bin/Rigel" 2>&1 | tee /tmp/rigel-hardware.log
```

Confirm that the `OpenGL Version` log identifies the intended hardware driver.
Confirm the successful `world.resources` record and its block/texture counts.
From another terminal, wait for the lifecycle signal rather than using a
startup delay:

```bash
tail -n +1 -F /tmp/rigel-hardware.log | \
  sed -n '/streaming.lifecycle state=quiescent /q'
```

Begin the stationary CPU capture only after that command exits. Keep the camera
and world unchanged during the capture. Use the profiler's `Streaming` scopes
to measure streaming and the `Render` scopes to measure renderer work; total
process CPU alone does not identify which subsystem consumed it.

### 9.3 Xvfb with llvmpipe

Run the same configuration under a virtual display with software rendering:

```bash
LIBGL_ALWAYS_SOFTWARE=1 RIGEL_PROFILE=1 RIGEL_CHUNK_BENCH=1 \
  xvfb-run -a -s "-screen 0 1280x720x24" \
  "$rigel_build_dir/bin/Rigel" 2>&1 | tee /tmp/rigel-llvmpipe.log
```

Verify that the `OpenGL Version` line identifies llvmpipe and that the successful
`world.resources` record has valid block/texture counts, then wait for the same
quiescent record before measuring. Xvfb plus llvmpipe is useful for repeatable
startup and streaming validation when no hardware GPU is available, but
software rasterization can dominate process CPU. Attribute CPU to streaming
only when the `Streaming` scopes or renderer-independent regressions provide
that evidence; do not infer it from the process total or from a hardware versus
llvmpipe comparison.

---

## 10. Known Limitations

- Overlay state is owned by the application's `FrameRenderer`, not by a world
  or `WorldView`.
- Missing shaders disable that overlay component.
- Entity boxes reflect AABB extents, not exact mesh silhouettes.

---

## Related Docs

- `docs/RenderingPipeline.md`
- `docs/InputSystem.md`
- `docs/EntitySystem.md`
- `docs/ApplicationLifecycle.md`
- `docs/WorldGeneration.md`
