# WorldSet and World Ownership

This document describes the implemented ownership model for worlds, views,
shared voxel resources, and persistence context. `WorldSet` can contain
multiple worlds, while the application currently creates and runs one default
world and one view.

## Core Types

### WorldSet

`WorldSet` is the container and lookup point for world entries. Each entry owns
one `World` and optionally one `WorldView`:

```cpp
struct WorldEntry {
    World world;
    std::unique_ptr<WorldView> view;
};
```

The implemented API includes:

- `createWorld(id)`, which creates or returns a world.
- `createView(id, assets)`, which creates the world if necessary and initializes
  its single view.
- `world(id)`, `view(id)`, and `findView(id)` for lookup.
- `clear()` for teardown-only destruction of every view and world.

Before calling `clear()`, callers must detach each view's streaming callbacks,
stop its asynchronous chunk loader, and clear the active view. `clear()` then
destroys all views before the worlds and chunk managers to which they are
bound. There is no per-world destruction operation.

`WorldSet` also owns the shared `WorldResources`, persistence format registry,
persistence service, storage backend, configured preferred format, and root
path. Each world entry retains its active persistence format after bootstrap
resolves it.

### WorldResources

One `WorldResources` instance is shared by every world in a `WorldSet`. It owns:

- `BlockRegistry`
- `TextureAtlas`

Block definitions and atlas textures therefore have set-wide ownership rather
than per-world ownership.

### World

`World` owns authoritative simulation and persistence-facing state:

- `WorldId`
- `ChunkManager` and its block data
- `WorldEntities`
- A shared `WorldGenerator`
- A persistence `ProviderRegistry`

It provides block access and entity ticking. It does not own streaming state,
meshes, render configuration, shaders, or GPU resources.

### WorldView

`WorldView` refers to one `World` and the set's `WorldResources`. It owns the
derived and renderer-facing state for that world:

- `ChunkStreamer`
- `WorldMeshStore` CPU meshes
- `ChunkRenderer` and its GPU mesh/shadow cache
- `EntityRenderer`
- `WorldRenderConfig`
- Voxel and shadow shader handles

The `World` installs its generator once from the save-owned creation inputs and
rejects a divergent replacement. `WorldView::setGenerator()` binds streaming
to that world-owned generator and rejects a different one. The chunk manager,
mesh store, block registry, and texture atlas remain fixed for the lifetime of
the view.

## Application Wiring

The current application path uses `WorldSet::defaultWorldId()` and stores
pointers to that world and view as the active pair. It then:

1. Initializes the set-wide block registry and texture atlas.
2. Creates the active `World` and its `WorldView`.
3. Loads or durably publishes save-owned world settings and the generator
   snapshot, then configures the world generator and persistence providers.
4. Wires the view to the asynchronous chunk loader.
5. Assigns streaming and render configuration to the view.
6. Updates and renders only that active pair in the main loop.

There is no runtime world switching or simultaneous multi-view rendering in
`Application`.

## Persistence and Configuration

`WorldSet::persistenceContext(id)` uses the selected world's provider registry
and active format, plus the root path and storage backend from the `WorldSet`.
Before a format has been resolved, it uses the set-wide configured preference.
The application configures those shared values for its active default world
before loading or saving it. The root used by that boot path is
`saves/world_<id>`.

The subsystem bootstrap functions accept a world ID and include these optional
highest-precedence files for that ID:

- `config/worlds/<worldId>/streaming.yaml`
- `config/worlds/<worldId>/render.yaml`
- `config/worlds/<worldId>/persistence.yaml`

Configuration values are loaded and applied by the application and subsystem
providers; they are not stored as a general configuration object on `World` or
`WorldSet`. The streaming override file contributes only streaming policy.
Each published save owns `world-settings.yaml` and
`generator-definition.yaml`, and reload does not enumerate installed generator
definitions.

## Current Limitations

- `WorldSet` stores at most one `WorldView` for each world.
- `Application` creates only the default world and view.
- Persistence root, configured format preference, and storage are set-wide;
  resolved active formats and provider registries are per-world.
- GPU caches belong to each view's `ChunkRenderer`; no shared GPU cache exists
  outside a view.

---

## Related Docs

- `docs/ApplicationLifecycle.md`
- `docs/VoxelEngine.md`
- `docs/WorldGeneration.md`
- `docs/PersistenceAPI.md`
- `docs/ConfigurationSystem.md`
