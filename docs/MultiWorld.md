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
persistence service, storage backend, preferred format, and root
path.

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

`WorldView::setGenerator()` replaces the streaming pipeline's generator. The
chunk manager, mesh store, block registry, and texture atlas remain fixed for
the lifetime of the view. The application assigns the same generator to both
the `World` and its `WorldView` during bootstrap.

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

`WorldSet::persistenceContext(id)` uses the selected world's provider registry,
but copies the root path, preferred format, and storage backend from
the `WorldSet`. The application configures those shared values for its active
default world before loading or saving it. The root used by that boot path is
`saves/world_<id>`.

The subsystem bootstrap functions accept a world ID and include these optional
highest-precedence files for that ID:

- `config/worlds/<worldId>/world_generation.yaml`
- `config/worlds/<worldId>/render.yaml`
- `config/worlds/<worldId>/persistence.yaml`

Configuration values are loaded and applied by the application and subsystem
providers; they are not stored as a general configuration object on `World` or
`WorldSet`. Generation fields are creation inputs only. Each published save
owns `world-settings.yaml` and `generator-definition.yaml`, and reload bypasses
installed generation fields while still loading streaming policy.

## Current Limitations

- `WorldSet` stores at most one `WorldView` for each world.
- `Application` creates only the default world and view.
- Persistence root, preferred format, and storage are set-wide even
  though provider registries are per-world.
- GPU caches belong to each view's `ChunkRenderer`; no shared GPU cache exists
  outside a view.

---

## Related Docs

- `docs/ApplicationLifecycle.md`
- `docs/VoxelEngine.md`
- `docs/WorldGeneration.md`
- `docs/PersistenceAPI.md`
- `docs/ConfigurationSystem.md`
