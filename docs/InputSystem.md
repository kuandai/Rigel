# Input System

This document describes the input system as implemented in Rigel. It covers
action bindings, runtime key state tracking, mouse look, and event dispatch.

## Overview

- Key bindings are loaded as an asset from `assets/manifest.yaml`.
- Actions are named strings (for example: `move_forward`, `debug_overlay`).
- Each action maps to a single key or is explicitly unbound.
- `InputDispatcher` exposes polling helpers and listener callbacks.
- GLFW callbacks feed a double-buffered key state (`keypress`).

## Core Components

### InputBindings
- Stores `action -> optional key` mappings.
- Supports bind/unbind/lookup operations.
- Stored as an asset (`InputBindings` derives from `AssetBase`).

### InputBindingsLoader
- Loader for the `input` manifest category.
- Parses `bindings` as a map of `action -> key name`.
- Accepts common key names and function keys (`F1`..`F25`).
- Treats `none`, `unbound`, `null`, or `~` as unbound.

### InputDispatcher
- Holds a shared `InputBindings`.
- Emits `onActionPressed` / `onActionReleased` events per frame.
- Provides polling helpers:
  - `isActionPressed`
  - `isActionJustPressed`
  - `isActionJustReleased`

### InputListener
- Optional interface for action-based callbacks.
- Used by `Application` to toggle the debug overlay.

### WindowState / CameraState
- `WindowState` tracks cursor capture, last mouse position, focus, and time reset.
- `CameraState` holds position + yaw/pitch used for mouse look updates.

## Manifest Configuration

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
        debug_overlay: F1
        imgui_overlay: F3
        debug_toggle_near_terrain: F4
        toggle_mouse_capture: TAB
        unbound_action: none
```

## Default Bindings

If a binding is missing from the manifest, `Input::ensureDefaultBindings`
applies defaults (only when an action is not defined). Explicitly unbound
actions remain unbound.

Defaults currently include:
- `move_forward`: W
- `move_backward`: S
- `move_left`: A
- `move_right`: D
- `move_up`: SPACE
- `move_down`: LCTRL
- `sprint`: LSHIFT
- `debug_overlay`: F1
- `imgui_overlay`: F3
- `debug_toggle_near_terrain`: F4
- `toggle_mouse_capture`: TAB
- `demo_spawn_entity`: F2

## Key Parsing Rules

- Case-insensitive; spaces/dashes are normalized.
- Single letters map to `A`-`Z`, digits map to `0`-`9`.
- Function keys `F1`..`F25` are supported.
- Common names are supported: `SPACE`, `ENTER`, `ESC`, `TAB`,
  `UP`, `DOWN`, `LEFT`, `RIGHT`, `LSHIFT`, `LCTRL`, `LALT`, etc.
- Unbinding: `none`, `unbound`, `null`, `~`.
- Unknown names log a warning and result in an unbound action.

## Runtime Flow

1) `AssetManager` loads `manifest.yaml`.
2) `InputBindingsLoader` loads `input/default` (if present).
3) `Application` attaches the bindings to `InputDispatcher`.
4) Each frame:
   - `keyupdate()` updates key state buffers.
   - `InputDispatcher::update()` emits action press/release events.
   - Systems can poll action states via `isActionPressed` etc.

## Mouse Look and Cursor Capture

- The GLFW cursor callback updates `CameraState.yaw` and `CameraState.pitch`.
- Mouse look is only applied when `WindowState.cursorCaptured` is true.
- `setCursorCaptured()` toggles:
  - `GLFW_CURSOR_DISABLED` and optional raw mouse motion.
  - `WindowState.firstMouse` to avoid jumps on capture.

## Direct Mouse Buttons

Mouse buttons are currently handled outside the action system (e.g. block
placement/removal uses `glfwGetMouseButton`). There are no manifest-defined
mouse bindings yet.

## Integration Notes

- Bindings are optional; actions can be fully unbound.
- Defaults only apply when an action is missing (not when explicitly unbound).
- The system is keyboard-focused; mouse buttons are handled directly.

## Extending (Not Implemented Yet)

- Allow multiple keys per action (array values in the manifest).
- Support mouse buttons and axis inputs.
- Add context-based bindings (e.g., menu vs gameplay).
- Hot-reload bindings in development builds.

---

## Related Docs

- `docs/ApplicationLifecycle.md`
- `docs/DebugTooling.md`
- `docs/AssetSystem.md`
