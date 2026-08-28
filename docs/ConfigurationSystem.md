# Configuration Ownership

This document describes Rigel's current configuration owners, their lifetimes,
and their supported inputs. Configuration is deliberately split by who makes a
decision:

| Owner | Lifetime | Supported source |
| --- | --- | --- |
| Player | Global across worlds | One tolerant `UserPreferences` file |
| Published world | Lifetime of one save | Strict `world-settings.yaml` and `generator-definition.yaml` inside the save |
| Content author | New-world creation input | Strict named `GeneratorDefinition` assets declared by the manifest |
| Engine | Process or world-session policy | Internal code, derived from player intent where applicable |
| Developer or benchmark | One diagnostic or experiment | Explicit environment variables, APIs, constructors, or benchmark arguments |

There is no shared configuration stack or universal configuration service.
Normal startup does not inspect the working directory for bare YAML files, a
`config/` directory, or numeric `config/worlds/<id>/` overrides.

## Player Settings

`Preferences::UserPreferences` owns personal and device requests. The current
player surface is exactly:

| Section | Player setting | Default | Accepted values |
| --- | --- | --- | --- |
| Display | Display Mode | Windowed | Windowed or Borderless |
| Display | Window Size | 800 x 600 | width and height from 1 through 16384 |
| Display | VSync | On | On or Off |
| Display | FPS Limit | Unlimited | Unlimited or an integer from 30 through 1000 |
| Graphics | View Distance | 12 chunks | 2 through 16 chunks |
| Graphics | Shadows | On | On or Off |
| Camera | Field of View | 60 degrees vertical | 50 through 110 degrees vertical |
| Controls | Mouse Sensitivity | 0.12 | 0.01 through 1.00 |
| Controls | Invert Y | Off | On or Off |
| Controls | Bindings | Shipped defaults | Sparse keyboard/mouse replacement lists |

View Distance is the only player streaming control. Renderer range, projection,
loading, preload, unload retention, and the shadow-distance ceiling are derived
engine policy. Shadows has one internal On profile; its cascade and filtering
values are not player settings.

On Linux, the production file is
`$XDG_CONFIG_HOME/rigel/user-preferences.yaml` when
`XDG_CONFIG_HOME` is absolute, and otherwise
`$HOME/.config/rigel/user-preferences.yaml`. A complete example is:

```yaml
schema_version: 1
display:
  mode: windowed
  windowed_size: [1280, 720]
  vsync: true
  fps_limit: unlimited
graphics:
  view_distance_chunks: 12
  shadows: true
camera:
  vertical_fov_degrees: 70
input:
  mouse_sensitivity: 0.12
  invert_y: false
  bindings:
    move_forward: [W, UP]
    remove_block: [MOUSE_LEFT]
    place_block: [MOUSE_RIGHT]
```

The binding map is sparse. An absent action inherits the required player
default from `assets/manifest.yaml`; an empty list explicitly unbinds that
action. The supported actions are move forward, backward, left, and right;
ascend; descend; sprint; remove block; and place block. See
`docs/InputSystem.md` for symbolic token names and frame-boundary behavior.

Rigel has no player settings for exclusive fullscreen, monitor or refresh-rate
selection, anti-aliasing, shadow tiers, render or UI scale, audio, or
controllers. TAA is an internal diagnostic path and ships Off.

### Tolerant loading and atomic persistence

Schema 1 is tolerant per field. An invalid known leaf warns and uses that
leaf's shipped default without discarding valid siblings. Unknown fields and
unknown binding actions warn and are ignored; unknown schema-1 nodes are
retained when Rigel later saves known values. A missing file returns defaults
without creating a file.

Malformed, missing-schema, unsupported-schema, unreadable, oversized, and
nonregular files also return safe defaults, but ordinary saves are blocked so
Rigel cannot overwrite an unsupported or damaged document implicitly.
`replaceWithRequested()` is the explicit destructive replacement operation.

Writes serialize one complete document, take the sibling advisory lock, stage
replacement bytes, and publish atomically. For a prepared live change, a
definite prepublication failure keeps the prior requested state and restores
the prior effective state; failure to restore a fallible physical display is a
fatal error. The observation-driven manual-resize path below is the exception.
If replacement happened before directory durability became uncertain, Rigel
reports that distinct result and keeps the newly published request and
effective state.

### Requested and effective display state

The persisted display tuple is the player's requested state. The effective
state describes the state Rigel is currently using; the two may deliberately
differ:

- Borderless uses the current desktop bounds while preserving the independent
  remembered Window Size for a return to Windowed mode.
- `RIGEL_CHUNK_BENCH=1` forces effective VSync Off and effective FPS Limit to
  Unlimited without changing the persisted request.
