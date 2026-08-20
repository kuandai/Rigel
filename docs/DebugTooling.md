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
- Only non-missing chunks appear (queued or ready states).
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
generation, chunk-load, and mesh work counts. Each category reports pending
requests, in-flight work, and a cumulative started count. Pending load counts
include deferred region requests. Pending generation and mesh counts include
scheduler and capacity wait queues; mesh requests waiting for neighbor data are
also pending.

The lifecycle states are:

- `discovering_spawn`: initial camera placement is not complete.
- `awaiting_initial_stream`: spawn discovery completed, but no streaming update
  has run.
- `streaming`: work was pending, in flight, or started during the current update.
- `stabilizing`: an entire update completed without streaming work.
- `quiescent`: three consecutive full updates completed without pending,
  in-flight, or newly started work.

The application writes a `streaming.lifecycle` log record whenever the lifecycle
state changes. The record contains the state, all work counts, and the stable
update count. In particular, this record is the readiness signal for automated
performance capture:

```text
streaming.lifecycle state=quiescent generation.pending=0 generation.in_flight=0 ...
```

The started counts remain cumulative, so tests can take a snapshot at
quiescence and verify that stationary updates do not start additional work.
Quiescence bookkeeping examines active request queues only; it does not rescan
the desired chunk set or poll persistence for discovery.

---

## 9. Reproducible Streaming Validation

Use the same world and streaming configuration for comparisons, and record the
full `streaming.lifecycle state=quiescent` line with every result. Configure an
out-of-tree build as described in the project README, then set its location:

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
From another terminal, wait for the lifecycle signal rather than using a startup
delay:

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

Verify that the `OpenGL Version` line identifies llvmpipe, then wait for the
same quiescent record before measuring. Xvfb plus llvmpipe is useful for
repeatable startup and streaming validation when no hardware GPU is available,
but software rasterization can dominate process CPU. Attribute CPU to streaming
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
