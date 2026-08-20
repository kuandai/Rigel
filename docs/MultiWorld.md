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
- `removeWorld(id)` and `clear()` for destruction.

`WorldSet` also owns the shared `WorldResources`, persistence format registry,
persistence service, storage backend, preferred format, policies, and root
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

`WorldView::setGenerator()` binds the streaming pipeline to the supplied
generator. The application assigns the same generator to both the `World` and
its `WorldView` during bootstrap.

## Application Wiring

The current application path uses `WorldSet::defaultWorldId()` and stores
pointers to that world and view as the active pair. It then:

1. Initializes the set-wide block registry and texture atlas.
2. Creates the active `World` and its `WorldView`.
3. Configures the world generator and persistence providers.
4. Wires the view to the asynchronous chunk loader.
5. Assigns streaming and render configuration to the view.
6. Updates and renders only that active pair in the main loop.

There is no runtime world switching or simultaneous multi-view rendering in
`Application`.

## Persistence and Configuration

`WorldSet::persistenceContext(id)` uses the selected world's provider registry,
but copies the root path, preferred format, policies, and storage backend from
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
`WorldSet`.

## Current Limitations

- `WorldSet` stores at most one `WorldView` for each world.
- `Application` creates only the default world and view.
- Persistence root, preferred format, policies, and storage are set-wide even
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
