# Configuration System

This document describes Rigel's current preference, internal engine policy,
persistence, and generator-definition configuration paths.

---

## Index

- [Overview](#overview)
- [Global User Preferences](#global-user-preferences)
- [Ownership and Sources](#ownership-and-sources)
- [Save-Owned World Identity](#save-owned-world-identity)
- [Generator Definitions](#generator-definitions)
- [Streaming Policy](#streaming-policy)
- [Render Policy](#render-policy)
- [Persistence Policy](#persistence-policy)
- [Limitations](#limitations)
- [Related Docs](#related-docs)

---

## Overview

Rigel separates configuration by owner and lifetime. `UserPreferences` owns
global player requests in one platform file. A published save owns its world
settings, generator provenance, normalized generator snapshot, and persistence
format identity. The installed manifest supplies strict generator definitions
only when creating a world. Streaming, renderer, and desktop persistence policy
are internal code.

There is no shared configuration source stack. Normal startup does not inspect
the process working directory for bare YAML files, a `config/` directory, or
numeric `config/worlds/<id>/` overrides.

---

## Global User Preferences

`Preferences::UserPreferences` is a schema-version-1 typed value with these
shipped defaults:

| Request | Default | Accepted values |
| --- | --- | --- |
| Display mode | `windowed` | `windowed`, `borderless` |
| Remembered window size | `[800, 600]` | two dimensions from 1 through 16384 |
| VSync | `true` | boolean |
| FPS limit | `unlimited` | `unlimited`, or 30 through 1000 |
| View distance | `12` chunks | 2 through 16 |
| Shadows | `true` | boolean |
| Vertical FOV | `60` degrees | finite value from 50 through 110 |
| Mouse sensitivity | `0.12` | finite value from 0.01 through 1.00 |
| Invert Y | `false` | boolean |
| Binding overrides | none | sparse lists for supported gameplay actions |

On Linux, the sole production path is
`$XDG_CONFIG_HOME/rigel/user-preferences.yaml` when `XDG_CONFIG_HOME` is
absolute, otherwise `$HOME/.config/rigel/user-preferences.yaml`. Tests and
tools construct `UserPreferencesStore` with one explicit absolute path. A
missing file returns defaults without creating the file.

Supported schema-1 documents are parsed independently per field. An invalid
leaf or section warns and uses the shipped default for only that leaf or
section; valid siblings remain usable. Unknown fields and binding actions warn
and are ignored. Unknown schema-1 nodes are retained when known requests are
later saved. Binding overrides distinguish an absent action from an explicit
empty list. Binding lists accept bounded symbolic keyboard and mouse tokens;
raw multi-digit GLFW codes and unknown tokens invalidate only that action's
override.

Malformed, missing-schema, unsupported-schema, unreadable, oversized, and
nonregular files return safe defaults without changing the existing path.
Ordinary saves remain blocked after such a load, and also recheck the current
file while holding a sibling `user-preferences.yaml.lock` advisory lock through
atomic publication, so a newer schema installed by another cooperating Rigel
writer is not overwritten. The lock file contains no preferences and is never
read as a configuration source. It serializes cooperating writers only;
external editors and processes that ignore the lock must not write the file
concurrently. `replaceWithRequested()` is the explicit operation for replacing
an unsupported document and uses the same lock.

Writes stage complete replacement bytes and publish them atomically. Commit
errors distinguish a definite prepublication failure from a replacement that
was published before parent-directory durability became uncertain. Both loaded
and serialized documents are limited to 262144 bytes; retained unknown fields
cannot make a saved replacement exceed the load bound, and an oversized result
leaves the previous file intact. Requested and effective preferences are
separate values: hardware recovery may select a safe effective value, while
persistence always writes the requested value. The store does not itself apply
requests to window, renderer, streaming, or input consumers.

`ApplicationPreferences` is the direct runtime owner for applying input,
Shadows, and View Distance requests. Shadows are one On/Off request; the
low-level cascade, texture, and filtering fields are not user preferences.
Enabling validates the renderer-owned profile and prepares complete depth,
transmittance, and framebuffer resources before swapping them into the active
view. Disabling swaps in an empty resource set. A definite preference
publication failure restores the prior effective resources and request;
durability uncertainty keeps the already-published request and effective
state. Startup preparation failure leaves the persisted request intact and
reports Shadows as effectively Off.

`Application` exposes the public active-session seams for Shadows and View
Distance. A View Distance request only replaces the pending candidate; it does
not change requested preferences, active streaming, or rendering synchronously.
After events are collected for the next application frame, the owner derives
and validates one complete active-world policy, prepares the preference save,
applies that same immutable policy to the world and loader, and publishes the
save. Repeated requests before that boundary supersede the pending candidate.
A definite preparation or publication failure restores the prior complete
policy and requested value.

Sensitivity and invert-Y update the cursor callback immediately. A binding
candidate is compiled against the required nine-action manifest asset without
mutating the cached asset, then queued for `InputState::beginFrame()`. A
definite preparation or publication failure retains the prior requested and
effective state. If replacement bytes were published but directory durability
is uncertain, the complete candidate remains requested and visible. Resetting
controls clears only the sparse binding map, preserving sensitivity and
invert-Y while restoring manifest inheritance.

---

## Ownership and Sources

- Global player requests load from the one absolute platform
  `user-preferences.yaml` path described above.
- The shipped manifest declares `generator_definitions/default`, backed by
  `assets/generators/default.yaml`. New-world bootstrap resolves that creation
  input only after confirming that no published save identity exists.
- Published worlds load their settings and canonical generator snapshot from
  the existing `saves/world_<id>` root. They do not enumerate installed
  generator definitions.
- Normal streaming, renderer, and persistence behavior comes from internal
  policy. Tests and benchmarks inject exact typed values at their existing
  construction seams.

Debug startup checks a finite list of exact retired
`world_generation.yaml`, `streaming.yaml`, `render.yaml`, `persistence.yaml`,
and `worldgen_overlays/no_carvers.yaml` locations for the active world. Existing
entries produce an actionable warning but are never parsed. Release startup
does not perform this check. Neither path enumerates directories or treats the
working directory as configuration authority.

---

## Save-Owned World Identity

Every newly created world publishes two format-independent files under the
existing `saves/world_<worldId>` root:

- `world-settings.yaml` owns the settings schema version, display name, actual
  seed, generator source ID and revision, definition schema version, and
  generator semantics version.
- `generator-definition.yaml` is the canonical resolved generator runtime
  snapshot.

Persistence backends may repeat the display name for byte-format compatibility,
but that copy is derived. Reload uses `world-settings.yaml` even when the
backend copy is stale; the backend's world ID and format marker still identify
the save and must remain valid.

Both documents are durably committed in a unique sibling staging directory,
along with the selected persistence backend's authoritative metadata marker,
then the complete directory is atomically published without replacing an
existing root. Failed creation never exposes a partial final root. A root
containing only one identity file, or lacking a decodable authoritative backend
marker, is incomplete and is not loaded or repaired from current preferences.
When a final root exists, startup validates its settings, snapshot, referenced
content, and backend identity before removing any deletion-authorized staging
sibling. A missing final root may still resume a valid publication handoff.
Markerless or ambiguous staging entries are preserved in the bounded slot
namespace for inspection. Existing save data without both files is legacy or
unknown and is rejected without mutation.

The seed and source revision are deliberately absent from the generator
snapshot. The seed and evaluator semantics version are applied from
`WorldSettings` when the snapshot is decoded; source revision remains provenance
only. Snapshot
serialization also omits aliases, comments, authoring-only metadata, and graph
nodes unreachable from runtime outputs.
Snapshot parsing is strict and accepts only the canonical representation and
supported schema/semantics versions.

---

## Generator Definitions

Production generation is graph-only. New-world creation selects a named
`generator_definitions` asset; the shipped bootstrap selects `rigel:default`
from `assets/generators/default.yaml`. An installed definition owns:

- schema version, namespaced ID, positive source revision, label, and
  description;
- finite inclusive bounds and explicit solid/water materials;
- sea level, terrain output name, climate, biome targets, coast range,
  per-biome `water_fill`, and surface layers;
- the density graph, optional caves, and optional structures.

Definitions are strict. Unknown or type-inapplicable fields, unknown node or
climate types, duplicate IDs, dangling references, cycles, missing terrain or
cave outputs, invalid biome/material references, invalid bounds, and
incoherent cave/structure dependencies fail before staging a save. All declared
definitions are validated as one set, and IDs are unique across that set.

The canonical save snapshot contains normalized effective runtime data. It
sorts keyed content, retains only graph nodes reachable from terrain and enabled
cave outputs, and omits author metadata, aliases, comments, overlays, flags,
inactive sections, and noncanonical scalar terrain forms. The evaluator consumes the
same `GeneratorDefinitionData` represented by those bytes.

Engine code derives the fixed stage sequence: global climate, local climate,
biome resolution, terrain density, optional caves, surface rules, and optional
structures. Authors enable caves and structures through their definition-owned
sections; there is no author-facing pipeline or simple terrain mode.

---

## Streaming Policy

Installed startup constructs one internal automatic policy. It neither parses
nor ships YAML for scheduler queues, frame budgets, worker partitioning,
loader queues/caches/inflight work, prefetch, unload distance, or resident
chunk caps. There is no player, content, or save-owned engine-policy document.

The current topology normalizes detected logical processors to 4 through 64.
It selects one IO worker below eight processors and two otherwise, clamps
payload-build workers to `processors / 5` in the range 1 through 4, and assigns
the remainder to generation/mesh scheduling in the range 2 through 12.
`ChunkStreamer` divides those scheduler workers between its generation and
mesh pools. Unknown host topology uses the four-processor policy.

Current internal capacities preserve bounded startup behavior: generation and
mesh queue limits are 128, request advancement is 4096 per update,
generation/mesh application is 128 per category, disk payload and region
drains are 16, the region cache and inflight owner limits are 16, and no raw
resident-chunk cap is enabled. These are implementation policy rather than a
supported configuration contract. Scheduler tests construct `StreamingConfig`
directly and loader tests use explicit constructors/setters; developer
benchmarks accept only the exact CLI inputs needed for reproducible runs.

The sole player request is
`UserPreferences.graphics.view_distance_chunks`, with a default of 12 and an
inclusive range of 2 through 16. For an accepted radius `N`, one immutable
active-world policy owns all dependent values:

- desired/load radius: `N` chunks;
- unload radius: `N + 1` chunks;
- renderer culling distance and shadow-distance ceiling: `(N + 1) * 32`
  world units;
- projection far plane: the greater of 500 world units and the culling
  distance plus one 32-unit chunk;
- loader preload radius: `clamp(N / persistence-region-span, 1, 2)` regions,
  with at most 12 speculative regions admitted for one request.

Only `N` is persisted. The derived values and policy generation are runtime
state. The world view, chunk streamer, asynchronous loader, frame projection,
and shadow submission consume the same policy instance. A later streaming or
render-config assignment cannot replace its distances.

The one-chunk hysteresis is covered by a deterministic lifecycle regression.
It preloads a sparse radius-12 sphere with one solid boundary probe, settles to
quiescence, moves one chunk on the X axis, settles again, reverses to the origin,
and settles a third time. The two runs differ only in unload distance, and no
terrain generation is involved. With unload distance 12, the move entered 441
chunks, evicted 441, and left 7,153 resident. Reversal then reloaded 441,
generated none, evicted 441 from the opposite side, started no remesh jobs, and
returned to 7,153 residents. With unload distance 13, the move entered the same
441 chunks, evicted none, and left 7,594 resident; reversal caused no loads,
generation, eviction, or remeshing and retained 7,594 residents. This evidence
supports radius 13 for the tested one-chunk reversal without implying general
CPU or byte savings.

The immediate retention cost in that regression is 441 chunks, or 6.2% over
the 7,153 radius-12 residents. This is not the full distance-bound increase:
inclusive integer spheres contain 7,153 coordinates at radius 12 and 9,171 at
radius 13, a worst increase of 2,018 coordinates, or 28.2%. A fully allocated
chunk has 128 KiB of block arrays, so the dense block-array bounds for those
deltas are 55.125 MiB and 252.250 MiB, respectively. Chunks allocate block
subchunks sparsely, and these figures exclude chunk and allocator overhead,
asynchronous copies, and currently unmetered CPU and GPU mesh memory. They are
block-storage bounds, not measurements of total resident memory.

The player view limit bounds a synchronous desired-set rebuild to 35,937 cube
candidates and 17,077 selected sphere coordinates. Its derived one-chunk
retention radius contains at most 20,479 sphere coordinates. Normal preload
scans at most 124 radius-two neighboring region coordinates and admits at most
12. These are operational ceilings for Rigel's fixed 32-cubed chunks rather
than integer or address-space maxima.
Tests and benchmarks may still construct `StreamingConfig` with exact view and
unload radii or set exact loader preload inputs as explicit developer
injection; an active application world replaces those values from its policy.

The desired set is rebuilt only when the camera enters a different chunk, a
distance changes, or the internal streamer generator assignment changes its
vertical clip. Increasing View Distance publishes the new policy immediately
at the application boundary and bounded scheduler work fills the larger set.
Decreasing it changes render culling at that boundary; load/generation
cancellation, desired-set reconciliation, persistence-gated eviction, and
neighbor remeshing advance during streaming updates. Retained residents are
reconciled in deterministic batches of at most 64 per update. A newer live
edit replaces the same pending policy transition instead of adding another
reconciliation queue. Save-owned generator definitions are not
live-replaceable.
The internal request-advancement budget does not turn the desired-set rebuild
into a partial scan.

---

## Render Policy

Normal startup uses one code-owned `RenderProfile`. There is no render YAML,
raw render asset, project-root override, per-world override, or saved renderer
document. The sole player-owned renderer cost requests are View Distance and
Shadows On/Off. `WorldView::setRenderProfileForDiagnostics()` is the explicit
low-level replacement seam for tests and developer diagnostics; application
startup does not call it.

The shipped Shadows On profile uses three 6,144-pixel cascades, a 200-world-unit
internal distance limit, a 0.25 split blend, 0.001 depth bias, 0.02 normal bias,
near/far PCF radii of 2/3, transparent scale 1, strength 3, and fade power 1.
The active View Distance policy independently supplies a shadow-distance
ceiling each frame. The effective distance is the smaller of that ceiling and
the profile limit, and the derived value is never written back into the
profile.

TAA remains an internal experiment, disabled in the shipped profile, with a
0.95 history blend and 1.0 jitter scale. There is no anti-aliasing player or
world setting. The sun direction `[0.5, 1.0, 0.3]`, transparent alpha `0.5`,
clear color, and lighting weights are shipped art constants rather than an
environment configuration domain.

Profiler collection is separate developer tooling. Debug builds compile the
instrumentation, collection defaults Off, and only the exact environment value
`RIGEL_PROFILE=1` enables it. It is absent from render, world, and player state.

---

## Persistence Policy

The installed desktop application creates new saves with the CR backend and
LZ4 region compression disabled. These are code-owned bootstrap decisions, not
YAML settings. Once creation publishes the save, its authoritative format
marker controls every reload even if installed policy later changes.

The runtime persistence provider registry carries concrete dependencies such
as the block registry and the internal CR settings object. Tests may construct
contexts with the Memory backend or enable CR LZ4 explicitly to exercise those
implementations. Normal startup has no persistence asset, bare-root file,
`config/` source, or `config/worlds/<id>/` source.

---

## Limitations

- Installed persistence policy and saved world identity are resolved once at
  startup. Supported UserPreferences changes use their direct live apply paths;
  Shadows and View Distance require an active world session.
- Validation is implemented by concrete owners rather than one generic schema
  engine. Generator definitions and save-owned settings are strict;
  UserPreferences parsing is tolerant per field.
- Generator definitions have no inheritance, overlay, flag, or author-facing
  stage-pipeline mechanism.
- Shipped player binding defaults are content in the asset manifest; sparse
  global user replacements are `UserPreferences`.

---

## Related Docs

- `docs/WorldGeneration.md`
- `docs/RenderingPipeline.md`
- `docs/PersistenceAPI.md`
- `docs/MultiWorld.md`
- `docs/AssetSystem.md`
- `docs/EmbeddedAssets.md`
- `docs/InputSystem.md`
