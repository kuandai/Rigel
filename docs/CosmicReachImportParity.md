# Cosmic Reach import parity

The Git-owned Cosmic Reach snapshot was compared with a fresh deterministic
import before its removal. This records the migration evidence and the small,
intentional compatibility rules that remain in the importer.

## Source and output

- Cosmic Reach version: 0.6.1
- Source JAR SHA-256:
  `58a2cc3b79b5413cfa0f2e4ae3b37f44ed7f11a5e828de57f9f3f71599ac570e`
- Generated tree SHA-256 at the parity gate:
  `f71bc35a71d9d02b6768db9d2796412a61e1ed163603d5562e00b9ebc726a227`
- Generated counts: 431 blocks, 438 textures, 16 entity models,
  7 animation sets, and 59 sounds

The source JAR is not part of the repository. Its digest, rather than its
filename or a version string, identifies the migration input.

## Block comparison

The legacy tree contained 249 normalized block definitions. Every one of the
249 logical block identifiers exists in the generated tree. The required
world-generation materials were checked individually:

| Identifier | Result |
| --- | --- |
| `base:dirt` | Exact normalized semantic match |
| `base:grass` | Exact normalized semantic match |
| `base:sand` | Exact normalized semantic match |
| `base:stone_shale` | Exact normalized semantic match |
| `base:water[type=source]` | Same properties and texture bytes; source path moved to `textures/blocks/fluids/` |

Across the full legacy set, 171 definitions are exact normalized matches. The
remaining 78 have explained differences:

- 9 use a different YAML texture-map shorthand but expand to the same six
  face paths.
- 19 use paths reorganized by Cosmic Reach 0.6.1 while retaining identical
  texture bytes.
- 27 legacy definitions left cube faces or referenced texture files missing;
  the importer resolves all six faces from the actual CR model.
- 21 use the current JAR's model face assignments or payloads instead of the
  legacy converter's first-texture approximation.
- 13 honor `cullsSelf` from the current CR model where the old converter did
  not.
- 2 ladders honor the current source's light attenuation of 1, which the old
  snapshot omitted.

Some categories overlap because one block can differ in more than one field.
No mismatch was normalized away to obtain parity.

Cosmic Reach 0.6.1 added parameters to ten states that Rigel previously
addressed by a bare ID. The importer emits the source identity and an explicit
bare compatibility alias for those known IDs. This preserves saved/runtime
references without retaining the old asset files.

## Entity, animation, and sound comparison

- All six legacy CR animation sets are semantically identical after the
  importer adds their logical IDs. The JAR also supplies one new animation.
- All sixteen legacy OGG files are byte-identical to the JAR; the current JAR
  supplies 59 OGG files in total.
- Rigel's scale, hitbox, animation linkage, and render-offset metadata is
  reapplied to the corresponding imported entity models.
- Current CR texture paths are used for planet models; the legacy paths did
  not resolve in the old ignored texture pack.
- `base/models/entities/wire.json` is omitted with a recorded warning because
  the 0.6.1 JAR references `base:textures/entities/wire.png` but does not
  contain that texture. Publishing the broken reference would violate the
  generated-tree closure rule.

## Current renderer boundary

Rigel's normalized block loader supports reusable axis-aligned cuboid models in
addition to the specialized `cube` and `none` paths. The importer resolves the
0.6.1 model parent graph and texture aliases, emits inherited geometry once,
and binds texture-only substitutions in each block definition. Declared bounds,
inflation, missing faces, reversed or cropped UV rectangles, UV quarter-turns,
and face shading/culling metadata are preserved. Right-angle block-state
orientation remains a closed measured set.

Explicit plane primitives, animated and nonstandard block textures,
collision-shape fidelity, and model-accurate raycasting remain outside the
runtime boundary. Import provenance retains the model-support schema, closure
census, and sorted identifiers under disjoint omission reasons. The detailed
counts and final output identity are recorded below.

CR model emission texture maps also remain outside the current normalized
block contract. Scalar emitted light is preserved. The importer recognizes
the source field rather than treating it as unknown, and the limitation is
kept explicit here instead of silently claiming full CR rendering parity.

## Final block-model census

