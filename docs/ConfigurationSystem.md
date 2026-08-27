# Configuration System

This document describes Rigel's current preference, streaming, rendering,
persistence, and generator-definition configuration paths.

---

## Index

- [Overview](#overview)
- [Global User Preferences](#global-user-preferences)
- [Config Sources and Precedence](#config-sources-and-precedence)
  - [Generator Definitions and Streaming](#generator-definitions-and-streaming)
  - [Rendering](#rendering)
  - [Persistence](#persistence)
- [Config Provider and Sources](#config-provider-and-sources)
- [Save-Owned World Identity](#save-owned-world-identity)
- [Generator Definitions](#generator-definitions)
- [Render Config](#render-config)
- [Persistence Config](#persistence-config)
- [Per-World Overrides](#per-world-overrides)
- [Limitations](#limitations)
- [Related Docs](#related-docs)

---

## Overview

Rigel uses layered configuration for the remaining streaming, rendering, and
persistence policy. Each layered type is loaded from a fixed stack of sources,
and later sources override earlier values. Configs are read once during
application bootstrap. Generator content is separate: a new world resolves one
strict named asset, while an existing world loads only its save-owned canonical
snapshot.

Fields merge according to their YAML shape:

- Scalars replace the earlier value.
- Objects and maps merge by key, so omitted keys retain their earlier values.
- Sequences replace the earlier sequence, including when the later sequence is
  empty.

Persistence's typed CR options merge by key.

Three layered config types are supported today:

- `StreamingConfig` (runtime chunk loading, generation, and meshing schedules)
- `WorldRenderConfig` (render pipeline settings)
- `PersistenceConfig` (save/load format and provider options)

Typed providers load each layered subsystem's settings from YAML input using
rapidyaml. `Voxel::WorldConfigProvider` supplies only `StreamingConfig` to
production bootstrap and ignores unrelated top-level keys. Rendering is loaded by
`Render::RenderConfigProvider`, and persistence by
`Persistence::PersistenceConfigProvider`. Each subsystem's bootstrap function
uses the shared standard-source builder, but the typed provider remains the
semantic owner of parsing and merging its settings. Unknown fixed keys produce
a warning and are not applied.

`UserPreferences` is separate from these layered providers. It owns global
player display, graphics, camera, and input requests in one platform file and
does not consult assets, saves, the working directory, or per-world overrides.

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

`ApplicationPreferences` is the direct runtime owner for applying input and
View Distance requests. `Application` exposes the sole public View Distance
session seam. A request only replaces the pending candidate; it does not change
requested preferences, active streaming, or rendering synchronously. After
events are collected for the next application frame, the owner derives and
validates one complete active-world policy, prepares the preference save,
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

## Config Sources and Precedence

Each config type has a fixed source order defined by its subsystem bootstrap.
The general rule is:

1) Embedded defaults (from assets).
2) Project-level overrides under `config/`.
3) Project root overrides (for quick testing).
4) Per-world overrides under `config/worlds/<worldId>/`.

### Generator Definitions and Streaming

The shipped manifest declares `generator_definitions/default`, whose asset is
`assets/generators/default.yaml`. New-world bootstrap resolves the selected
namespaced definition only after save inspection. Published-world startup does
not enumerate or load installed generator definitions.

Streaming sources (in order):

1. `assets/config/streaming.yaml` (embedded as `raw/streaming_config`)
2. `config/streaming.yaml`
3. `streaming.yaml`
4. `config/worlds/<worldId>/streaming.yaml`

These sources contain only streaming policy. Generator declarations, flags,
overlays, and stage lists are not interpreted by this path. Unavailable
installed generator content is irrelevant to published-world reload.

### Rendering

Sources (in order):

1. `assets/config/render.yaml` (embedded as `raw/render_config`)
2. `config/render.yaml`
3. `render.yaml`
4. `config/worlds/<worldId>/render.yaml`

### Persistence

Sources (in order):

1. `assets/config/persistence.yaml` (embedded as `raw/persistence_config`)
2. `config/persistence.yaml`
3. `persistence.yaml`
4. `config/worlds/<worldId>/persistence.yaml`

---

## Config Providers and Sources

Each typed provider aggregates neutral `Config::IConfigSource` instances:

- `EmbeddedConfigSource` reads an embedded raw asset (e.g.
  `raw/streaming_config`).
- `FileConfigSource` reads a file from disk.

Each source provides its complete YAML document and a name used in validation
errors. Providers prepare and validate a replacement value for each available
source before publishing it as the input to the next layer. There is no
cross-file overlay path or conditional flag mechanism.

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

## Streaming Config

`StreamingConfig` owns the remaining runtime chunk loading, generation, and
meshing schedule inputs under the `streaming` key. It uses the dedicated
streaming source order above. Player View Distance is absent from this YAML
domain. Unload distance and loader prefetch are derived active-world policy,
not streaming YAML inputs.

| Key | Type | Code fallback | Notes |
| --- | --- | --- | --- |
| `streaming.gen_queue_limit` | int | `0` | Generation dispatch cap before executor-capacity bounds (0 = no configured cap; maximum explicit cap `32768`). |
| `streaming.mesh_queue_limit` | int | `0` | Mesh dispatch cap before executor-capacity bounds (0 = no configured cap; maximum explicit cap `32768`). |
| `streaming.update_budget_per_frame` | int | `0` | Load/generation/missing-mesh requests advanced per update (0 = unlimited). |
| `streaming.apply_budget_per_frame` | int | `0` | Generation and mesh results applied per category (0 = unlimited). |
| `streaming.worker_threads` | int | `2` | Total worker count partitioned between generation and meshing. |
| `streaming.io_threads` | int | `1` | Region IO thread count. |
| `streaming.load_worker_threads` | int | `2` | Chunk payload build thread count; all three worker settings total at most `64`. |
| `streaming.load_apply_budget_per_frame` | int | `8` | Disk payload apply budget (0 = unlimited). |
| `streaming.load_region_drain_budget` | int | `32` | Region completion drain budget. |
| `streaming.load_queue_limit` | int | `0` | Pending disk load cap (0 = unlimited; maximum explicit cap `32768`). |
| `streaming.load_max_cached_regions` | int | `8` | Cached region cap (0 = unlimited, maximum `256`). |
| `streaming.load_max_inflight_regions` | int | `8` | Configured physical region-read cap (0 = no configured physical-read cap; maximum explicit cap `64`). The zero-cap fallback retains at most 64 speculative owners in the loader-owned queue; dispatched or pool-pending work is bounded separately by IO executor capacity, including one inline result when `io_threads` is 0. |
| `streaming.max_resident_chunks` | int | `0` | Resident chunk cache cap (0 = unlimited, maximum explicit cap `65536`). |

Region worker submission is also capped by the physical IO thread count.
Speculative owners remain in a direct-first loader queue bounded by
`load_max_inflight_regions`, or by a 64-owner fallback when that setting is
zero. That fallback does not include separately bounded dispatched or
pool-pending work, which may occupy the IO executor capacity. With no IO
worker, reads execute inline and at most one completed result remains
dispatched-undrained. The setting continues to control physical read
concurrency rather than rejecting direct chunk demand.

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
12. Per-frame budgets are limited to 32,768. These are operational ceilings for
Rigel's fixed 32-cubed chunks rather than integer or address-space maxima.
Tests and benchmarks may still construct `StreamingConfig` with exact view and
unload radii or set exact loader preload inputs as explicit developer
injection; an active application world replaces those values from its policy.

Negative queue, budget, thread, and cache values are clamped to zero.
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
`update_budget_per_frame` does not turn the desired-set rebuild into a partial
scan.

---

## Render Config

`WorldRenderConfig` is loaded from YAML under the optional `render` root node.
If `render` is absent, the root is used directly.

Defaults below reflect the code defaults from `WorldRenderConfig`. The embedded
config (`assets/config/render.yaml`) may override them.

### Quick Reference (Rendering)

| Key | Type | Default | Notes |
| --- | --- | --- | --- |
| `render.sun_direction` | vec3 | `[0.5, 1.0, 0.3]` | Directional light vector. |
| `render.transparent_alpha` | float | `0.5` | Alpha for transparent pass. |
| `render.shadow.enabled` | bool | `false` | Toggle cascaded shadows. |
| `render.shadow.cascades` | int | `3` | Cascade count (maximum `4`). |
| `render.shadow.map_size` | int | `1024` | Shadow map resolution (maximum `6144`). |
| `render.shadow.max_distance` | float | `200.0` | Internal profile distance, capped by the active View Distance policy. |
| `render.shadow.split_lambda` | float | `0.5` | Log/linear split blend. |
| `render.shadow.bias` | float | `0.0005` | Depth bias. |
| `render.shadow.normal_bias` | float | `0.005` | Normal-based bias. |
| `render.shadow.pcf_radius` | int | `1` | Fallback for near and far PCF radii (maximum `4`). |
| `render.shadow.pcf_radius_near` | int | `1` | Near PCF radius override (maximum `4`). |
| `render.shadow.pcf_radius_far` | int | `1` | Far PCF radius override (maximum `4`). |
| `render.shadow.transparent_scale` | float | `1.0` | Transparent attenuation. |
| `render.shadow.strength` | float | `1.0` | Shadow strength multiplier. |
| `render.shadow.fade_power` | float | `1.0` | Shadow fade exponent. |
| `render.taa.enabled` | bool | `false` | Toggle TAA. |
| `render.taa.blend` | float | `0.9` | History blend factor. |
| `render.taa.jitter_scale` | float | `1.0` | Subpixel jitter scale. |
| `render.profiling.enabled` | bool | `false` | Enable the per-frame profiler. |

`UserPreferences.graphics.view_distance_chunks` controls which chunks are
loaded and meshed. `WorldView` derives the renderer's world-unit culling range,
projection far plane, and shadow-distance ceiling from the accepted request.
`render.render_distance` is not a supported render YAML key.

Key fields:

- `sun_direction` (vec3)
- `transparent_alpha` (float)
- `shadow`:
  - `enabled`, `cascades`, `map_size`, `max_distance`
  - `split_lambda`, `bias`, `normal_bias`
  - `pcf_radius`, `pcf_radius_near`, `pcf_radius_far`
  - `transparent_scale`, `strength`, `fade_power`
- `taa`:
  - `enabled`, `blend`, `jitter_scale`

Values are clamped during load:

- `shadow.cascades` and `shadow.map_size` are clamped to at least `1`.
- `pcf_radius` and related values are clamped to at least zero.
- `taa.blend` is clamped to `[0, 1]`.

Shadow cascades, map dimensions, and PCF radii above their documented maxima
are rejected before renderer or streaming workers are created. The 6,144 map
limit matches the largest shipped configuration while placing a fixed bound on
both texture-array allocations. The PCF limit matches the implemented
four-texel shader kernel instead of accepting values that would be silently
reduced during rendering.

When a near or far PCF radius is not configured, it follows `pcf_radius`.
Specific near and far values take precedence across configuration layers, so a
later generic radius only updates sides that have no specific value.

`RIGEL_PROFILE=1` forces profiling on at runtime, regardless of config. Setting
`RIGEL_PROFILE=0` forces profiling off.

---

## Persistence Config

`PersistenceConfig` is loaded from YAML under the optional `persistence` root
node. If `persistence` is absent, the root is used directly.

Defaults below reflect the code defaults from `PersistenceConfig`. The embedded
config (`assets/config/persistence.yaml`) may override them.

### Quick Reference (Persistence)

| Key | Type | Default | Notes |
| --- | --- | --- | --- |
| `persistence.format` | string | `cr` | Preferred format ID. |
| `persistence.providers` | map | - | Typed backend options by ID. |
| `persistence.providers.rigel:persistence.cr.lz4` | bool | `false` | CR backend compression. |

Key fields:

- `format`: preferred format ID (default `cr`).
- `providers`: typed backend settings keyed by provider ID. The only supported
  configuration provider is `rigel:persistence.cr`.

CR's `lz4` option uses the exact lowercase `true`/`false` contract. Invalid
values fail with the source and
full key before a source layer is published. Example from the shipped config:

```yaml
persistence:
  format: cr
  providers:
    rigel:persistence.cr:
      lz4: false
```

The runtime persistence provider registry remains separate from configuration;
it carries objects such as the block registry and CR settings after bootstrap.

---

## Per-World Overrides

Per-world overrides are resolved by world ID. The default world ID is numeric
and used directly in the override paths:

- `config/worlds/<worldId>/streaming.yaml`
- `config/worlds/<worldId>/render.yaml`
- `config/worlds/<worldId>/persistence.yaml`

These files are optional and only override fields they define. As the last
source layer, they have the highest precedence.

---

## Limitations

- Layered engine/content configs and saved world identity are loaded once at
  startup. Supported UserPreferences changes use their direct live apply paths;
  View Distance requires an active world session.
- Validation is implemented by concrete owners rather than one generic schema
  engine. Generator definitions and save-owned settings are strict; other
  current config domains retain their documented parsing behavior.
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
