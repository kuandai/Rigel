# Rigel Documentation

Rigel is a voxel engine prototype focused on world generation, chunk streaming,
rendering, and persistence. The docs mix current implementation details and
forward-looking design notes; sections labeled `planned` are not yet runtime
behavior.

## Where to Start

- `docs/VoxelEngine.md` for a high-level architecture overview.
- `docs/ApplicationLifecycle.md` for the runtime flow and async paths.
- `docs/ConfigurationSystem.md` for how configs are loaded and overridden.

## Major Components

### Core Runtime

- `docs/ApplicationLifecycle.md` (bootstrap, main loop, shutdown)
- `docs/InputSystem.md` (bindings, dispatcher, mouse look)
- `docs/DebugTooling.md` (chunk visualizer, frame graph, entity bounds)

### Voxel + World

- `docs/WorldGeneration.md` (pipeline, streaming, overlays)
- `docs/RenderingPipeline.md` (voxel rendering, TAA, shadows)
- `docs/VoxelSvoBenchmarks.md` (chunk-only vs voxel-SVO comparison sheet)
- `docs/ShaderSystem.md` (shader assets, compilation, defines)
- `docs/VoxelEngine.md` (block and chunk structures)

### Entities

- `docs/EntitySystem.md` (runtime, rendering, models, persistence)

### Persistence

- `docs/PersistenceAPI.md` (format-agnostic save/load)
- `docs/PersistenceBackends.md` (CR and memory backends)
- `docs/MultiWorld.md` (world ownership and persistence context)

### Assets + Configuration

- `docs/AssetSystem.md` (manifest and loader behavior)
- `docs/EmbeddedAssets.md` (resource embedding)
- `docs/ConfigurationSystem.md` (config sources and precedence)

### Testing

- `docs/TestFramework.md` (in-tree harness and CTest wiring)

## Repository Layout (Key Paths)

- `src/`: engine code
- `include/`: public headers
- `assets/`: embedded assets and configs
- `tests/`: unit tests
- `docs/`: system documentation

## Scope and Status

The project is in active development.

Authoritative runtime references (implementation-first):

- `docs/ApplicationLifecycle.md`
- `docs/WorldGeneration.md`
- `docs/RenderingPipeline.md`
- `docs/InputSystem.md`
- `docs/PersistenceAPI.md`

Other pages may include planned architecture and future hooks.
