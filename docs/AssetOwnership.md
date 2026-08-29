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
