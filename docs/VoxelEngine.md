# Voxel Engine Overview

This document summarizes the implemented voxel data, streaming, meshing, and
rendering architecture. Detailed generation, rendering, and persistence
behavior is covered by the linked subsystem documents.

## Ownership

- `WorldSet` owns world entries, shared `WorldResources`, and persistence
  services. Each entry contains one `World` and optionally one `WorldView`.
- `WorldResources` owns the set-wide `BlockRegistry` and `TextureAtlas`.
- `World` owns authoritative chunk data through `ChunkManager`, entities, a
  `WorldGenerator`, and persistence providers.
- `WorldView` owns derived state: `ChunkStreamer`, `WorldMeshStore`,
  `ChunkRenderer`, `EntityRenderer`, shaders, and `WorldRenderConfig`.
- `FrameRenderer` owns frame-level camera matrices, TAA, and debug overlays and
  delegates world drawing to the active `WorldView`.

The application currently creates one default world and one view, although a
`WorldSet` can contain multiple world entries.

## Blocks and Registry

`BlockID` is a 16-bit runtime identifier. ID zero is registered as `base:air`.
`BlockState` occupies four bytes and stores the ID, one metadata byte, and one
byte containing sky and block light nibbles.

`BlockType` supplies the properties used by storage and meshing:

- Identifier and model name
- Opacity, solidity, and same-type face-culling flags
- One texture path per axis-aligned face
- `Opaque`, `Cutout`, `Transparent`, or `Emissive` render layer
- Emitted-light and attenuation values

`BlockRegistry` assigns sequential IDs and supports lookup by ID or identifier.
`WorldResources::initialize()` uses `BlockLoader` to load block assets and build
the texture atlas before worlds are created. Initialization rejects failed block
definitions, an all-air registry, or an empty atlas. A successful interactive
startup logs the loaded definition and texture counts before spawn discovery.

## Chunk Storage and Coordinates

Chunks are 32 blocks on each axis. A `Chunk` divides its storage into eight
16-cubed subchunks and allocates a subchunk only when it contains non-air data.
It caches non-air and opaque counts for empty/full tests.

`ChunkCoord` uses signed integer chunk coordinates. `worldToChunk()` performs
floor division and `worldToLocal()` uses positive modulo, so negative world
coordinates map correctly.

`ChunkManager` owns loaded chunks and provides world-coordinate block access.
Reading an unloaded coordinate returns air; writing creates the containing
chunk. A changed boundary block invalidates the loaded neighbor on that face.

Chunk state distinguishes two kinds of change:

- Mesh dirtiness and `meshRevision` track whether derived geometry is current.
- `persistDirty` tracks whether authoritative block data needs saving.

Mesh invalidation notifications coalesce by coordinate in `ChunkManager`, so a
settled world does not require a scan of every loaded chunk to discover edits.

## Streaming

`WorldView::updateStreaming()` delegates to its `ChunkStreamer`. The streamer
builds a spherical desired set when the camera enters a different chunk or a
streaming distance changes. Desired coordinates are ordered nearest first.
Chunks beyond the effective unload radius are removed, and an optional
`ChunkCache` limit evicts non-desired residents. The effective unload radius is
never smaller than the view radius, so unload hysteresis exists only when the
configured unload distance is greater than the configured view distance.

For a missing desired chunk, the normal path is:

1. Request persisted data through the configured chunk-loader callback.
2. Wait while that request is pending.
3. Use the loaded payload when present, or enqueue world generation when the
   loader reports the chunk missing.
4. Wait for desired cardinal-neighbor data, then enqueue one mesh build.
5. Install the completed CPU mesh into `WorldMeshStore`.

Generation, load, and mesh capacity waits remain explicit pending work.
Terminal desired-frontier failures and deferred persistence or replacement
operations also remain unresolved lifecycle work until recovery, cancellation,
departure, or reset. Dirty mesh requests, capacity waiters, and retry deadlines
are event-driven; stationary updates do not rescan the desired volume or loaded
chunk set to rediscover them.

## Asynchronous Validity

Generation workers produce detached `ChunkBuffer` values. Cancellation tokens
stop work that leaves the desired set, and the main thread applies a result
only while its coordinate still has the queued-generation state.

Mesh tasks copy the chunk and a one-block padded neighborhood before leaving
the main thread. A coordinate has at most one mesh build in flight. Further
invalidations update the active request's observed revision and coalesce into a
replacement request.

A mesh result is installable only when all of these still match:

- Active mesh request ID
- Queued-mesh streamer state
- Chunk instance ID
- Captured mesh revision

Rejected results increment stale-work metrics and current dirty state is
rescheduled. Worker threads never mutate live chunks or GPU resources.

## Mesh Construction

`MeshBuilder` emits visible faces for blocks whose model is `cube`. It samples
the padded neighborhood for boundary culling and per-vertex ambient occlusion.
Faces against opaque neighbors are removed; `cullSameType` also removes faces
between matching non-opaque blocks.

Each visible face contributes four `VoxelVertex` values and two triangles.
Vertices contain local position, UV, normal index, ambient-occlusion level,
texture-array layer, and a flags byte. Geometry is grouped into per-layer index
ranges in `ChunkMesh`.

`WorldMeshStore` keys CPU meshes by chunk coordinate and assigns a stable
`MeshId` plus a revision that advances on replacement. Voxel-empty chunks have
no CPU mesh-store entry. Accepted-empty geometry for nonempty voxel data
retains an empty CPU-mesh lifecycle entry.

The chunk debug snapshot does not use mesh-store presence as draw evidence. It
reports voxel occupancy, installed empty/nonempty CPU geometry, current remesh
intent, and renderer-confirmed main-pass draw evidence as independent values.
Trace detail is the configured tracer's latest retained historical key, kind,
and build/draw outcomes, never proof of the current lifecycle owner.
Consequently, a completed or empty lifecycle may have no draw, and installed
nonempty geometry may still be distance-culled or awaiting its first main-pass
submission.

## Rendering

`ChunkRenderer` consumes `WorldMeshStore` through `WorldRenderContext`. Its GPU
cache is keyed by `MeshId`; entries upload when the store revision changes and
are pruned when the CPU mesh disappears.

Available chunk meshes are culled by `render.render_distance`. Opaque and
cutout layers write depth, transparent chunks are sorted back-to-front with
alpha blending, and emissive geometry uses additive blending. Cascaded shadow
passes and entity shadow casting are part of the same world render.

`WorldView` renders chunks first and entities second. `FrameRenderer` selects
the default framebuffer or TAA scene target, resolves TAA when enabled, and
then draws frame-level debug overlays.

## Texture Atlas

The set-wide `TextureAtlas` stores one 16-by-16 RGBA block texture per
`GL_TEXTURE_2D_ARRAY` layer by default. It also uploads one average-tint texel
per layer for transparent shadow transmittance. Mesh vertices select a layer;
all cube faces use normalized zero-to-one UVs.

## Current Limitations

- Only `cube` block models produce chunk geometry; other model names are
  skipped by `MeshBuilder`.
- Mesh generation emits independent faces rather than greedy merged quads.
- Voxel visibility uses distance culling only; there is no frustum or occlusion
  culling for chunks.
- Block texture animation is not implemented.
- Chunks, the block registry, mesh-store mutation, and GPU operations are
  main-thread operations; background jobs work from copies.

---

## Related Docs

- `docs/ApplicationLifecycle.md`
- `docs/WorldGeneration.md`
- `docs/RenderingPipeline.md`
- `docs/MultiWorld.md`
- `docs/PersistenceAPI.md`
- `docs/ConfigurationSystem.md`
