# WorldSet and World Ownership

This document describes the implemented ownership model for worlds, views,
shared semantic resources, and persistence context. `WorldSet` can contain
multiple worlds, while the graphical application owns one view for its active
default world.

## Core Types

### WorldSet

`WorldSet` is the container and lookup point for semantic world entries. Each
entry owns one `World`:

```cpp
struct WorldEntry {
    World world;
};
```

The implemented API includes:

- `createWorld(id)`, which creates or returns a world.
- `world(id)` for lookup.
- `clear()` for teardown-only destruction of every world.

Before calling `clear()`, graphical callers must detach each view's streaming
callbacks, stop its asynchronous chunk loader, and destroy the view. There is
no per-world destruction operation.

`WorldSet` also owns the shared `WorldResources`, persistence format registry,
persistence service, storage backend, configured preferred format, and root
path. Each world entry retains its active persistence format after bootstrap
resolves it.

### WorldResources

One `WorldResources` instance is shared by every world in a `WorldSet`. It owns:

- `BlockRegistry`

Block definitions therefore have set-wide semantic ownership. GPU texture
atlases are not part of `WorldResources`.

### World

`World` owns authoritative simulation and persistence-facing state:

- `WorldId`
- `ChunkManager` and its block data
- `WorldEntities`
- A shared `WorldGenerator`
- A persistence `ProviderRegistry`

It provides block access and entity ticking. It does not own streaming state,
meshes, render policy, shaders, or GPU resources.

### WorldView

`WorldView` refers to one `World` and the set's `WorldResources`. It owns the
derived and renderer-facing state for that world:

- `ChunkStreamer`
- `WorldMeshStore` CPU meshes
- `ChunkRenderer` and its GPU mesh/shadow cache
- `EntityRenderer`
- A `TextureAtlas` derived from the shared registry's logical texture paths
- The shipped internal `RenderProfile`
- Voxel and shadow shader handles

The `World` installs its generator once from the save-owned creation inputs and
rejects a divergent replacement. `WorldView::setGenerator()` binds streaming
to that world-owned generator and rejects a different one. The chunk manager,
mesh store, block registry, and texture atlas remain fixed for the lifetime of
the view.

## Application Wiring

The current application path uses `WorldSet::defaultWorldId()` and stores
pointers to that world and view as the active pair. It then:

1. Initializes the set-wide block registry.
2. Creates the active `World`, then constructs and initializes its owned
   `WorldView`.
3. Loads or durably publishes save-owned world settings and the generator
   snapshot, then configures the world generator and persistence providers.
4. Wires the view to the asynchronous chunk loader.
5. Applies automatic streaming policy; the view retains shipped render policy.
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

Installed persistence policy selects CR for new saves. Existing worlds resolve
their authoritative saved format marker; there is no bare-root, `config/`, or
numeric per-world persistence source. Renderer and streaming policy are also
internal and have no per-world files. Configuration values are not stored as a
general configuration object on `World` or `WorldSet`. Each published save owns
`world-settings.yaml` and `generator-definition.yaml`, and reload does not
enumerate installed generator definitions.

## Current Limitations

- `Application` creates only the default world and view.
- Persistence root, configured format preference, and storage are set-wide;
  resolved active formats and provider registries are per-world.
- GPU caches and the texture atlas belong to each view; no GPU cache exists in
  `WorldSet` or `WorldResources`.

---

## Related Docs

- `docs/ApplicationLifecycle.md`
- `docs/VoxelEngine.md`
- `docs/WorldGeneration.md`
- `docs/PersistenceAPI.md`
- `docs/ConfigurationSystem.md`