- If the requested startup display cannot be created, Rigel tries a fixed safe
  800 x 600 Windowed display. It keeps the failed request on disk instead of
  silently replacing it.
- Startup logs the requested and effective display tuples. In Borderless mode,
  the tuple's size remains the remembered Window Size; the actual window uses
  the current desktop bounds.

A supported live display change prepares the preference write before changing
the window or swap interval. A reported nonfatal failure retains or restores
the prior working display; failure of the physical rollback is fatal.

A manual Windowed resize is observed after the window has already changed, so
it changes the effective remembered size immediately and saves it after a short
debounce. If preparation is blocked or publication definitely fails, the new
effective size remains active while the requested and on-disk size remain
unchanged. Definite publication failure is retried once; if that retry also
fails, or preparation was blocked, automatic attempts stop for that size and
the unsaved resize is reported again during close or cleanup. A distinct later
resize starts a new persistence attempt. Borderless transitions never erase
the remembered Window Size.

### Runtime mutability

The preference file is loaded at startup and is not hot-reloaded. Runtime
changes go through the direct `Application`/`ApplicationPreferences` owner:

| Setting | Current apply behavior |
| --- | --- |
| Display Mode and Window Size | Live main-thread window transition; nonfatal failure rolls back |
| VSync | Live swap-interval change; nonfatal failure rolls back |
| FPS Limit | Live frame-pacer change; benchmark mode still controls the effective value |
| View Distance | Coalesced until the next application frame boundary, then one policy is applied to the active view and loader |
| Shadows | Live active-world resource change; the complete replacement is prepared before the old resources are retired |
| Field of View | Immediate renderer update using the same vertical FOV for normal and diagnostic projections |
| Mouse Sensitivity and Invert Y | Immediate effective input update |
| Sparse bindings | Complete candidate compiled first, then installed at `InputState::beginFrame()` without manufactured input edges |

Shadows and View Distance require an active world session. Reset Controls clears
only sparse binding replacements and preserves sensitivity and Invert Y.

## Save-Owned World Settings

A published world owns its generation identity under the existing
`saves/world_<worldId>` root:

- `world-settings.yaml` stores World Name, the actual seed, readable generator
  provenance, and the distinct version values.
- `generator-definition.yaml` stores the canonical normalized generator
  snapshot used at runtime.
- The selected persistence backend stores its authoritative format marker.

These files are strict save data, not another player-preference file. They are
staged with the backend marker and the complete directory is published before
the world can generate a chunk. A failed creation does not expose a partial
final save root. A backend may repeat the display name for byte-format
compatibility, but that copy is derived; `world-settings.yaml` remains the
configuration authority.

### World creation

A player-facing world creation surface has only these inputs:

| Creation field | Meaning |
| --- | --- |
| World Name | The save-owned display name |
| Seed | The actual integer seed persisted before generation |
| World Type | The selected definition's human-readable label and description |

World Type does not expose a raw asset ID, revision, density graph, caves,
structures, or pipeline stages. Installed definitions are validated before
they can be offered by label and description.

The current prototype has no world-creation screen. When `saves/world_0` is
absent, the application supplies World Name `world_0`, Seed `1337`, and World
Type **Default** (`Rigel's standard continents, biomes, and caves.`). Those are
creation inputs, not fallback values for an existing save.

For reference, the resulting strict settings document has this shape and uses
the currently supported versions:

```yaml
world:
  schema_version: 2
  display_name: "world_0"
  seed: 1337
  generator:
    id: "rigel:default"
    source_revision: 1
    definition_schema_version: 2
    semantics_version: 1
```

The player-facing World Type is **Default**; `rigel:default` is stored only as
provenance. The current application has no supported post-creation World Name,
Seed, or World Type edit operation.

### Version meanings

The version values are intentionally independent:

- `world.schema_version` decodes the `WorldSettings` document. Rigel currently
  supports version 2.
- `definition_schema_version` decodes the canonical snapshot's data shape.
  Rigel currently supports generator definition schema 2.
- `source_revision` records the author's revision of the named installed
  definition for provenance and diagnostics. It never overrides the snapshot.
- `semantics_version` selects the generator evaluator behavior needed to
  interpret the snapshot. Rigel currently supports semantics version 1.

An unknown newer settings, definition, or semantics version is a load error.
Rigel does not reinterpret it using the closest supported version.

### Snapshot authority

Installed `GeneratorDefinition` assets are creation inputs only. On reload,
Rigel does not enumerate them or substitute an installed definition with a
matching ID. The save-local snapshot is the sole generator definition input,
and `WorldSettings.seed` supplies its seed.

