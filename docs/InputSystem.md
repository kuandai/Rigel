# Input System

This document describes Rigel's input system, focusing on configurable
key bindings loaded from the asset manifest and the dispatcher used to
query actions or broadcast input events.

Overview
--------
- Key bindings are loaded as an asset from `assets/manifest.yaml`.
- Actions are named strings (e.g., `move_forward`, `debug_overlay`).
- Each action is mapped to a single key or left unbound.
- `InputDispatcher` provides per-frame action queries and event callbacks.
- `Application` uses actions instead of hardcoded keycodes.

Key Components
--------------
1) InputBindings
   - Stores the mapping of action -> optional key.
   - Supports binding, unbinding, and querying keys for actions.
   - Acts as a shared asset (`InputBindings` derives from `AssetBase`).

2) InputBindingsLoader
   - Asset loader for the `input` manifest category.
   - Parses `bindings` as a map of action -> key name.
   - Supports common key names (letters, digits, function keys, arrows).
   - Supports unbinding via `none`, `unbound`, `null`, or `~`.

3) InputDispatcher
   - Holds a shared `InputBindings`.
   - Can dispatch action events to listeners on press/release.
   - Provides `isActionPressed`, `isActionJustPressed`,
     and `isActionJustReleased` queries for polling.

4) InputListener
   - Optional interface for action-based callbacks.
   - `Application` uses a listener to toggle the debug overlay.

Manifest Configuration
----------------------
Declare bindings in the asset manifest:

```yaml
assets:
  input:
    default:
      bindings:
        move_forward: W
        move_backward: S
        move_left: A
        move_right: D
        move_up: SPACE
        move_down: LCTRL
        sprint: LSHIFT
        exit: ESC
        debug_overlay: F1
        unbound_action: none
```

Key Parsing Rules
-----------------
- Case-insensitive; spaces/dashes are normalized.
- Single letters map to `A`-`Z`, digits map to `0`-`9`.
- Function keys `F1`..`F25` are supported.
- Common names are supported: `SPACE`, `ENTER`, `ESC`, `TAB`,
  `UP`, `DOWN`, `LEFT`, `RIGHT`, `LSHIFT`, `LCTRL`, `LALT`, etc.
- Unbinding: `none`, `unbound`, `null`, `~`.
- Unknown names log a warning and result in an unbound action.

Runtime Flow
------------
1) `AssetManager` loads `manifest.yaml`.
2) `InputBindingsLoader` loads `input/default` (if present).
3) `Application` attaches the bindings to `InputDispatcher`.
4) Each frame:
   - `keyupdate()` updates key state buffers.
   - `InputDispatcher::update()` emits action press/release events.
   - Systems can poll action states via `isActionPressed` etc.

Integration Notes
-----------------
- Bindings are optional; actions can be fully unbound.
- Defaults are applied only when an action is missing, not when it is
  explicitly unbound.
- The system is currently keyboard-only; mouse bindings are not yet supported.

Extending
---------
Potential extensions (not implemented yet):
- Allow multiple keys per action (array values in the manifest).
- Support mouse buttons and axis inputs.
- Add context-based bindings (e.g., menu vs gameplay).
- Hot-reload bindings in development builds.
