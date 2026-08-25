# Input System

This document describes the input state owned by each Rigel application,
manifest-defined keyboard actions, cursor-driven camera look, and direct mouse
button interactions.

## Overview

- `Application::Impl` owns one `InputState`; device state is not shared between
  application instances.
- GLFW key and mouse-button callbacks write transitions into that instance's
  pending state.
- `InputState::beginFrame()` publishes the pending device state, clears consumed
  edge flags for the next frame, and notifies action listeners.
- Gameplay reads held and edge-triggered keyboard actions from `InputState`.
- Block edits read mouse-button press edges from the same state.

## Core Components

### InputBindings

- Stores `action -> optional key` mappings.
- Supports bind, unbind, lookup, and action enumeration.
- Is loaded as an `InputBindings` asset from the `input` manifest category.

### InputBindingsLoader

- Parses `bindings` as a map of action names to key names.
- Accepts common key names and function keys (`F1` through `F25`).
- Treats `none`, `unbound`, `null`, or `~` as explicitly unbound.

### InputState

- Owns the current and pending key and mouse-button arrays.
- Accepts callback events through `handleKeyEvent()` and
  `handleMouseButtonEvent()`.
- Exposes direct key queries and action-mapped queries.
- Notifies registered `InputListener` instances when bound actions are pressed
  or released.

### WindowState and CameraState

- `WindowState` tracks the GLFW window, cursor capture, last cursor position,
  focus, and frame-time reset state.
- `CameraState` holds position, orientation vectors, yaw, pitch, and movement
  settings.
- `InputCallbackContext` points GLFW callbacks at the application-owned input,
  window, and camera state.

## Manifest Configuration

Declare keyboard bindings in the asset manifest:

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
        toggle_mouse_capture: TAB
        debug_overlay: F1
        imgui_overlay: F3
        unbound_action: none
```

If `input/default` is absent, the application creates an empty binding set.
`ensureDefaultBindings()` then adds defaults only for actions that are missing;
an explicitly unbound action remains unbound.

Defaults currently include:

- `move_forward`: W
- `move_backward`: S
- `move_left`: A
- `move_right`: D
- `move_up`: Space
- `move_down`: Left Control
- `sprint`: Left Shift
- `toggle_mouse_capture`: Tab
- `debug_overlay`: F1
- `imgui_overlay`: F3
- `demo_spawn_entity`: F2

## Key Parsing Rules

- Parsing is case-insensitive, and spaces and dashes are normalized.
- Single letters map to `A` through `Z`; digits map to `0` through `9`.
- Function keys `F1` through `F25` are supported.
- Common names include `SPACE`, `ENTER`, `ESC`, `TAB`, direction keys, and
  left/right Shift, Control, Alt, and Super keys.
- Unknown names produce a warning and leave the action unbound.

## Per-Frame Lifecycle

1. `glfwPollEvents()` invokes the registered callbacks. Key and mouse-button
   callbacks update the pending arrays without changing the state visible to
   gameplay in the current frame.
2. `InputState::beginFrame()` copies pending state to the current arrays.
   Press and release edges are visible for that frame. Held and repeat state is
   retained in the pending arrays until release.
3. During `beginFrame()`, bound key edges invoke `InputListener` press and
   release callbacks.
4. The application and gameplay helpers query the published state for camera
   movement, cursor capture, entity spawning, block edits, and overlays.

The main action queries are:

- `isActionPressed()` for held movement and sprint actions.
- `isActionJustPressed()` for one-frame actions such as cursor capture and demo
  entity spawning.
- `isActionJustReleased()` for release-triggered behavior.

The debug overlay and profiler window listeners toggle their state on action
release. They are bound independently to `debug_overlay` and `imgui_overlay`.

## Mouse Look and Cursor Capture

- The cursor-position callback updates camera yaw and pitch while the cursor is
  captured. The first sample after capture or a warp is stored as the origin
  and does not rotate the camera.
- `setCursorCaptured()` hides the cursor for look. On platforms where GLFW 3.3
  honors `GLFW_CURSOR_DISABLED`, that mode and raw mouse motion are used. On
  macOS, GLFW can report a disabled cursor without confining it, so capture
  falls back to `GLFW_CURSOR_HIDDEN` and pins the pointer to the window center.
- `maintainCursorLock()` re-applies that confinement each frame so the cursor
  cannot walk off-screen while looking around.
- The `toggle_mouse_capture` action changes capture state once on its key press
  edge.
- Focus changes reset frame timing; regaining focus restores capture when it is
  active. ImGui is configured not to change the GLFW cursor while gameplay owns
  it.

## Mouse Buttons

Mouse buttons are not action-bound. The GLFW mouse-button callback records
their state in `InputState`, and `isMouseButtonJustPressed()` exposes a
one-frame press edge. While the cursor is captured, left press removes the
raycast block and right press places a block. Holding a button does not repeat
the edit on later frames.

## Current Limitations

- Each action maps to at most one keyboard key.
- Mouse buttons and axes cannot be declared as actions in the manifest.
- Bindings are loaded during application bootstrap and are not hot-reloaded.

---

## Related Docs

- `docs/ApplicationLifecycle.md`
- `docs/DebugTooling.md`
- `docs/AssetSystem.md`
