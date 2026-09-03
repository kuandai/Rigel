# Block collision

Rigel represents static block collision independently from visual block
geometry. This document describes the normalized asset contract, Cosmic Reach
snapshot rules, world query ownership, entity response, and the current
limitations.

## Physical shape contract

Each `BlockType` owns an immutable `BlockCollisionShape` in block-cell
coordinates. A shape is one of:

- `Empty`, with no boxes;
- `FullCube`, the canonical `[0, 0, 0, 1, 1, 1]` box; or
- `Boxes`, one or more positive-volume axis-aligned boxes.

`Empty` and `FullCube` do not allocate box storage. A `Boxes` shape validates
and shares immutable storage, and exposes a read-only span so world queries can
iterate it without per-query allocation. A shape may contain at most 16 boxes.
The staged Cosmic Reach 0.6.1 tree has a measured maximum of seven, so the
normalized limit retains deliberate headroom while bounding runtime work. Box
coordinates are finite and may range from `-0.25` through `1.25` on each axis.
This bounded overhang is validated, retained, and queried; it is not clamped
back into the owning cell.

The shape is not a `BlockModel`. Models own render cuboids, faces, texture
bindings, and orientation. Collision has no faces, textures, render layer, or
live relationship to those cuboids. A registration can therefore be visible
but non-colliding, or collide differently from its rendered model.

The normalized block format accepts `collision: none`, `collision: full`, or a
non-empty `collision.boxes` list. Every normalized block must provide one of
these values; omission and the removed `solid` field are rejected. See
`docs/AssetSystem.md` for the complete YAML schema and validation rules.

## Cosmic Reach snapshots

Collision generation occurs only during import. Runtime loading never reads
Cosmic Reach JSON and never derives physics from the normalized visual model.
The source contract has no independent collider field: `BlockState.walkThrough`
gates stock collision, while `BlockState.getAllBoundingBoxes` delegates to the
resolved block model's bounds. Rigel snapshots that source behavior as follows
rather than retaining a runtime dependency on it.

For every published source registration, the importer writes an explicit
collision shape that is already oriented for that registration:

1. A true source `walkThrough` property produces `Empty`, even when the
   resolved model has visible geometry.
2. Otherwise, the importer takes the positive-volume cuboids from the fully
   resolved source model. Inherited bounds and inflation have already been
   applied.
3. Zero-volume render helpers are discarded. In particular, they do not become
   extra stair colliders.
4. The registration's supported orthogonal orientation is applied before the
   normalized boxes are written. Runtime collision does not rotate them again.
5. One exact unit-cell box is canonicalized to `FullCube`; other single and
   multiple boxes remain explicit.

The importer records disjoint empty, full-cube, single-partial, and multi-box
counts together with exact, conservative-fallback, and ambiguous derivation
counts. Validation reconstructs those counts from the generated block tree and
the publication policy. The current policy permits only exact published
shapes: conservative fallback and ambiguity must both be zero. Unsupported or
ambiguous source geometry is omitted or rejected instead of silently receiving
a full-cube collider. This keeps fallback provenance auditable without making
fallback a normal publication path.

The reconciled Cosmic Reach 0.6.1 census contains 2,021 published
registrations: 67 empty, 315 full-cube, 956 single-partial, and 683 multi-box
shapes. Sixty-six walk-through registrations are visible but empty. Twenty-four
piston-head registrations retain collision overhang of exactly `0.25`
cell, and published stair shapes omit their zero-volume render helpers. These
figures are validation evidence, not runtime dispatch rules.

Plane-bearing and other unsupported registrations are omitted by the importer.
Visual planes do not automatically create collision, and there is no separate
plane-collider derivation path.

## World query ownership

`World::forEachCollisionBox` is the reusable static-world query. Its caller
provides world-space bounds and a callback; the world owns candidate-cell
selection, block lookup, shape storage access, local-to-world translation, and
overlap filtering. The callback receives each overlapping world-space AABB
synchronously. An entity therefore depends on collision boxes returned by the
world, not on block registration or shape-storage details.

Candidate-cell bounds include the complete supported `[-0.25, 1.25]` local
range, so an overhanging box is discoverable from either side of its owning
cell. Empty shapes are skipped, full cubes use a direct path, and explicit
boxes are visited through immutable spans. Invalid, unrepresentable, or
excessively large queries are rejected rather than performing unbounded work.
The fixed 65,536-cell query limit and 16-box shape limit cap one accepted query
at 1,048,576 explicit-box examinations. Callback traversal itself does not
allocate.

## Entity movement and contact

An entity uses its world-space AABB and resolves velocity in fixed X, then Y,
then Z order. For each nonzero axis delta, it queries the AABB covering the
entire movement interval, tests orthogonal overlap at that axis's starting
position, and clamps movement to the nearest blocking face. A hit clears that
velocity component and sets the corresponding collision flag. A downward Y hit
also establishes ground contact; a short downward support probe preserves
grounded state while stationary.

Each later axis starts from the position accepted by the preceding axes. This
prevents tunneling across a static AABB during one axis sweep, but the fixed
axis order makes diagonal corner and sliding results order-dependent. A
rejected world query cancels movement on that axis.

`BlockCollisionContactTolerance`, currently `1e-4` world unit, is the single
named static-block contact tolerance. World candidate expansion and overlap
filtering, entity separation, orthogonal overlap tests, and support probes all
derive from it.

An entity already overlapping a static box is not generally pushed out. In
particular, a stationary initial overlap remains stationary. Collision response
only constrains eligible movement toward a blocking face; it is not a general
depenetration or rigid-body solver.

## Traversal and retained limitations

Stairs are static collections of axis-aligned collision boxes. Entities can
land on their horizontal surfaces and are blocked by their vertical faces, but
there is no step-up traversal. Horizontal movement does not automatically lift
an entity onto the next stair tread.

The collision path intentionally does not provide:

- entity-to-entity collision;
- collision for the player-controlled camera, which moves independently of
  `Entity` physics;
- block selection from physical collision boxes: edit targeting instead uses
  declared oriented visual-model cuboid faces, as documented in
  `docs/BlockTargeting.md`;
- convex, rotated, sloped, or other non-AABB colliders;
- automatic collision for visual plane primitives;
- generalized initial-overlap depenetration; or
- axis-order-independent response.

`EntityTags::NoClip` also bypasses static-world collision for an entity. The
current collision work does not add player/controller physics, step-up, slopes,
entity collision, or persistence behavior.
