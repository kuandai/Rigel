# Asset ownership

Rigel's source tree and Cosmic Reach's distribution have separate ownership.
Git stores assets authored for Rigel. A developer-provided Cosmic Reach JAR is
the source for compatible Cosmic Reach runtime content; generated derivatives
live under `.rigel/assets/` and are never committed.

## Migration baseline

At commit `e5659da`, the tracked `assets/` tree contained 308 files:

| Category | Tracked files | Ownership at the baseline |
| --- | ---: | --- |
| Block definitions | 249 | Cosmic Reach-derived normalized snapshots |
| Entity models | 15 | 14 Cosmic Reach-derived, 1 Rigel demo model |
| Entity animations | 7 | 6 Cosmic Reach-derived, 1 Rigel demo animation |
| Sounds | 16 | Cosmic Reach-derived |
| Generator definitions | 1 | Rigel |
| Shaders | 19 | Rigel |
| Manifest | 1 | Rigel |

The checkout also relied on 243 ignored files under `assets/textures/`. The
tracked block definitions referenced 147 texture paths, so an apparently clean
checkout did not contain a complete interactive runtime asset set.

The Cosmic Reach 0.6.1 JAR used to validate the migration contains its content
under the `base/` prefix. Its identity is the SHA-256 digest
`58a2cc3b79b5413cfa0f2e4ae3b37f44ed7f11a5e828de57f9f3f71599ac570e`.
The digest records the migration input without making the JAR or an absolute
machine path part of the repository.

## Logical runtime contract

Physical ownership does not change resource identifiers. Both tracked
`assets/` and generated `.rigel/assets/` contribute paths relative to their
root to one embedded resource registry. A duplicate logical path is an error.

Current world generation requires these generated block identifiers:

- `base:stone_shale`
- `base:grass`
- `base:dirt`
- `base:sand`
- `base:water[type=source]`

`base:air` remains the registry's built-in empty block identity, while an
imported air definition supplies the corresponding source semantics.

## Source ownership contract

- `assets/` contains only Rigel-authored content after migration.
- `.rigel/source/Cosmic-Reach.jar` is an optional, ignored staging location.
- `.rigel/assets/` is a deterministic, ignored importer output.
- `.rigel/cosmic-reach-import.json` records source and output identity.
- Cosmic Reach content is neither downloaded nor discovered by the runtime.
- Source-only builds and tests do not require proprietary content.

## Local workflow

The cohesive importer CLI supports six operations:

```bash
python3 scripts/rigel_assets.py stage /path/to/Cosmic-Reach.jar
python3 scripts/rigel_assets.py sync
python3 scripts/rigel_assets.py status
python3 scripts/rigel_assets.py validate
python3 scripts/rigel_assets.py snapshot --output /path/to/build/snapshots \
    --jar-sha256 <hash>
python3 scripts/rigel_assets.py retire-snapshots \
    --output /path/to/build/snapshots --retain /path/to/build/snapshots/<hash>
```

`sync` constructs and validates a complete staging tree, then publishes it with
its provenance under an interprocess lock. Status, validation, snapshot, and
synchronization checks use the same lock. Abandoned staging trees are reclaimed,
and a retained previous generation restores a coherent tree and provenance pair
when publication is interrupted. A failed import preserves the previous valid
tree; removed source assets disappear on the next successful import. Provenance
records the source JAR SHA-256, importer schema and source hash, deterministic
output-tree SHA-256, source prefix, and category counts without timestamps or
machine-specific paths. Block-model provenance also records the support-schema
version, generated geometric candidates, recovered states, corrected base
approximations, and sorted state identifiers under disjoint plane/mixed and
nonstandard-texture omission reasons. Changing the support schema makes an
otherwise coherent import non-current and forces regeneration.
Block-collision provenance separately records its support schema, the empty,
full, single-partial, and multi-box registration counts, and exact, conservative
fallback, and ambiguous derivation counts. Validation reconciles those shape
counts against the generated block documents before accepting the provenance.

CMake resolves a JAR in this order: explicit `RIGEL_COSMIC_REACH_JAR` cache
path, the environment variable of the same name, then the canonical staged
file. It invokes `sync`, then copies the coherent generated tree under the lock
to a content-addressed build-directory snapshot before enumerating resources.
Assembly inputs remain on that immutable snapshot if a later import publishes a
new generation. Before copying a replacement, snapshot recovery reads the
atomically replaced generated assembly and retains its referenced generation.
The incoming tree is the only additional candidate, bounding snapshots to two
complete trees even before the first handoff marker exists. Once resource
enumeration has handed off the new paths, CMake marks that generation active and
retires its predecessor. Synchronization skips reconstruction when the JAR,
importer, and generated-tree hashes are current.
