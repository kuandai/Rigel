# Block targeting and selection outline

Rigel selects the nearest declared surface of a block's immutable visual
`BlockModel`. Selection does not use the block's physical
`BlockCollisionShape`, and a non-air block is only a candidate until its model
geometry is intersected. This keeps the center-ray result aligned with the
geometry that the block mesher emits.

## Geometry contract

A `BlockModelInstance` combines reusable model geometry with one supported
right-angle orientation. The model contains axis-aligned cuboids in local
block-cell coordinates; every cuboid independently declares any subset of its
six cardinal faces. The same internal bounds-and-direction transform supplies
oriented geometry to meshing, targeting, registry-wide targeting extents, and
the selection outline.

Targeting follows these rules:

- The canonical built-in full cube uses a direct ray-versus-AABB path.
- A normalized cuboid is selectable only on a declared face. Missing faces do
  not hit, matching their absence from the emitted mesh.
- Empty models do not hit. A ray continues to later model geometry through an
  occupied cell whose model is empty or whose declared surfaces miss the ray.
- A visible model remains selectable when its independent collision shape is
  `none`. Conversely, collision geometry that has no corresponding declared
  visual surface is not selectable.
- Zero thickness on the face-normal axis is supported when the other two axes
  give the face positive area. Two declared opposing faces on the same plane
  make that surface selectable from either side. This represents the imported
  crop and vegetation geometry that rendering emits without requiring a
  positive-volume selection box.
- The ray direction is normalized before intersection, so distance and
  position are measured in world units even when the caller supplies a
  non-unit direction. The maximum distance is inclusive.

The supported primitive remains a declared cardinal face of a normalized
axis-aligned cuboid after one of the closed right-angle block orientations.
Rigel has no arbitrary triangle primitive or source plane-model primitive. A
source plane is not accepted by the importer merely because a zero-thickness
cuboid can express the measured two-sided crop surfaces.

## Hit result and deterministic boundaries

`BlockTarget` describes the exact selected surface:

- `block` and `state`: the owning world coordinate and the state read there;
- `distance` and `position`: the world-space distance and exact point on the
  ray;
- `face` and `normal`: the oriented cardinal model face and its integer
  outward normal; and
- `cuboidIndex`: the stable zero-based index in the immutable model.

`BlockRayIntersectionTolerance`, currently `1e-5` world units, is the one named
tolerance for block-model ray math. It defines boundary membership, distance
ties, inclusive face extents, and DDA boundary ties. A point strictly inside
all three positive-volume bounds selects the nearest declared forward exit
surface. A point within the tolerance of a boundary is not considered inside;
an immediate surface at distance zero may therefore be selected.

Equal-distance results are deterministic. Model-local ties prefer the lower
cuboid index and then the lower `Direction` value. World ties prefer the
lexicographically lower owning X, Y, and Z coordinate before applying the same
cuboid and face order. AABB edge and corner ties use the same face order.
Invalid, non-finite, effectively zero-length, or integer-domain-overflowing
rays return no target.

## Overhang-aware DDA

The raycast retains grid DDA instead of scanning a three-dimensional box over
the whole segment. Because visible geometry can extend beyond its owning
cell, visiting a ray cell does not imply that the owner has the same
coordinate. `BlockRegistry` incrementally records the aggregate bounds of all
registered, oriented models that declare at least one surface. Registration
rejects an aggregate whose conservative per-cell owner candidate volume would
exceed 512 coordinates.

For every traversed ray cell, targeting expands an owner range from those
finite aggregate bounds and tests each newly possible owning coordinate. The
previous monotonic range acts as an allocation-free visited set: an owner can
enter the moving candidate range only once, so owners retained across the next
DDA cell are not retested. Air is discarded cheaply; non-air owners are tested
against their actual model faces. The measured generated asset range of
`[-0.25, 1.25]` on every axis produces at most a 3-by-3-by-3 range for a
traversed cell.

The globally nearest model hit is retained rather than returning the first
occupied cell or first candidate. Traversal continues across a DDA boundary
that ties the best distance. It stops only when the next cell boundary is
farther than the best hit plus the intersection tolerance; by then every owner
whose geometry could produce an equal or closer hit has been tested. This
finds lateral overhangs and protrusions while keeping work proportional to
traversed cells times a registry-bounded candidate count, plus the cuboids and
declared faces examined for each non-air candidate. There is no segment box
scan or BVH.

## Runtime ownership

The application resolves one center target after camera input each frame. It
passes that value directly to block edits, gallery presentation, and
`FrameRenderer`. Removal changes the target's owning block. Placement uses the
exact selected face normal and changes the adjacent owning cell. If an edit
changes the world, the application refreshes the target before presentation
and rendering; removal also takes precedence over simultaneous placement so
stale surface metadata cannot place a block.

Gallery mode uses the same raycast but suppresses mutations. Its presentation
looks up catalog specimens and culling-diagnostic cells by `BlockTarget.block`,
which remains the owning coordinate even when the selected surface protrudes
into a neighbor. The window reports the one-based hit cuboid position and
model cuboid count, cardinal face, distance in blocks, model orientation, and
the independent collision description alongside the existing catalog and
render-layer metadata.

The renderer receives an already-resolved `BlockTarget`; it neither raycasts
nor modifies the world, chunks, or model assets.

## Selection outline

The target outline is ordinary selection feedback and is independent of the
F1 diagnostics toggle. `FrameRenderer` reuses the entity-debug line shader,
VAO, and dynamic VBO through the shared AABB-edge helper. It emits the twelve
true edges (24 `GL_LINES` vertices) of every oriented cuboid in the selected
model, translated from the owning block coordinate. A model with no current
target draws nothing.

Every bound is expanded by `0.002` world units before edge generation. The
expansion makes coplanar edges, including zero-thickness surfaces, visible
without disabling occlusion. Lines use depth testing, disable depth writes,
and restore the caller's program, VAO, array-buffer binding, depth, cull,
blend, and depth-write state.

Without TAA the stable, non-jittered outline follows world rendering. With TAA
the scene is resolved first, resolved color is copied back to the engine-owned
scene target, and the outline is composited there against the retained scene
depth before color is presented. Selection lines therefore do not contaminate
temporal history and do not depend on the window framebuffer's depth format.

The outline is per cuboid, not a unioned silhouette. Shared and internal edges
of multi-cuboid models may remain visible, and every cuboid is outlined rather
than only the cuboid that supplied the hit. There is no OBB, convex, mesh-BVH,
or arbitrary-triangle outline path.

## Scope and limitations

- Block targeting does not include entities. Entity bounds and entity
  collision remain separate systems.
- Physical collision may use several normalized AABBs but never determines a
  block selection result.
- Selection supports normalized cuboid faces and their right-angle
  orientations, not OBBs, convex hulls, general planes, or arbitrary triangle
  meshes.
- The outline does not remove internal edges or compute a silhouette union.
- Player physics, stair traversal, and entity collision are outside this
  subsystem.

## Related documentation

- `docs/VoxelEngine.md`
- `docs/BlockGallery.md`
- `docs/BlockCollision.md`
- `docs/RenderingPipeline.md`
- `docs/DebugTooling.md`
