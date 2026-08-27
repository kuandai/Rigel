# Input System

Rigel keeps physical device state in each `Application` instance and resolves
that state through one effective semantic binding map. Shipped player defaults,
sparse global user overrides, and developer-only controls have distinct owners.

## Ownership

- `assets/manifest.yaml` is the sole shipped-default source for the nine player
  actions.
- `UserPreferences.input.bindings` stores sparse per-action replacements. An
  absent action inherits the manifest list; an empty list explicitly unbinds
  it.
- `ApplicationPreferences` validates and compiles a complete candidate without
  changing the cached manifest asset. It publishes the requested preference
  and queues the effective map for the next frame boundary.
- Debug overlay, profiler overlay, demo entity spawn, and prototype mouse
  capture remain fixed developer/prototype bindings. They are not manifest
  player defaults and cannot be changed through `UserPreferences`.

The manifest is required at startup. Missing, malformed, incomplete, or extra
player actions are startup errors; Rigel does not synthesize a second default
table in C++.

## Binding Values

`InputBindings` stores `action -> physical-input list`. A physical input is a
typed keyboard key or mouse button, so keyboard and mouse codes cannot alias.
One bounded symbolic decoder is shared by manifest loading and user-preference
compilation.

Supported keyboard tokens include single letters and digits, `F1` through
`F25`, Space, Tab, Enter, Escape, Backspace, Insert, Delete, Home, End,
Page Up/Down, arrow keys, lock keys, Print Screen, Pause, and left/right Shift,
Control, Alt, and Super aliases. Mouse tokens are `MOUSE_LEFT`, `MOUSE_RIGHT`,
`MOUSE_MIDDLE`, and `MOUSE_4` through `MOUSE_8`. Parsing is case-insensitive;
spaces and hyphens normalize to underscores. Raw multi-digit GLFW codes and
unknown names are rejected.

The shipped player defaults are:

| Action | Physical input |
| --- | --- |
| `move_forward` | W |
| `move_backward` | S |
| `move_left` | A |
| `move_right` | D |
| `ascend` | Space |
| `descend` | Left Control |
| `sprint` | Left Shift |
| `remove_block` | Left Mouse |
| `place_block` | Right Mouse |

The fixed developer/prototype bindings are F1 for `debug_overlay`, F3 for
`imgui_overlay`, F2 for `demo_spawn_entity`, and Tab for
`toggle_mouse_capture`. Duplicate physical inputs across actions emit one
warning for a compiled map but remain active.

## Per-Frame State

GLFW key and mouse callbacks update pending physical arrays. At
`InputState::beginFrame()` Rigel:

1. publishes pending physical held and edge state;
2. installs any queued binding candidate;
3. when a map changed, restores its semantic held baseline from the physical
   state captured when that candidate was queued, so inputs already held at
   that point do not manufacture edges;
4. derives semantic edges from that queue-time baseline or from the prior
   frame state, preserving physical transitions that arrived after the
   candidate was queued; and
5. notifies action listeners.

Pressing a second alternative while an action is held does not create another
semantic press. Releasing one alternative while another remains held does not
create a release. A complete press/release tap between frames retains both
edges. Focus loss releases pending held inputs so actions do not remain stuck.

The main queries are `isActionPressed()`, `isActionJustPressed()`, and
`isActionJustReleased()`. Direct physical queries remain available for device
diagnostics, but gameplay movement and block edits use semantic actions.

## Mouse Look and Cursor Capture

The cursor callback reads the effective global mouse sensitivity and invert-Y
values owned by `ApplicationPreferences`. Sensitivity and invert-Y changes are
visible immediately; binding-map changes wait for `beginFrame()`. The first
cursor sample after capture or focus change only establishes the position
baseline. Pitch remains clamped to -89 through 89 degrees.

`setCursorCaptured()` selects disabled or normal cursor mode, enables raw mouse
motion where supported, and resets the first-sample guard. A future normal
pause/settings surface can replace the prototype Tab capture binding without
putting that prototype action into player preferences.

## Related Docs

- `docs/ApplicationLifecycle.md`
- `docs/ConfigurationSystem.md`
- `docs/DebugTooling.md`
