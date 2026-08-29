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

Rigel's normalized block loader supports only `cube` and `none` geometry.
The old snapshot already approximated explicit doors, plants, machines, and
other CR models as cubes. The importer preserves that existing boundary for
explicit states while resolving their face textures more accurately.

State-generator outputs whose geometry is itself a full cube are imported.
The 0.6.1 source also describes 1,607 slab, stair, plane, or other non-cube
generated states. Those are validated through the generator include graph but
omitted with a provenance warning instead of being falsely published as
cubes. Supporting those shapes would be a separate block-model/meshing
project, outside this asset-hygiene migration.

Six additional 0.6.1 block states reference 64×16 animated sprites or a 64×64
entity texture. Rigel's block atlas accepts 16×16 tiles and cannot represent
the source animation metadata. None of these states existed in the 249-file
legacy snapshot. The importer validates their source definitions, omits the six
unsupported runtime states with a provenance warning, and validates every
published block texture dimension. Publishing definitions that later fail
`WorldResources` initialization would be misleading; adding animated or
non-tile block textures is a separate renderer feature.

CR model emission texture maps also remain outside the current normalized
block contract. Scalar emitted light is preserved. The importer recognizes
the source field rather than treating it as unknown, and the limitation is
kept explicit here instead of silently claiming full CR rendering parity.
