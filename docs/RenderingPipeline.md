# Rendering Pipeline and Shadow System

This document describes the current rendering flow for voxels, entities, and
shadows in Rigel. It focuses on how the renderer is wired today, not on planned
features.

---

## 1. Overview

`FrameRenderer` owns frame-level camera matrices, TAA resources and resolve,
and GL debug-overlay state. It delegates world drawing to `WorldView::render`,
which:

- Builds a `WorldRenderContext` with mesh store, texture atlas, shaders, the
  shipped render profile, and frame-derived ranges.
- Delegates voxel rendering to `ChunkRenderer`.
- Delegates entity rendering to `EntityRenderer`.

The application supplies the active `World`, `WorldView`, camera vectors,
viewport, and frame time to `FrameRenderer` once per frame. The application
renders the ImGui profiler after `FrameRenderer` returns.

`UserPreferences.graphics.shadows` is the sole player shadow request. The
renderer supplies one internal On profile; it has no enable field or YAML
source.

---

## 2. Frame Flow (Current)

1. Application updates simulation and streaming, then submits a frame context.
2. `FrameRenderer` builds `view` and `projection` matrices.
3. Optional TAA jitter is applied to the projection.
4. Scene renders to the TAA scene FBO (if enabled) or the default framebuffer.
5. `WorldView`:
   - Calls `ChunkRenderer::render`.
   - Calls `EntityRenderer::render`.
6. Optional TAA resolve blends history into the current frame.
7. Entity debug bounds render before the resolve when TAA is enabled (or after
   the world when it is disabled).
8. The current block-model selection outline renders with the stable
   non-jittered projection and scene depth, independent of the F1 diagnostics
   toggle. With TAA it is added after resolve but before color presentation, so
   it does not enter temporal history.
9. The chunk visualizer and frame graph render after the resolved scene.

---

## 3. Voxel Rendering

### 3.1 Data Flow

- `WorldMeshStore` holds CPU meshes keyed by `MeshId` and `MeshRevision`.
- `ChunkRenderer` caches GPU meshes and uploads when revisions change.
- Meshes are rendered per chunk using an offset uniform.
- `MeshBuilder` emits canonical cubes and normalized cuboids into the same
  chunk mesh. Model faces remain grouped by their block registration's render
  layer; there is no draw object per modeled block.

The canonical full cube retains its specialized meshing and AO path.
Normalized model faces preserve authored cardinal-face presence, UV crop and
quarter turns, and shading normals. Their neighbor culling is conservative:
only unit-boundary rectangles fully covered by opposite neighbor boundary
faces are hidden. Opacity alone never makes partial geometry a whole-cell
occluder.
Cube-style AO is applied only to a requesting normalized face that spans a
complete unit-cell boundary; other model faces use the unoccluded value.

### 3.2 Culling and Ordering

- Distance culling uses the active View Distance policy's world-unit range,
  `(accepted chunks + 1) * 32`.
- The same immutable policy supplies the projection far plane and a shadow
  distance ceiling. The shipped profile supplies a separate 200-world-unit
  limit; `WorldView` passes the smaller value to each frame without changing
  either owner.
- There is no frustum culling in the current pipeline.
- Transparent chunks are sorted back-to-front by view depth.

### 3.3 Render Layers

Each chunk mesh contains layer ranges:

- `Opaque`: depth writes on, no blending.
- `Cutout`: depth writes on, alpha test using `u_alphaCutoff`.
- `Transparent`: depth writes off, alpha blending, sorted back-to-front.
- `Emissive`: depth writes off, additive blending.

Layer selection is controlled by `u_renderLayer` in the voxel shader.

---

## 4. Internal Render Policy

`WorldView` starts with one shipped `RenderProfile`. Cascade count and map size,
distance limit, split blend, bias, normal bias, PCF radii, transparency scale,
strength, and fade power are fixed renderer tuning. The same profile owns the
static sun direction. Foreground transparent geometry preserves the alpha
sampled from its texture; the profile's transparency scale applies only to the
separate shadow-transmittance pass. Clear color and lighting weights remain
direct shipped constants. None of these values is loaded from player, world,
content, working-directory, or per-world configuration.

Tests and developer diagnostics can use the explicitly named
`WorldView::setRenderProfileForDiagnostics()` seam for exact low-level inputs.
Normal application startup does not replace the profile. TAA is part of this
internal profile and is shipped Off; there is no anti-aliasing player option.
Profiler collection is separate developer tooling enabled only by
`RIGEL_PROFILE=1` in builds that include instrumentation.

---

## 5. Shadow System (Cascaded Shadows)

### 5.1 Resources

`ChunkRenderer` owns:

- Depth array: `GL_DEPTH_COMPONENT24` 2D array, one layer per cascade.
- Transmittance array: `GL_RGBA8` 2D array, one layer per cascade.
- One framebuffer reused for depth and transmittance passes.

Depth maps use `GL_NEAREST`. Transmittance maps use `GL_LINEAR`.

For an On transition, `ChunkRenderer` allocates a candidate set, validates the
depth and combined transmittance framebuffer attachments, and only then swaps
the candidate into the active view. The prior set remains available until the
preference file is published; definite publication failure swaps it back. An
Off transition installs no shadow targets and releases the retired targets only
after publication succeeds. With no installed targets, shadow rendering exits
before framebuffer state queries or draw submission.

### 5.2 Cascade Splits

Splits are computed with a log/linear mix:

- `splitLambda` controls blending between uniform and logarithmic splits.
- Splits are stored in `u_shadowSplits`.

### 5.3 Cascade Bounds (Current)

The system uses a camera-centered cube:

- Centered on the camera position.
- Radius is the smaller of the cascade far split and the frame's effective
  shadow distance.
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
- Distance-based fade controlled by the internal fade power and the frame's
  effective shadow distance.

Transparent voxels do not receive shadows (`u_renderLayer == Transparent`).

---

## 6. Temporal AA (Post-Process)

TAA runs as a post-process pass owned by `FrameRenderer`:

- Scene renders into a color+depth FBO.
- History uses two ping-pong textures plus depth history.
- Jitter uses a Halton (2,3) sequence scaled by the internal jitter value.
- Resolve uses `shaders/taa_resolve` and the internal history blend.

The shipped profile keeps TAA disabled, so history is invalidated each frame.

---

## 7. Known Limitations

- Selection outlines draw the twelve AABB edges of each model cuboid rather
  than a silhouette union; multi-cuboid internal edges may remain visible.
- No frustum culling; distance-only culling for voxels.
- Shadow cascades use a camera-centered cube instead of fitting the frustum.
- Transparent layer does not receive shadows in the main pass.
- Shader source inclusion is not implemented.

---

## Related Docs

- `docs/ShaderSystem.md`
- `docs/EntitySystem.md`
- `docs/BlockTargeting.md`
- `docs/VoxelEngine.md`
- `docs/DebugTooling.md`
- `docs/ConfigurationSystem.md`
