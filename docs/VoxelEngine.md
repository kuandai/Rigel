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
  `ChunkRenderer`, `EntityRenderer`, shaders, and the internal `RenderProfile`.
- `FrameRenderer` owns frame-level camera matrices, TAA, and debug overlays and
  delegates world drawing to the active `WorldView`.

The application currently creates one default world and one view, although a
`WorldSet` can contain multiple world entries.

## Blocks and Registry

`BlockID` is a 16-bit runtime identifier. ID zero is registered as `base:air`.
`BlockState` occupies four bytes and stores the ID, one metadata byte, and one
byte containing sky and block light nibbles.

`BlockType` supplies the properties used by storage and meshing:

- Identifier and an immutable model instance
- Opacity, solidity, and same-type face-culling flags
- Full-cube face textures or named normalized-model texture bindings
- `Opaque`, `Cutout`, `Transparent`, or `Emissive` render layer
- Emitted-light and attenuation values

`BlockRegistry` assigns sequential IDs and supports lookup by ID or identifier.
`WorldResources::initialize()` uses `BlockLoader` to load block assets and build
the texture atlas before worlds are created. Initialization rejects failed block
definitions, an all-air registry, or an empty atlas. A successful interactive
startup logs the loaded definition and texture counts before spawn discovery.
The candidate registry is published atomically and then frozen before chunk
mesh workers can read it. Registrations transitively retain immutable,
Rigel-normalized `BlockModel` geometry.

There are three model paths: built-in `cube`, built-in empty `none`, and
reusable axis-aligned cuboid assets. A normalized model owns one or more cuboid
bounds plus only its declared cardinal faces. Each face selects a named texture
slot and preserves its UV rectangle, quarter-turn rotation, shading direction,
AO request, and culling request. Block registrations bind those slots and may
apply only the measured right-angle X/Y/Z orientations. Cuboid bounds are in
cell units and may extend outside `[0, 1]`.

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
streaming distance changes. The sphere is clipped to chunks intersecting the
generator's inclusive finite Y bounds, and desired coordinates are ordered
nearest first.
Chunks beyond the effective unload radius are removed, and an optional
`ChunkCache` limit evicts non-desired residents. The active-world View Distance
policy derives the unload radius as exactly one chunk beyond the accepted view
radius.

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

Generator replacement and a live View Distance transition own an explicit
desired-rebuild diagnostic and a deterministic resident reconciliation that
advances at most 64 coordinates per scheduler call. A newer distance request or
concurrent camera move replaces the target retention snapshot without
triggering an all-resident scan. Meshing samples resident in-world neighbors
across the unload fringe, but masks every voxel row outside current finite
bounds to air without erasing persisted data.

## Asynchronous Validity

Generation workers produce detached `ChunkBuffer` values. When demand is
retired, an executor-queued job that remains unstarted is removed by an
incarnation-qualified handle and releases capacity immediately. A job already
claimed by a worker keeps its cancellation token and the single
coordinate-keyed physical owner until the main thread drains the exact result.
Re-demand coalesces behind that owner. The main thread settles the matching
owner and applies a result only when the queued-generation state, epoch,
generator version, and current demand remain valid.

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

`MeshBuilder` has a specialized branch for the canonical built-in full cube.
That branch retains its original face emission, padded-neighborhood culling,
per-vertex AO, UV behavior for identity orientation, and world-generation
compatibility. Normalized cuboids use a separate branch that emits each
declared face at its oriented bounds. Missing faces remain missing, and cropped,
reversed, and quarter-turned UVs are applied directly.

Neighbor culling is boundary- and coverage-aware. A requested model face is
eligible only when it lies on the unit-cell boundary and its tangential bounds
stay inside that boundary. It is hidden only when an opaque or applicable
same-type neighbor has opposite boundary faces whose union covers the requested
rectangle. Non-boundary and out-of-cell faces remain visible, and a partial
opaque neighbor cannot over-cull a full-cell face. Model-model adjacency
therefore favors extra interior geometry over visible holes.

The canonical cube keeps its existing AO. A normalized face uses cube-style AO
only when it explicitly requests AO and spans the complete unit-cell boundary;
other model faces receive the conservative fully unoccluded level. AO samples
treat an opaque model as an occluder only when a closed cuboid covers the whole
cell.

Each visible face contributes four `VoxelVertex` values and two triangles.
Vertices contain local position, UV, normal index, ambient-occlusion level,
texture-array layer, and a flags byte. Geometry is grouped into per-layer index
ranges in `ChunkMesh`. Cuboid geometry is part of those chunk batches and does
not create a render object per block.

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

Available chunk meshes are culled by a world-unit range derived from the
accepted `UserPreferences.graphics.view_distance_chunks` request. Opaque and
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
canonical cube faces use normalized zero-to-one UVs, while normalized models
select subrectangles and quarter turns within the same texture layer.

## Block geometry versus gameplay

Normalized models are visual geometry. Collision still tests the source-authored
`solid` property against the entire block cell, and edit raycasting stops at any
occupied cell rather than intersecting model faces. A partial solid model
therefore does not shrink collision or raycast bounds, and visible geometry
outside `[0, 1]` does not extend them into another cell.

Opacity, emitted light, and attenuation likewise remain source-authored
block-registration properties. Rigel does not infer solidity or lighting from
cuboid coverage.

## Current Limitations

- Normalized block geometry is limited to measured single/multiple axis-aligned
  cuboids and the closed right-angle block-state orientation set. There is no
  general model transform or scene graph.
- Plane primitives, animated block textures, and non-16-by-16 block textures
  are not supported. The importer omits those states instead of approximating
  them.
- Collision and edit raycasting operate on whole cells, not visual model
  bounds or faces.
- Mesh generation emits independent faces rather than greedy merged quads.
- Voxel visibility uses distance culling only; there is no frustum or occlusion
  culling for chunks.
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
