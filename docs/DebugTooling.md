# Debug Tooling

This document describes the runtime debug overlays and diagnostic hooks in
Rigel. These tools are intended for development builds and are not configurable
through gameplay UI.

---

## 1. Overview

The debug overlay is owned by `Application` and renders after the main scene.
When enabled it draws:

- Chunk streaming field (colored cubes for pipeline state).
- Frame time graph (ms per frame).
- ImGui profiler window (flame graph of per-frame scopes).
- Entity bounds wireframes.

The debug overlay is toggled by the `debug_overlay` action (F1 by default).
ImGui profiler visibility is controlled separately by the `imgui_overlay`
action (F3 by default), which toggles `DebugState::imguiEnabled`.

---

## 2. Toggle and Lifetime

- `Render::DebugState` holds all overlay state (`DebugField`, `FrameTimeGraph`,
  `EntityDebug`).
- `Input::DebugOverlayListener` toggles `DebugState::overlayEnabled` on action
  release (`debug_overlay` / F1 by default).
- `Input::ImGuiOverlayListener` toggles `DebugState::imguiEnabled` on action
  release (`imgui_overlay` / F3 by default).
- `Application` calls:
  - `Render::initDebugField`
  - `Render::initFrameGraph`
  - `Render::initEntityDebug`
  - `Render::releaseDebugResources` on shutdown

If a shader asset is missing, the corresponding overlay logs a warning and is
skipped.

---

## 3. Chunk Streaming Field

### 3.1 Data Source

- `WorldView::getChunkDebugStates` exposes `ChunkStreamer` state.
- `WorldView::getVoxelSvoDebugPages` exposes `VoxelSvoLodManager` page states.
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

Chunk state mapping (from `ChunkStreamer::DebugState`):

- `QueuedGen` (red): waiting for world generation.
- `LoadedFromDisk` (light gray): loaded from persistence, awaiting processing.
- `ReadyData` (yellow): chunk data loaded/generated, mesh not queued.
- `QueuedMesh` (blue): waiting for mesh build.
- `ReadyMesh` (green): mesh available.

Voxel-SVO page state mapping (from `VoxelPageState`):

- `QueuedSample` (magenta): page queued for sampling.
- `Sampling` (orange): worker sampling/build in progress.
- `ReadyCpu` (cyan): CPU page/tree ready.
- `Meshing` (blue): far-mesh build in progress.
- `ReadyMesh` (lime): far mesh ready.

### 3.4 Rendering Rules

- Shader: `shaders/chunk_debug`.
- Per-state meshes are built so faces between same-state neighbors are culled.
- Faces between different states are not culled (state boundaries remain
  visible).
- Voxel-SVO overlays derive page span from `page_size_voxels` and page `level`,
  so coarse pages render larger than chunk-sized pages.
- Backface culling is disabled; depth testing is off; alpha blending is on.

---

## 4. Frame Time Graph

- Shader: `shaders/frame_graph`.
- `Render::recordFrameTime` appends delta time in milliseconds.
- Ring buffer size is 180 samples; newest samples render on the right.
- Values are clamped to 50 ms and drawn as vertical bars at the bottom of the
  screen.
- Graph rendering disables depth test and uses alpha blending.

---

## 5. Profiler Window (ImGui)

- The ImGui profiler window displays a flame graph for the last frame.
- It is only available when ImGui is linked (`imgui` package found).
- The window appears when `imgui_overlay` is enabled (F3), independent of
  `debug_overlay` (F1).
- The voxel-SVO panel includes live counters for:
  - desired sets (`desiredVisibleCount`, `desiredBuildCount`),
  - visible far coverage (`visibleReadyMeshCount`),
  - eviction classes (`evictedMissing`, `evictedQueued`, `evictedReadyCpu`,
    `evictedReadyMesh`),
  - mesh-block reasons (`meshBlockedMissingNeighbors`,
    `meshBlockedLeafMismatch`).

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

- `RIGEL_CHUNK_BENCH=1` enables chunk benchmark statistics.
- When enabled, `Application` prints a summary on exit:
  - Generated, processed, meshed counts and rates.
  - Timing breakdown for generation and meshing.

---

## 8. Known Limitations

- Profiler view requires ImGui and is hidden when ImGui is unavailable.
- Debug overlay is global and not per-world.
- Missing shaders disable that overlay component.
- Entity boxes reflect AABB extents, not exact mesh silhouettes.

---

## Related Docs

- `docs/RenderingPipeline.md`
- `docs/InputSystem.md`
- `docs/EntitySystem.md`
- `docs/ApplicationLifecycle.md`
- `docs/WorldGeneration.md`
