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

The 0.6.1 source describes 1,607 generated non-cube states. Cuboids recover
1,601 geometrically, and 1,590 of those are publishable with the current 16×16
block atlas. Supported non-cubic base registrations now reference the same
normalized assets instead of retaining their earlier cube approximation. A
current import emits 51 shared normalized cuboid definitions and 2,021 block
definitions in total, an increase of exactly 1,590 blocks over the original
parity output. Of 106 geometrically non-cubic base registrations in the source,
100 are corrected from the earlier cube approximation; the remaining six are
split evenly between the independent geometry and texture omissions below.
The resulting tree SHA-256 is
`e4d1c653f6cd36b876b033e8d359d188779261e4829adfc01ee6f8b62b4e81f3`.

Explicit plane primitives remain outside the normalized model contract. Six
generated variants use cuboid-plus-plane geometry; together with three base
registrations they are visibly omitted with one provenance diagnostic. The
64×16 conveyor and splitter textures likewise remain unsupported, producing a
separate texture-dimension diagnostic. Animated textures, generalized planes,
collision-shape fidelity, and model-accurate raycasting remain outside the
runtime boundary.

Import provenance retains the model-support schema and closure census. The
1,607 candidates divide into 1,590 recovered states, six plane/mixed omissions,
and eleven nonstandard-texture omissions. The same disjoint reasons record the
three plus three omitted base approximations, including sorted block-state IDs,
so the 106-state base census closes independently. Repeating a forced import of
the same JAR produces the same tree hash and provenance bytes.

CR model emission texture maps also remain outside the current normalized
block contract. Scalar emitted light is preserved. The importer recognizes
the source field rather than treating it as unknown, and the limitation is
kept explicit here instead of silently claiming full CR rendering parity.
