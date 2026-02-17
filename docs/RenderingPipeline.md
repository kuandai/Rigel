# Rendering Pipeline and Shadow System

This document describes the current rendering flow for voxels, entities, and
shadows in Rigel. It focuses on how the renderer is wired today, not on planned
features.

---

## 1. Overview

Rendering is driven by `WorldView::render`, which:

- Builds a `WorldRenderContext` with mesh store, texture atlas, shaders, and
  render config.
- Delegates voxel rendering to `ChunkRenderer`.
- Delegates entity rendering to `EntityRenderer`.

The Application sets the view/projection matrices and optionally applies TAA
before calling `WorldView::render`.

---

## 2. Frame Flow (Current)

1. Application builds `view` and `projection` matrices.
2. Optional TAA jitter is applied to the projection.
3. Scene renders to the TAA scene FBO (if enabled) or the default framebuffer.
4. `WorldView`:
   - Updates chunk streaming and meshes.
   - Calls `ChunkRenderer::render`.
   - Calls `EntityRenderer::render`.
5. Optional TAA resolve blends history into the current frame.
6. Debug overlays render (chunk visualizer, frame graph, entity debug).

---

## 3. Voxel Rendering

### 3.1 Data Flow

- `WorldMeshStore` holds CPU meshes keyed by `MeshId` and `MeshRevision`.
- `ChunkRenderer` caches GPU meshes and uploads when revisions change.
- Meshes are rendered per chunk using an offset uniform.

### 3.2 Culling and Ordering

- Distance culling uses `renderDistance` from `WorldRenderConfig`.
- There is no frustum culling in the current pipeline.
- Transparent chunks are sorted back-to-front by view depth.
- With SVO LOD enabled, chunk mesh rendering is near-band gated with
  hysteresis (`near_mesh_radius_chunks` / `lod_start_radius_chunks`).

### 3.3 Render Layers

Each chunk mesh contains layer ranges:

- `Opaque`: depth writes on, no blending.
- `Cutout`: depth writes on, alpha test using `u_alphaCutoff`.
- `Transparent`: depth writes off, alpha blending, sorted back-to-front.
- `Emissive`: depth writes off, additive blending.

Layer selection is controlled by `u_renderLayer` in the voxel shader.

### 3.4 Far Opaque LOD Pass (Preview)

- `SvoLodManager` emits opaque node-derived draw instances from ready LOD cells.
- Emission is camera-distance gated and hysteresis-stabilized, and uses
  distance-to-cell bounds against `renderDistance`.
- `ChunkRenderer` submits those instances through `shaders/svo_lod`.
- The current pass draws opaque proxy cubes and serves as Sprint 3 bootstrap.
- Under chunk-streaming pressure, `WorldView` throttles SVO update/upload work
  so generation/meshing remains prioritized.

---

## 4. Render Configuration

Render config is loaded from the config provider:

- `assets/config/render.yaml` (embedded as `raw/render_config`)
- `config/render.yaml`
- `render.yaml`
- `config/worlds/<worldId>/render.yaml`

Key fields in `WorldRenderConfig`:

- `renderDistance`
- `sunDirection`
- `transparentAlpha`
- `shadow` (see Section 5)
- `taa` (see Section 6)
- `svo` (preview CPU build + GPU upload pipeline with a far opaque bootstrap pass)
  - `lod_max_cells`, `lod_max_cpu_bytes`, `lod_max_gpu_bytes` bound SVO cache growth.
- `profilingEnabled` (per-frame profiler toggle; config key `render.profiling.enabled`)

---

## 5. Shadow System (Cascaded Shadows)

### 5.1 Resources

`ChunkRenderer` owns:

- Depth array: `GL_DEPTH_COMPONENT24` 2D array, one layer per cascade.
- Transmittance array: `GL_RGBA8` 2D array, one layer per cascade.
- One framebuffer reused for depth and transmittance passes.

Depth maps use `GL_NEAREST`. Transmittance maps use `GL_LINEAR`.

### 5.2 Cascade Splits

Splits are computed with a log/linear mix:

- `splitLambda` controls blending between uniform and logarithmic splits.
- Splits are stored in `u_shadowSplits`.

### 5.3 Cascade Bounds (Current)

The system uses a camera-centered cube:

- Centered on the camera position.
- Radius is `min(cascadeFar, shadow.maxDistance)` if maxDistance is set.
- This is not a true camera-frustum fit.

To stabilize shadows, the light-space bounds are snapped to the shadow texel
grid (texel-aligned light-space center).

### 5.4 Depth Pass

For each cascade:

- Render with `shaders/voxel_shadow_depth`.
- Opaque and cutout layers render to the depth array.
- Cutout uses alpha cutoff of `0.5`.
- Face culling is disabled so backfaces cast into the map.

Entity shadow casting is hooked via `IShadowCaster` in `WorldRenderContext`.

### 5.5 Transparent Transmittance Pass

If transparent chunks exist and `transparentScale > 0`:

- Render transparent layers with `shaders/voxel_shadow_transmit`.
- Blend with `glBlendFunc(GL_ZERO, GL_SRC_COLOR)` to accumulate transmittance.
- Uses the atlas "tint" textures for colored transparency.

If skipped, the transmittance map is cleared to white (no attenuation).

### 5.6 Main Pass Sampling

The voxel shader uses:

- `u_shadowMap` and `u_shadowTransmittanceMap` array samplers.
- `u_shadowMatrices` (light view-projection per cascade).
- PCF sampling with radius based on view distance.
- Cascade blending near split boundaries.
- Distance-based fade controlled by `shadow.fadePower` and `shadow.maxDistance`.

Transparent voxels do not receive shadows (`u_renderLayer == Transparent`).

---

## 6. Temporal AA (Post-Process)

TAA runs as a post-process pass in `Application`:

- Scene renders into a color+depth FBO.
- History uses two ping-pong textures plus depth history.
- Jitter uses Halton (2,3) sequence scaled by `taa.jitterScale`.
- Resolve uses `shaders/taa_resolve` and blends with `taa.blend`.

If TAA is disabled, the history is invalidated each frame.

---

## 7. Known Limitations

- Far LOD currently renders opaque proxy cubes, not final material-aware LOD geometry.
- No frustum culling; distance-only culling for voxels.
- Shadow cascades use a camera-centered cube instead of fitting the frustum.
- Transparent layer does not receive shadows in the main pass.
- Shader include support is not implemented (defines only).

---

## Related Docs

- `docs/ShaderSystem.md`
- `docs/EntitySystem.md`
- `docs/VoxelEngine.md`
- `docs/DebugTooling.md`
- `docs/ConfigurationSystem.md`