The pre-support census divided the 1,607 generated non-cube omissions by source
geometry:

| Source shape | States | Geometrically expressible as cuboids |
| --- | ---: | ---: |
| One cuboid | 929 | 929 |
| Multiple cuboids | 672 | 672 |
| Cuboids plus planes | 6 | 0 |
| **Total** | **1,607** | **1,601** |

| Published support | Before cuboid support | Final import | Change |
| --- | ---: | ---: | ---: |
| Block definitions | 431 | 2,021 | +1,590 |
| Shared normalized cuboid definitions | 0 | 51 | +51 |
| Generated non-cube candidates recovered | 0 | 1,590 | +1,590 |
| Non-cubic base registrations with normalized geometry | 0 | 100 | +100 |

Exactly 1,590 candidate states are newly recovered; the 11-state difference
from geometric expressibility is the current 16-by-16 atlas restriction. This
is support for the measured cuboid subset, not general Cosmic Reach model
support.

The source also has 106 non-cubic base registrations that the earlier importer
published with cube geometry. One hundred now bind normalized cuboids. The
remaining six are omitted rather than retaining a misleading cube: three have
plane/mixed geometry and three have nonstandard texture dimensions.

Omission accounting is disjoint:

| Provenance reason | Generated candidates | Base registrations | Recorded IDs |
| --- | ---: | ---: | ---: |
| Plane or mixed geometry | 6 | 3 | 9 |
| Nonstandard texture dimensions | 11 | 3 | 14 |
| **Total** | **17** | **6** | **23** |

The plane-bearing states also depend on texture behavior outside the current
atlas contract, but the audit assigns each identifier one reason so totals do
not overlap. Plane primitives, animated textures, and the 64-by-16 conveyor and
splitter textures all remain unsupported.

The real-JAR integration gate inspected these published representatives through
asset load, texture binding, CPU meshing, mesh-store installation, GPU upload,
and draw submission:

| Block state | Evidence checked |
| --- | --- |
| `base:wood_planks[slab_type=bottom]` | One cuboid, cropped UVs, half-cell bounds |
| `base:wood_planks[stair_type=bottom_PosX]` | Four cuboids, ten faces, quarter-turned UVs |
| `base:door_steel[part=bottom,power=on,direction=PosX]` | Y-oriented one-eighth-cell thickness |
| `base:ladder_steel[direction=PosX]` | Y-oriented two-face thin geometry |
| `base:table_pedestal_wood` | Opaque wood frame and pedestal plus alpha-blended glass top |
| `base:piston[direction=PosX,type=advancing,part=head]` | Two cuboids, cropped/reversed/rotated UVs, and an authored bound of 1.25 |
| `base:maize[type=farm,growth=4,part=bottom]` | Alpha-cutout crossed cuboids preserve transparent color and depth holes |

The final tree identity is
`f087345162f222962752a28aa3e8ee8e6ab94506bce04754daf46c5bd0d711d8`.
A forced second import from the JAR digest recorded above reproduced both this
tree hash and the provenance bytes exactly.

## Meshing measurement

The production `MeshBuilder` was measured in a Release build on 2026-08-30
with GNU 16.1.1, Linux x86_64, an Intel Core i7-12700, and 20 reported hardware
threads. The deterministic invented fixture used 30 timed iterations after five
warmups per workload:

| Workload | Blocks | Vertices | Indices | Mean | P50 | P95 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| All canonical cubes | 16,384 | 16,384 | 24,576 | 0.969 ms | 0.966 ms | 0.999 ms |
| 75% cubes, 12.5% single, 12.5% multiple | 16,384 | 189,440 | 284,160 | 3.167 ms | 3.165 ms | 3.263 ms |
| 62.5% single, 37.5% multiple | 16,384 | 540,672 | 811,008 | 18.240 ms | 18.227 ms | 18.581 ms |

Pre-timing validation checked exact mesh cardinalities, layer batching, all
supported orientations, a frozen registry, and
`canonical_cube_fast_path=true`. The benchmark is deliberately synthetic and
does not claim to reproduce the frequency distribution of a real world. Full
environment metadata, minimum/maximum timings, fixture construction, and the
reproduction command are recorded in `docs/DebugTooling.md`.
