# Rigel Documentation

Rigel is a voxel engine prototype focused on world generation, chunk streaming,
rendering, and persistence. This documentation describes the current
implementation and how the systems fit together.

## Where to Start

- `docs/VoxelEngine.md` for a high-level architecture overview.
- `docs/ApplicationLifecycle.md` for the runtime flow and async paths.
- `docs/ConfigurationSystem.md` for configuration ownership, persistence, and
  runtime mutability.

## Major Components

### Core Runtime

- `docs/ApplicationLifecycle.md` (bootstrap, main loop, shutdown)
- `docs/InputSystem.md` (per-application device state, bindings, mouse look)
- `docs/DebugTooling.md` (chunk visualizer, frame graph, entity bounds)

### Voxel + World

- `docs/BlockTargeting.md` (model-surface raycasts, exact hits, and selection
  outlines)
- `docs/BlockCollision.md` (normalized physical shapes, world queries, and entity sweeps)
- `docs/BlockGallery.md` (developer launch, navigation, catalog, and visual review)
- `docs/WorldGeneration.md` (strict graph generation, pipeline, streaming)
- `docs/RenderingPipeline.md` (voxel rendering, TAA, shadows)
- `docs/ShaderSystem.md` (shader assets and compilation)
- `docs/VoxelEngine.md` (block models, meshing, and chunk structures)

### Entities

- `docs/EntitySystem.md` (runtime, static-world collision, rendering, models, persistence)

### Persistence

- `docs/PersistenceAPI.md` (format-agnostic save/load)
- `docs/PersistenceBackends.md` (CR and memory backends)
- `docs/MultiWorld.md` (world ownership and persistence context)

### Assets + Configuration

- `docs/AssetSystem.md` (manifest, normalized block models, and loader behavior)
- `docs/EmbeddedAssets.md` (resource embedding)
- `docs/AssetOwnership.md` (Git/JAR/generated-content ownership and workflow)
- `docs/CosmicReachImportParity.md` (real-JAR migration evidence)
- `docs/ConfigurationSystem.md` (configuration ownership and lifetimes)

### Testing

- `docs/TestFramework.md` (in-tree harness and CTest wiring)

## Repository Layout (Key Paths)

- `src/`: engine code
- `include/`: public headers
- `assets/`: tracked Rigel-owned embedded assets and configs
- `.rigel/`: ignored staged/imported Cosmic Reach content
- `tests/`: unit tests
- `docs/`: system documentation

## Scope and Status

The project is in active development. These documents describe implemented
behavior and call out current limitations where relevant.