The snapshot is normalized runtime data. It contains sorted effective content
and only graph nodes reachable from the terrain and enabled cave outputs. It
does not copy comments, author labels and descriptions, aliases, overlays,
flags, inactive sections, unreachable nodes, the seed, or source revision.
Parsing accepts only the supported canonical representation. Referenced block
content must still exist; a missing saved reference rejects the world instead
of selecting a replacement material or generator.

### Clean break and legacy refusal

The ownership model is a clean break from the former shared YAML sources. A
pre-existing root without the complete supported settings, canonical snapshot,
and authoritative backend marker is legacy, incomplete, or unknown. Startup
rejects it before installing a generator or mutating chunk, entity, metadata,
or region data. Rigel does not infer a seed or provenance, trust placeholder CR
metadata, use Seed 1337, substitute the current Default definition, or generate
unexplored chunks. There is no legacy importer.

The save root and the valid CR chunk, entity, and compressed/uncompressed
region encodings are unchanged. Refusal leaves the existing root and its files
in place for inspection; configuration identity compatibility is separate from
backend byte-format readability.

## Strict Generator Definition Authoring

Generator definitions are namespaced content assets declared through the
existing manifest. The shipped declaration is:

```yaml
namespace: base
assets:
  generator_definitions:
    default:
      path: generators/default.yaml
```

`assets/generators/default.yaml` is the complete working author example. A
definition owns its schema version, namespaced ID, positive source revision,
human label and description, finite bounds, terrain materials, climate, biome
targets and surfaces, density graph, caves, and structures.

Authoring is graph-only. The engine derives one fixed stage order from validated
feature sections. There is no simple-terrain mode, configurable pipeline,
custom stage or generator plugin, inheritance, flags, conditional expression,
or overlay mechanism. Caves and structures belong to the definition; an author
publishes a distinct complete definition when those choices differ.

Validation is strict before save staging. Unknown or type-inapplicable fields,
unknown graph node or climate types, duplicate definition IDs, duplicate or
dangling graph references, cycles, missing required outputs, invalid bounds,
incoherent feature dependencies, invalid biome filters, unavailable materials,
and unreachable graph nodes are errors.
There is no silent constant, temperature, zero-output, unrestricted-filter, or
installed-default fallback.

## Internal Engine Policy

Installed Rigel has no public or shipped `EngineConfig`. Normal scheduler,
loader, renderer, and persistence decisions are code-owned:

- automatic worker topology and bounded queue/application policy;
- load/preload/unload policy derived from the player's View Distance;
- one Shadows On profile and internal TAA, projection, and art tuning;
- CR as the installed new-save backend policy, with existing saves always
  using their authoritative stored format.

Exact scheduler capacities and renderer mathematics are implementation details,
not ordinary configuration. Their current behavior and test evidence live in
`docs/WorldGeneration.md`, `docs/RenderingPipeline.md`, and
`docs/DebugTooling.md`.

## Explicit Developer and Benchmark Inputs

Developer inputs are opt-in and do not add another source layer:

- `RIGEL_PROFILE=1` enables profiler collection when instrumentation is
  compiled. Other values leave it Off.
- `RIGEL_CHUNK_BENCH=1` enables application chunk statistics and removes VSync
  and FPS pacing from the effective benchmark display without rewriting player
  preferences.
- `RIGEL_BUILD_TESTS` and `RIGEL_BUILD_BENCHMARKS` are build options, not
  runtime settings.
- Benchmark executables accept their documented exact CLI inputs for such
  values as worker count or queue limits. Tests inject exact policy through
  constructors and setters.
- `WorldView::setRenderProfileForDiagnostics()` is the explicitly named
  low-level renderer seam. Normal application startup never calls it.
- Debug/profiler/demo input actions are fixed developer controls and remain
  separate from player binding defaults and sparse overrides.

These inputs are not persisted to `user-preferences.yaml` or a world save and
are not supported as installed-game configuration. See `docs/DebugTooling.md`
for benchmark commands and diagnostic output.

### Obsolete-path diagnostics

Debug startup checks only the exact retired `world_generation.yaml`,
`streaming.yaml`, `render.yaml`, `persistence.yaml`, and
`worldgen_overlays/no_carvers.yaml` locations associated with the active world.
If an entry exists, Rigel warns that it is ignored and names the current owner
or replacement. It does not parse the entry, enumerate directories, or search
arbitrary roots. Release startup does not perform this diagnostic.

## Related Documentation

- `docs/InputSystem.md`
- `docs/WorldGeneration.md`
- `docs/RenderingPipeline.md`
- `docs/PersistenceAPI.md`
- `docs/PersistenceBackends.md`
- `docs/MultiWorld.md`
- `docs/AssetSystem.md`
- `docs/DebugTooling.md`
